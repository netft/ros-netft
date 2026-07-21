#include "support/fake_sensor.hpp"

#include "netft_driver/protocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using netft_driver::Command;
using netft_driver::test::FakeSensor;

struct CommandResult {
  int exit_code{};
  std::string stdout_text;
  std::string stderr_text;
};

std::string quote(const std::string & value)
{
  std::string result{"'"};
  for (const auto character : value) {
    if (character == '\'') result += "'\\''";
    else result += character;
  }
  return result + "'";
}

CommandResult run_check(const std::string & arguments)
{
  const auto base = std::filesystem::temp_directory_path() /
                    ("netft-check-" + std::to_string(::getpid()));
  const auto output = base.string() + ".out";
  const auto error = base.string() + ".err";
  const auto command = quote(NETFT_CHECK_PATH) + " " + arguments +
                       " > " + quote(output) + " 2> " + quote(error);
  const int status = std::system(command.c_str());
  std::ifstream stdout_stream{output}, stderr_stream{error};
  std::ostringstream captured_stdout, captured_stderr;
  captured_stdout << stdout_stream.rdbuf();
  captured_stderr << stderr_stream.rdbuf();
  std::filesystem::remove(output); std::filesystem::remove(error);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : 127,
          captured_stdout.str(), captured_stderr.str()};
}

void expect_strict_json(const std::string & text, std::uint64_t warning_count = 0,
                        bool serious_observed = false,
                        std::optional<std::uint64_t> duplicate_count = std::nullopt,
                        std::optional<std::uint64_t> out_of_order_count = std::nullopt)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("netft-check-json-" + std::to_string(::getpid()));
  { std::ofstream stream{path}; stream << text; }
  const std::string script = std::string{
      "import json,sys; "
      "payload=json.load(open(sys.argv[1]), parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value))); "
      "required={'device_error_count','device_status','duplicate_count','elapsed_s','endpoint','last_force','last_ft_sequence','last_rdt_sequence','last_torque','lost_count','malformed_count','out_of_order_count','receive_rate_hz','reconnect_count','requested_duration_s','sample_count','timeout_count','warning_count'}; "
      "assert set(payload) == required; assert isinstance(payload['sample_count'], int); assert isinstance(payload['device_status'], str); "} +
      "assert payload['warning_count'] == " + std::to_string(warning_count) + "; " +
      (duplicate_count ? "assert payload['duplicate_count'] == " + std::to_string(*duplicate_count) + "; " : "") +
      (out_of_order_count ? "assert payload['out_of_order_count'] == " + std::to_string(*out_of_order_count) + "; " : "") +
      (serious_observed ? "assert payload['device_error_count'] > 0" : "assert payload['device_error_count'] == 0");
  const int status = std::system(("python3 -c " + quote(script) + " " + quote(path.string())).c_str());
  std::filesystem::remove(path);
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

std::string endpoint_arguments(const FakeSensor & sensor)
{
  return "--host " + sensor.host() + " --port " + std::to_string(sensor.port()) +
         " --duration 0.15 --receive-timeout 0.05";
}

void expect_safe_shutdown(const FakeSensor & sensor, bool exact = false)
{
  const auto commands = sensor.commands();
  ASSERT_FALSE(commands.empty());
  EXPECT_EQ(commands.back(), Command::StopStreaming);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), Command::SetSoftwareBias), 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), Command::ResetConditionLatch), 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), Command::StartBuffered), 0);
  if (exact) EXPECT_EQ(commands, (std::vector<Command>{Command::StartRealtime, Command::StopStreaming}));
}

TEST(NetFTCheck, WritesStrictJsonWithIndependentScalesAndStopsWithoutBias)
{
  const auto destination = std::filesystem::temp_directory_path() /
                           ("netft-check-" + std::to_string(::getpid()) + ".json");
  std::filesystem::remove(destination);
  FakeSensor sensor{200};
  const auto result = run_check(endpoint_arguments(sensor) +
      " --counts-per-force 100 --counts-per-torque 10 --output " + destination.string() + " --json");
  expect_safe_shutdown(sensor, true);
  std::ifstream output{destination}; std::ostringstream json; json << output.rdbuf();
  std::filesystem::remove(destination);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stderr_text.empty());
  EXPECT_EQ(result.stdout_text, json.str());
  expect_strict_json(result.stdout_text);
  expect_strict_json(json.str());
}

