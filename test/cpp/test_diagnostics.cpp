#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "netft_driver/status.hpp"

namespace {
using namespace netft_driver;

static_assert(!std::is_constructible<DiagnosticEvaluator, double, bool>::value,
  "DiagnosticEvaluator must reject boolean rate tolerance");
static_assert(std::is_constructible<DiagnosticEvaluator, double, double>::value,
  "DiagnosticEvaluator must accept a double rate tolerance");

HealthSnapshot healthy()
{
  HealthSnapshot snapshot;
  snapshot.state = ClientState::Streaming;
  snapshot.sensor_host = "192.168.31.100";
  snapshot.sensor_port = 49152;
  snapshot.last_rdt_sequence = 100;
  snapshot.last_ft_sequence = 400;
  snapshot.last_ft_progress = "forward";
  snapshot.receive_rate = snapshot.publish_rate = 2000.0;
  snapshot.received_count = snapshot.published_count = 2000;
  snapshot.last_record_age = std::chrono::duration<double>{0.001};
  return snapshot;
}

TEST(Diagnostics, PreservesTheCompletePythonKeyOrder)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  const auto report = evaluator.evaluate(healthy());
  std::vector<std::string> keys;
  for (const auto & [key, value] : report.values) {
    static_cast<void>(value);
    keys.push_back(key);
  }
  EXPECT_EQ(keys, kDiagnosticValueKeys);
}

TEST(Diagnostics, TimeoutHasPriorityOverFirstConnectingRecord)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.state = ClientState::Connecting;
  snapshot.timeout_count = 1;
  snapshot.reconnect_count = 1;
  EXPECT_EQ(evaluator.evaluate(snapshot).message, "receive timeout observed since last diagnostic");
  EXPECT_EQ(evaluator.evaluate(snapshot).message, "waiting for first RDT record");
}

TEST(Diagnostics, BackoffIsImmediatelyAnErrorWithoutCounterChanges)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  evaluator.evaluate(snapshot);
  snapshot.state = ClientState::Backoff;
  const auto report = evaluator.evaluate(snapshot);
  EXPECT_EQ(report.level, 2);
  EXPECT_EQ(report.message, "connection lost; reconnecting");
}

TEST(Diagnostics, FaultedStateAndFaultCodeRemainErrorsAcrossEveryEvaluation)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.state = ClientState::Faulted;
  snapshot.fault_code = FaultCode::Socket;
  snapshot.last_error = "socket receive failed";
  snapshot.timeout_count = 1;

  const auto first = evaluator.evaluate(snapshot);
  const auto second = evaluator.evaluate(snapshot);
  EXPECT_EQ(first.level, 2);
  EXPECT_EQ(second.level, 2);
  EXPECT_EQ(first.log_key, "faulted");
  EXPECT_EQ(second.log_key, "faulted");
  EXPECT_NE(first.message.find("socket receive failed"), std::string::npos);
  EXPECT_NE(second.message.find("socket receive failed"), std::string::npos);
  std::vector<std::string> keys;
  for (const auto & [key, value] : second.values) {
    static_cast<void>(value);
    keys.push_back(key);
  }
  EXPECT_EQ(keys, kDiagnosticValueKeys);
}

TEST(Diagnostics, PreservesValuesAndWarnsOnlyForNewLoss)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  const auto ok = evaluator.evaluate(snapshot);
  EXPECT_EQ(ok.level, 0);
  EXPECT_EQ(ok.message, "streaming normally");
  EXPECT_EQ(ok.values[5], (std::pair<std::string, std::string>{"device_status", "0x00000000"}));
  snapshot.lost_count = 3;
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 1);
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);
}

TEST(Diagnostics, WarnsForConditionLatchAndReceiveRateDeviation)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto condition = healthy();
  condition.last_status = 0x80010000U;
  const auto condition_report = evaluator.evaluate(condition);
  EXPECT_EQ(condition_report.level, 1);
  EXPECT_NE(condition_report.message.find("monitor condition latched"), std::string::npos);

  auto slow = healthy();
  slow.receive_rate = 1000.0;
  const auto slow_report = evaluator.evaluate(slow);
  EXPECT_EQ(slow_report.level, 1);
  EXPECT_NE(slow_report.message.find("receive rate"), std::string::npos);
}

