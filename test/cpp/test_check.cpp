#include "core/support/fake_sensor.hpp"

#include "detail/protocol.hpp"

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

#define main netft_check_embedded_main
#include "../../src/check_main.cpp"
#undef main

namespace {
using netft::detail::Command;
using netft::test::FakeSensor;

struct CommandResult {
  int exit_code{};
  std::string stdout_text;
  std::string stderr_text;
};

std::string quote(const std::string &value) {
  std::string result{"'"};
  for (const auto character : value) {
    if (character == '\'') {
      result += "'\\''";
    } else {
      result += character;
    }
  }
  return result + "'";
}

CommandResult run_check(const std::string &arguments) {
  static unsigned invocation{};
  const auto base = std::filesystem::temp_directory_path() /
                    ("netft-check-" + std::to_string(::getpid()) + "-" +
                     std::to_string(invocation++));
  const auto output = base.string() + ".out";
  const auto error = base.string() + ".err";
  const auto command = quote(NETFT_CHECK_PATH) + " " + arguments + " > " + quote(output) +
                       " 2> " + quote(error);
  const int status = std::system(command.c_str());
  std::ifstream stdout_stream{output};
  std::ifstream stderr_stream{error};
  std::ostringstream captured_stdout;
  std::ostringstream captured_stderr;
  captured_stdout << stdout_stream.rdbuf();
  captured_stderr << stderr_stream.rdbuf();
  std::filesystem::remove(output);
  std::filesystem::remove(error);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : 127, captured_stdout.str(),
          captured_stderr.str()};
}

void expect_json_assertions(const std::string &text, const std::string &assertions) {
  static unsigned invocation{};
  const auto path = std::filesystem::temp_directory_path() /
                    ("netft-check-json-" + std::to_string(::getpid()) + "-" +
                     std::to_string(invocation++));
  {
    std::ofstream stream{path};
    stream << text;
  }
  const std::string script =
      "import json,sys; "
      "payload=json.load(open(sys.argv[1]), "
      "parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value))); " +
      assertions;
  const int status =
      std::system(("python3 -c " + quote(script) + " " + quote(path.string())).c_str());
  std::filesystem::remove(path);
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

std::string endpoint_arguments(const FakeSensor &sensor) {
  return "--host " + sensor.host() + " --port " + std::to_string(sensor.rdt_port()) +
         " --http-port " + std::to_string(sensor.http_port()) +
         " --configuration-connect-timeout 0.25 --configuration-timeout 0.75" +
         " --duration 0.08 --receive-timeout 0.05";
}

void expect_safe_shutdown(const FakeSensor &sensor, bool exact = false) {
  const auto commands = sensor.commands();
  ASSERT_FALSE(commands.empty());
  EXPECT_EQ(commands.back(), Command::StopStreaming);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), Command::SetSoftwareBias), 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), Command::ResetConditionLatch), 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), Command::StartBuffered), 0);
  if (exact) {
    EXPECT_EQ(commands, (std::vector<Command>{Command::StartRealtime, Command::StopStreaming}));
  }
}

TEST(NetFTCheckOptions, MapsDiscoveryConfigAndLeavesCalibrationOverrideEmptyByDefault) {
  std::array<std::string, 7> arguments{
      "netft_check", "--http-port", "8080", "--configuration-connect-timeout", "0.25",
      "--configuration-timeout", "0.75"};
  std::array<char *, arguments.size()> argv{};
  std::transform(arguments.begin(), arguments.end(), argv.begin(),
                 [](std::string &argument) { return argument.data(); });

  const auto options = parse_options(static_cast<int>(argv.size()), argv.data());

  EXPECT_EQ(options.config.http_port, 8080);
  EXPECT_DOUBLE_EQ(options.config.configuration_connect_timeout.count(), 0.25);
  EXPECT_DOUBLE_EQ(options.config.configuration_timeout.count(), 0.75);
  EXPECT_FALSE(options.config.calibration_override.has_value());
}

TEST(NetFTCheck, DiscoversCalibrationOverHttpByDefaultAndMapsConfigurationOptions) {
  FakeSensor sensor{200};

  const auto result = run_check(endpoint_arguments(sensor));

  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stderr_text.empty());
  EXPECT_EQ(sensor.http_request_count(), 1U);
  expect_safe_shutdown(sensor, true);
  expect_json_assertions(result.stdout_text,
                         "assert payload['configuration_source'] == 'sensor'; "
                         "assert payload['force_unit'] == 'N'; "
                         "assert payload['torque_unit'] == 'N-m'; "
                         "assert payload['counts_per_force_unit'] == 1000000; "
                         "assert payload['counts_per_torque_unit'] == 1000000");
}