TEST(NetFTCheck, SeriousAndWarningRecordsReturnEvidenceExitCode)
{
  for (const auto status : {0x80020000U, 0x80010000U}) {
    FakeSensor sensor{10};
    sensor.queue_record(55, status);
    const auto result = run_check(endpoint_arguments(sensor));
    expect_safe_shutdown(sensor);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_TRUE(result.stderr_text.empty());
    expect_strict_json(result.stdout_text, status == 0x80010000U ? 1 : 0, status == 0x80020000U);
  }
}

TEST(NetFTCheck, InvalidDurationAndOutputFailureDoNotPublishPartialJson)
{
  FakeSensor sensor;
  const auto invalid = run_check("--host " + sensor.host() + " --port " +
      std::to_string(sensor.port()) + " --duration nan");
  EXPECT_EQ(invalid.exit_code, 2);
  EXPECT_TRUE(invalid.stdout_text.empty());
  EXPECT_NE(invalid.stderr_text.find("duration must be finite and greater than zero"), std::string::npos);
  EXPECT_TRUE(sensor.commands().empty());

  const auto directory = std::filesystem::temp_directory_path() /
                         ("netft-check-dir-" + std::to_string(::getpid()));
  std::filesystem::create_directory(directory);
  const auto failed_output = run_check(endpoint_arguments(sensor) + " --output " + directory.string());
  expect_safe_shutdown(sensor, true);
  EXPECT_EQ(failed_output.exit_code, 2);
  EXPECT_TRUE(std::filesystem::is_directory(directory));
  std::filesystem::remove(directory);
}

TEST(NetFTCheck, MalformedAndSilentStreamsFailAfterFirstRecordTimeoutAndStop)
{
  for (const bool malformed : {true, false}) {
    FakeSensor sensor{0.5};
    if (malformed) sensor.queue_payload({'b', 'a', 'd'}); else sensor.pause();
    const auto result = run_check(endpoint_arguments(sensor));
    expect_safe_shutdown(sensor);
    EXPECT_EQ(result.exit_code, 2);
    EXPECT_TRUE(result.stdout_text.empty());
    EXPECT_NE(result.stderr_text.find("no Net F/T sample received"), std::string::npos);
  }
}

TEST(NetFTCheck, WarningObservedBeforeHealthyRecordsStillReturnsEvidenceExitCode)
{
  FakeSensor sensor{200};
  sensor.queue_record(55, 0x80010000U);
  const auto result = run_check(endpoint_arguments(sensor));
  expect_safe_shutdown(sensor);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(result.stderr_text.empty());
  expect_strict_json(result.stdout_text, 1);
}

TEST(NetFTCheck, DuplicateAndOutOfOrderWarningsRemainWindowEvidence)
{
  for (const auto & sequences : {std::array<std::uint32_t, 3>{1, 1, 2},
                                std::array<std::uint32_t, 3>{10, 9, 11}}) {
    FakeSensor sensor{200};
    sensor.queue_record(sequences[0]);
    sensor.queue_record(sequences[1], 0x80010000U);
    sensor.queue_record(sequences[2]);
    const auto result = run_check(endpoint_arguments(sensor));
    expect_safe_shutdown(sensor);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_TRUE(result.stderr_text.empty());
    expect_strict_json(result.stdout_text, 1);
  }
}

TEST(NetFTCheck, SeriousObservedBeforeHealthyRecordsStillReturnsEvidenceExitCode)
{
  FakeSensor sensor{200};
  sensor.queue_record(55, 0x80020000U);
  const auto result = run_check(endpoint_arguments(sensor));
  expect_safe_shutdown(sensor);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(result.stderr_text.empty());
  expect_strict_json(result.stdout_text, 0, true);
}

TEST(NetFTCheck, HelpWritesUsageToStdoutAndSucceeds)
{
  const auto result = run_check("--help");
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stderr_text.empty());
  EXPECT_NE(result.stdout_text.find("usage: netft_check"), std::string::npos);
}

TEST(NetFTCheck, InterruptionStopsTheStreamWithoutBias)
{
  FakeSensor sensor{200};
  const auto output = std::filesystem::temp_directory_path() /
                      ("netft-check-interrupt-" + std::to_string(::getpid()) + ".out");
  const auto command = "timeout --preserve-status -s INT 0.10 " + quote(NETFT_CHECK_PATH) + " " +
      endpoint_arguments(sensor) + " > " + quote(output.string()) + " 2>&1";
  const int status = std::system(command.c_str());
  std::filesystem::remove(output);
  expect_safe_shutdown(sensor, true);
  EXPECT_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : 127, 2);
}
}  // namespace