TEST(Diagnostics, ErrorsForCurrentDeviceFault)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.last_status = 0x80020000U;
  const auto report = evaluator.evaluate(snapshot);
  EXPECT_EQ(report.level, 2);
  EXPECT_NE(report.message.find("transducer saturation"), std::string::npos);
}

TEST(Diagnostics, LatchesRecoveredDeviceErrorForExactlyOneCycle)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);
  snapshot.device_error_count = 1;
  const auto recovered = evaluator.evaluate(snapshot);
  const auto settled = evaluator.evaluate(snapshot);
  EXPECT_EQ(recovered.level, 2);
  EXPECT_NE(recovered.message.find("serious device status"), std::string::npos);
  EXPECT_EQ(settled.level, 0);
}

TEST(Diagnostics, UsesExactMalformedStormThresholdWithinOneWindow)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.malformed_count = kMalformedStormThreshold - 1;
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);
  snapshot.malformed_count = (2 * kMalformedStormThreshold) - 1;
  const auto storm = evaluator.evaluate(snapshot);
  const auto settled = evaluator.evaluate(snapshot);
  EXPECT_EQ(storm.level, 2);
  EXPECT_NE(storm.message.find("malformed-packet storm"), std::string::npos);
  EXPECT_EQ(settled.level, 0);
}

TEST(Diagnostics, ReportsFtStallBackwardAndRestartForOneCycleEach)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  evaluator.evaluate(snapshot);
  snapshot.ft_stall_count = 1;
  const auto stalled = evaluator.evaluate(snapshot);
  snapshot.ft_backward_count = 1;
  const auto backward = evaluator.evaluate(snapshot);
  snapshot.ft_restart_count = 1;
  const auto restarted = evaluator.evaluate(snapshot);
  const auto settled = evaluator.evaluate(snapshot);
  EXPECT_EQ(stalled.level, 2);
  EXPECT_NE(stalled.message.find("stalled"), std::string::npos);
  EXPECT_EQ(backward.level, 2);
  EXPECT_NE(backward.message.find("backward"), std::string::npos);
  EXPECT_EQ(restarted.level, 1);
  EXPECT_NE(restarted.message.find("restarted"), std::string::npos);
  EXPECT_EQ(settled.level, 0);
}

TEST(Diagnostics, ThrottlesFaultLogsByLevelAndKey)
{
  FaultLogThrottle throttle{10.0};
  const DiagnosticReport warning{1, "slow", {}, "receive_rate"};
  const DiagnosticReport error{2, "lost", {}, "backoff"};
  const DiagnosticReport healthy_report{0, "normal", {}, "healthy"};
  EXPECT_TRUE(throttle.should_log(warning, 1.0));
  EXPECT_FALSE(throttle.should_log(warning, 2.0));
  EXPECT_TRUE(throttle.should_log(warning, 11.0));
  EXPECT_TRUE(throttle.should_log(error, 11.1));
  EXPECT_FALSE(throttle.should_log(error, 12.0));
  EXPECT_FALSE(throttle.should_log(healthy_report, 12.1));
  EXPECT_TRUE(throttle.should_log(error, 12.2));
}

class InvalidDiagnosticParameters : public ::testing::TestWithParam<std::pair<double, double>> {};
TEST_P(InvalidDiagnosticParameters, RejectsFinitePositiveRateAndUnitIntervalToleranceContract)
{
  const auto [rate, tolerance] = GetParam();
  EXPECT_THROW((DiagnosticEvaluator{rate, tolerance}), std::invalid_argument);
}
INSTANTIATE_TEST_SUITE_P(Parameters, InvalidDiagnosticParameters, ::testing::Values(
  std::make_pair(0.0, 0.2), std::make_pair(-1.0, 0.2),
  std::make_pair(std::numeric_limits<double>::infinity(), 0.2),
  std::make_pair(std::numeric_limits<double>::quiet_NaN(), 0.2),
  std::make_pair(2000.0, -0.001), std::make_pair(2000.0, 1.001),
  std::make_pair(2000.0, std::numeric_limits<double>::infinity()),
  std::make_pair(2000.0, std::numeric_limits<double>::quiet_NaN())));

}  // namespace