TEST(NetFTCheck, RequiresManualCountsAsACompletePairBeforeOpeningSockets) {
  for (const auto &manual_option : {std::string{"--counts-per-force 100"},
                                    std::string{"--counts-per-torque 10"}}) {
    FakeSensor sensor;

    const auto result = run_check(endpoint_arguments(sensor) + " " + manual_option);

    EXPECT_EQ(result.exit_code, 2);
    EXPECT_TRUE(result.stdout_text.empty());
    expect_json_assertions(result.stderr_text,
                           "assert payload['error'] == 'invalid_arguments'; "
                           "assert payload['exit_code'] == 2");
    EXPECT_EQ(sensor.http_request_count(), 0U);
    EXPECT_TRUE(sensor.commands().empty());
  }
}

TEST(NetFTCheck, CompleteManualCountsCreateNewtonAndNewtonMetreOverride) {
  FakeSensor sensor{200};
  sensor.queue_record(1, 0, 100, {100, -200, 300, 10, -20, 30});

  const auto result = run_check(endpoint_arguments(sensor) +
                                " --counts-per-force 100 --counts-per-torque 10");

  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stderr_text.empty());
  EXPECT_EQ(sensor.http_request_count(), 0U);
  expect_safe_shutdown(sensor, true);
  expect_json_assertions(result.stdout_text,
                         "assert payload['configuration_source'] == 'override'; "
                         "assert payload['force_unit'] == 'N'; "
                         "assert payload['torque_unit'] == 'N-m'; "
                         "assert payload['counts_per_force_unit'] == 100; "
                         "assert payload['counts_per_torque_unit'] == 10; "
                         "assert payload['last_force_n'] == [1, -2, 3]; "
                         "assert payload['last_torque_nm'] == [1, -2, 3]");
}

TEST(NetFTCheck, JsonNamesSIValuesExplicitlyAndPreservesNativeConfigurationMetadata) {
  FakeSensor sensor{200};
  sensor.set_xml_configuration(
      "<netft><prodname>Fake</prodname><cfgcpf>100</cfgcpf><cfgcpt>10</cfgcpt>"
      "<scfgfu>kN</scfgfu><scfgtu>N-mm</scfgtu></netft>");
  sensor.queue_record(1, 0, 100, {100, -200, 300, 10, -20, 30});

  const auto result = run_check(endpoint_arguments(sensor));

  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stderr_text.empty());
  const std::string required =
      "required={'configuration_source','force_unit','torque_unit',"
      "'counts_per_force_unit','counts_per_torque_unit','last_force_n','last_torque_nm'}; "
      "assert required <= set(payload); ";
  expect_json_assertions(result.stdout_text,
                         required +
                             "assert payload['configuration_source'] == 'sensor'; "
                             "assert payload['force_unit'] == 'kN'; "
                             "assert payload['torque_unit'] == 'N-mm'; "
                             "assert payload['last_force_n'] == [1000, -2000, 3000]; "
                             "assert payload['last_torque_nm'] == [0.001, -0.002, 0.003]");
}

TEST(NetFTCheck, WarningEvidenceUsesNumericExitCodeAndStructuredJson) {
  FakeSensor sensor{200};
  sensor.queue_record(1, 0x80010000U, 100);

  const auto result = run_check(endpoint_arguments(sensor));

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(result.stderr_text.empty());
  expect_json_assertions(result.stdout_text,
                         "assert payload['warning_count'] >= 1; "
                         "assert payload['device_error_count'] == 0");
}

TEST(NetFTCheck, InvalidArgumentsUseNumericExitCodeAndStructuredJsonError) {
  const auto result = run_check("--duration nan");

  EXPECT_EQ(result.exit_code, 2);
  EXPECT_TRUE(result.stdout_text.empty());
  expect_json_assertions(result.stderr_text,
                         "assert payload['error'] == 'invalid_arguments'; "
                         "assert payload['exit_code'] == 2");
}

TEST(NetFTCheck, RuntimeFailuresUseNumericExitCodeAndStructuredJsonError) {
  FakeSensor sensor{200};
  sensor.pause();

  const auto result = run_check(endpoint_arguments(sensor));

  EXPECT_EQ(result.exit_code, 2);
  EXPECT_TRUE(result.stdout_text.empty());
  expect_json_assertions(result.stderr_text,
                         "assert payload['error'] == 'check_failed'; "
                         "assert payload['exit_code'] == 2");
  expect_safe_shutdown(sensor);
}

TEST(NetFTCheck, HelpUsesTheSuccessExitCode) {
  const auto result = run_check("--help");
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.stderr_text.empty());
}

} // namespace
