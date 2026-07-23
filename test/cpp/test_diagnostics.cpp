#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ros/diagnostics.hpp"

namespace {
using namespace netft_driver;

static_assert(!std::is_constructible<DiagnosticEvaluator, double, bool>::value,
  "DiagnosticEvaluator must reject boolean rate tolerance");
static_assert(std::is_constructible<DiagnosticEvaluator, double, double>::value,
  "DiagnosticEvaluator must accept a double rate tolerance");

const std::vector<std::string> kBaseValueKeys{
  "state", "sensor", "last_rdt_sequence", "last_ft_sequence", "last_ft_progress",
  "device_status", "active_status", "receive_rate_hz", "expected_receive_rate_hz",
  "rate_tolerance", "delivery_rate_hz", "received_count", "delivered_count",
  "rate_limited_count", "device_error_count", "lost_count", "duplicate_count",
  "out_of_order_count", "ft_stall_count", "ft_backward_count", "ft_restart_count",
  "malformed_count", "malformed_storm_threshold", "malformed_storm_window",
  "reconnect_count", "timeout_count", "callback_error_count", "last_record_age_s",
  "last_error"};

netft::HealthSnapshot healthy()
{
  netft::HealthSnapshot snapshot;
  snapshot.state = netft::ClientState::Streaming;
  snapshot.sensor_host = "127.0.0.1";
  snapshot.rdt_port = 49152;
  snapshot.last_rdt_sequence = 100;
  snapshot.last_ft_sequence = 400;
  snapshot.last_ft_progress = "forward";
  snapshot.receive_rate_hz = snapshot.delivery_rate_hz = 2000.0;
  snapshot.received_count = snapshot.delivered_count = 2000;
  snapshot.last_record_age = std::chrono::duration<double>{0.001};
  return snapshot;
}

std::vector<std::string> keys(const DiagnosticReport & report)
{
  std::vector<std::string> result;
  for (const auto & [key, value] : report.values) {
    static_cast<void>(value);
    result.push_back(key);
  }
  return result;
}

std::map<std::string, std::string> values(const DiagnosticReport & report)
{
  return {report.values.begin(), report.values.end()};
}

TEST(Diagnostics, ReportsHealthyStructuredValuesWithUpstreamNames)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.rate_limited_count = 5;

  const auto report = evaluator.evaluate(snapshot);
  const auto structured = values(report);

  EXPECT_EQ(report.level, 0);
  EXPECT_EQ(report.log_key, "healthy");
  EXPECT_EQ(keys(report), kBaseValueKeys);
  EXPECT_EQ(structured.at("state"), "streaming");
  EXPECT_EQ(structured.at("sensor"), "127.0.0.1:49152");
  EXPECT_EQ(structured.at("device_status"), "0x00000000");
  EXPECT_EQ(structured.at("active_status"), "healthy");
  EXPECT_EQ(structured.at("receive_rate_hz"), "2000.0");
  EXPECT_EQ(structured.at("expected_receive_rate_hz"), "2000.0");
  EXPECT_EQ(structured.at("rate_tolerance"), "0.200");
  EXPECT_EQ(structured.at("delivery_rate_hz"), "2000.0");
  EXPECT_EQ(structured.at("received_count"), "2000");
  EXPECT_EQ(structured.at("delivered_count"), "2000");
  EXPECT_EQ(structured.at("rate_limited_count"), "5");
  EXPECT_EQ(structured.at("last_record_age_s"), "0.001");
}

TEST(Diagnostics, AppendsSensorConfigurationMetadataWhenAvailable)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.sensor_configuration = netft::SensorConfiguration{
    "Net F/T",
    netft::Calibration{
      1000000.0,
      1000000.0,
      netft::ForceUnit::Newton,
      netft::TorqueUnit::NewtonMillimeter,
    },
    netft::CalibrationSource::Sensor,
    3,
  };
  snapshot.calibration_change_count = 2;

  const auto report = evaluator.evaluate(snapshot);
  const auto structured = values(report);
  auto expected_keys = kBaseValueKeys;
  expected_keys.insert(expected_keys.end(), {
    "configuration_source", "force_unit", "torque_unit", "configuration_revision",
    "calibration_change_count", "counts_per_force_unit", "counts_per_torque_unit"});

  EXPECT_EQ(keys(report), expected_keys);
  EXPECT_EQ(structured.at("configuration_source"), "sensor");
  EXPECT_EQ(structured.at("force_unit"), "N");
  EXPECT_EQ(structured.at("torque_unit"), "N-mm");
  EXPECT_EQ(structured.at("configuration_revision"), "3");
  EXPECT_EQ(structured.at("calibration_change_count"), "2");
  EXPECT_EQ(structured.at("counts_per_force_unit"), "1000000");
  EXPECT_EQ(structured.at("counts_per_torque_unit"), "1000000");
}

TEST(Diagnostics, AcceptsRateToleranceBoundsAndWarnsOutsideThem)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();

  snapshot.receive_rate_hz = 1600.0;
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);
  snapshot.receive_rate_hz = 2400.0;
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);
  snapshot.receive_rate_hz = 1599.9;
  const auto below = evaluator.evaluate(snapshot);
  snapshot.receive_rate_hz = 2400.1;
  const auto above = evaluator.evaluate(snapshot);

  EXPECT_EQ(below.level, 1);
  EXPECT_EQ(below.log_key, "receive_rate");
  EXPECT_EQ(above.level, 1);
  EXPECT_EQ(above.log_key, "receive_rate");
}

TEST(Diagnostics, UsesUpstreamDeviceStatusClassification)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();

  snapshot.last_status = 0x80010000U;
  const auto warning = evaluator.evaluate(snapshot);
  snapshot.last_status = 0x80020000U;
  const auto error = evaluator.evaluate(snapshot);

  EXPECT_EQ(warning.level, 1);
  EXPECT_EQ(warning.log_key, "condition_latch");
  EXPECT_EQ(values(warning).at("device_status"), "0x80010000");
  EXPECT_EQ(error.level, 2);
  EXPECT_EQ(error.log_key, "device_error");
  EXPECT_EQ(values(error).at("device_status"), "0x80020000");
}

TEST(Diagnostics, ReportsCounterDeltasForOneEvaluation)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);

  snapshot.device_error_count = 1;
  const auto device_error = evaluator.evaluate(snapshot);
  const auto device_settled = evaluator.evaluate(snapshot);
  snapshot.lost_count = 3;
  const auto loss = evaluator.evaluate(snapshot);
  const auto loss_settled = evaluator.evaluate(snapshot);

  EXPECT_EQ(device_error.level, 2);
  EXPECT_EQ(device_error.log_key, "device_error_event");
  EXPECT_EQ(device_settled.level, 0);
  EXPECT_EQ(loss.level, 1);
  EXPECT_EQ(loss.log_key, "packet_loss");
  EXPECT_EQ(loss_settled.level, 0);
}

TEST(Diagnostics, PrioritizesTimeoutOverConnectingStateForOneEvaluation)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.state = netft::ClientState::Connecting;
  snapshot.timeout_count = 1;

  const auto timeout = evaluator.evaluate(snapshot);
  const auto connecting = evaluator.evaluate(snapshot);

  EXPECT_EQ(timeout.level, 2);
  EXPECT_EQ(timeout.log_key, "receive_timeout");
  EXPECT_EQ(connecting.level, 1);
  EXPECT_EQ(connecting.log_key, "connecting");
}

TEST(Diagnostics, UsesExactMalformedStormThresholdWithinOneWindow)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);

  snapshot.malformed_count = kMalformedStormThreshold - 1;
  EXPECT_EQ(evaluator.evaluate(snapshot).level, 0);
  snapshot.malformed_count = (2 * kMalformedStormThreshold) - 1;
  const auto storm = evaluator.evaluate(snapshot);
  const auto settled = evaluator.evaluate(snapshot);

  EXPECT_EQ(storm.level, 2);
  EXPECT_EQ(storm.log_key, "malformed_storm");
  EXPECT_EQ(settled.level, 0);
}

TEST(Diagnostics, ReportsFtCounterDeltasForOneEvaluationEach)
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
  EXPECT_EQ(stalled.log_key, "ft_stall");
  EXPECT_EQ(backward.level, 2);
  EXPECT_EQ(backward.log_key, "ft_backward");
  EXPECT_EQ(restarted.level, 1);
  EXPECT_EQ(restarted.log_key, "ft_restart");
  EXPECT_EQ(settled.level, 0);
}

TEST(Diagnostics, ReportsCallbackCounterDeltasForOneEvaluation)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  evaluator.evaluate(snapshot);

  snapshot.callback_error_count = 1;
  const auto callback_error = evaluator.evaluate(snapshot);
  const auto settled = evaluator.evaluate(snapshot);

  EXPECT_EQ(callback_error.level, 1);
  EXPECT_EQ(callback_error.log_key, "callback_error");
  EXPECT_EQ(settled.level, 0);
}

TEST(Diagnostics, KeepsCallbackFaultAtErrorLevel)
{
  DiagnosticEvaluator evaluator{2000.0, 0.2};
  auto snapshot = healthy();
  snapshot.state = netft::ClientState::Faulted;
  snapshot.fault_code = netft::FaultCode::Callback;
  snapshot.callback_error_count = 1;

  const auto first = evaluator.evaluate(snapshot);
  const auto second = evaluator.evaluate(snapshot);

  EXPECT_EQ(first.level, 2);
  EXPECT_EQ(first.log_key, "faulted");
  EXPECT_EQ(second.level, 2);
  EXPECT_EQ(second.log_key, "faulted");
  EXPECT_EQ(values(second).at("state"), "faulted");
}

TEST(Diagnostics, ThrottlesFaultLogsByLevelAndStableLogKey)
{
  FaultLogThrottle throttle{10.0};
  const DiagnosticReport warning_a{1, "ignored-a", {}, "warning_key"};
  const DiagnosticReport warning_b{1, "ignored-b", {}, "warning_key"};
  const DiagnosticReport error{2, "ignored-c", {}, "error_key"};
  const DiagnosticReport healthy_report{0, {}, {}, "healthy"};

  EXPECT_TRUE(throttle.should_log(warning_a, 1.0));
  EXPECT_FALSE(throttle.should_log(warning_b, 2.0));
  EXPECT_TRUE(throttle.should_log(warning_b, 11.0));
  EXPECT_TRUE(throttle.should_log(error, 11.1));
  EXPECT_FALSE(throttle.should_log(error, 12.0));
  EXPECT_FALSE(throttle.should_log(healthy_report, 12.1));
  EXPECT_TRUE(throttle.should_log(error, 12.2));
}

class InvalidDiagnosticParameters : public ::testing::TestWithParam<std::pair<double, double>> {};
TEST_P(InvalidDiagnosticParameters, RejectsInvalidRateOrTolerance)
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
