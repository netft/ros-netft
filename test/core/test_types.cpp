#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <limits>
#include <string>

#include "netft/types.hpp"

TEST(Config, DefaultsToAutomaticSensorDiscovery) {
  const netft::Config config;
  EXPECT_EQ(config.sensor_host, "192.168.1.1");
  EXPECT_EQ(config.rdt_port, 49152);
  EXPECT_EQ(config.http_port, 80);
  EXPECT_FALSE(config.calibration_override.has_value());
  EXPECT_EQ(config.recovery_policy, netft::RecoveryPolicy::Reconnect);
}

TEST(Config, AcceptsDefaults) { EXPECT_NO_THROW(netft::validate(netft::Config{})); }

struct InvalidConfigCase {
  std::string name;
  std::function<void(netft::Config &)> apply;
};

class ConfigValidation : public ::testing::TestWithParam<InvalidConfigCase> {};

TEST_P(ConfigValidation, RejectsEveryInvalidScalar) {
  netft::Config config;
  GetParam().apply(config);
  EXPECT_THROW(netft::validate(config), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P(InvalidScalars, ConfigValidation,
                         ::
                             testing::Values(InvalidConfigCase{"empty_sensor_host",
                                                               [](auto &config) {
                                                                 config.sensor_host.clear();
                                                               }},
                                             InvalidConfigCase{
                                                 "whitespace_sensor_host",
                                                 [](auto &config) { config.sensor_host = " \t"; }},
                                             InvalidConfigCase{
                                                 "rdt_port_too_low",
                                                 [](auto &config) { config.rdt_port = 0; }},
                                             InvalidConfigCase{
                                                 "rdt_port_too_high",
                                                 [](auto &config) { config.rdt_port = 65536; }},
                                             InvalidConfigCase{
                                                 "http_port_too_low",
                                                 [](auto &config) { config.http_port = 0; }},
                                             InvalidConfigCase{
                                                 "http_port_too_high",
                                                 [](auto &config) { config.http_port = 65536; }},
                                             InvalidConfigCase{"receive_timeout_zero",
                                                               [](auto &config) {
                                                                 config.receive_timeout =
                                                                     std::chrono::duration<double>{
                                                                         0.0};
                                                               }},
                                             InvalidConfigCase{"receive_timeout_negative",
                                                               [](auto &config) {
                                                                 config.receive_timeout =
                                                                     std::chrono::duration<double>{
                                                                         -0.1};
                                                               }},
                                             InvalidConfigCase{
                                                 "receive_timeout_infinite",
                                                 [](auto &config) {
                                                   config.receive_timeout =
                                                       std::chrono::duration<double>{
                                                           std::numeric_limits<double>::infinity()};
                                                 }},
                                             InvalidConfigCase{"receive_timeout_nan",
                                                               [](auto &config) {
                                                                 config.receive_timeout =
                                                                     std::chrono::duration<double>{
                                                                         std::numeric_limits<
                                                                             double>::quiet_NaN()};
                                                               }},
                                             InvalidConfigCase{
                                                 "configuration_connect_timeout_zero",
                                                 [](auto &config) {
                                                   config.configuration_connect_timeout =
                                                       std::chrono::duration<double>{0.0};
                                                 }},
                                             InvalidConfigCase{
                                                 "configuration_connect_timeout_negative",
                                                 [](auto &config) {
                                                   config.configuration_connect_timeout =
                                                       std::chrono::duration<double>{-0.1};
                                                 }},
                                             InvalidConfigCase{
                                                 "configuration_connect_timeout_infinite",
                                                 [](auto &config) {
                                                   config.configuration_connect_timeout =
                                                       std::chrono::duration<double>{
                                                           std::numeric_limits<double>::infinity()};
                                                 }},
                                             InvalidConfigCase{
                                                 "configuration_connect_timeout_nan",
                                                 [](auto &config) {
                                                   config.configuration_connect_timeout =
                                                       std::chrono::duration<double>{
                                                           std::numeric_limits<
                                                               double>::quiet_NaN()};
                                                 }},
                                             InvalidConfigCase{"configuration_timeout_zero",
                                                               [](auto &config) {
                                                                 config.configuration_timeout =
                                                                     std::chrono::duration<double>{
                                                                         0.0};
                                                               }},
                                             InvalidConfigCase{"configuration_timeout_negative",
                                                               [](auto &config) {
                                                                 config.configuration_timeout =
                                                                     std::chrono::duration<double>{
                                                                         -0.1};
                                                               }},
                                             InvalidConfigCase{
                                                 "configuration_timeout_infinite",
                                                 [](auto &config) {
                                                   config.configuration_timeout =
                                                       std::chrono::duration<double>{
                                                           std::numeric_limits<double>::infinity()};
                                                 }},
                                             InvalidConfigCase{"configuration_timeout_nan",
                                                               [](auto &config) {
                                                                 config.configuration_timeout =
                                                                     std::chrono::duration<double>{
                                                                         std::numeric_limits<
                                                                             double>::quiet_NaN()};
                                                               }},
                                             InvalidConfigCase{"reconnect_initial_delay_zero",
                                                               [](auto &config) {
                                                                 config.reconnect_initial_delay =
                                                                     std::chrono::duration<double>{
                                                                         0.0};
                                                               }},
                                             InvalidConfigCase{"reconnect_initial_delay_negative",
                                                               [](auto &config) {
                                                                 config.reconnect_initial_delay =
                                                                     std::chrono::duration<double>{
                                                                         -0.1};
                                                               }},
                                             InvalidConfigCase{
                                                 "reconnect_initial_delay_infinite",
                                                 [](auto &config) {
                                                   config.reconnect_initial_delay =
                                                       std::chrono::duration<double>{
                                                           std::numeric_limits<double>::infinity()};
                                                 }},
                                             InvalidConfigCase{"reconnect_initial_delay_nan",
                                                               [](auto &config) {
                                                                 config.reconnect_initial_delay =
                                                                     std::chrono::duration<double>{
                                                                         std::numeric_limits<
                                                                             double>::quiet_NaN()};
                                                               }},
                                             InvalidConfigCase{"reconnect_max_delay_zero",
                                                               [](auto &config) {
                                                                 config.reconnect_max_delay =
                                                                     std::chrono::duration<double>{
                                                                         0.0};
                                                               }},
                                             InvalidConfigCase{"reconnect_max_delay_negative",
                                                               [](auto &config) {
                                                                 config.reconnect_max_delay =
                                                                     std::chrono::duration<double>{
                                                                         -0.1};
                                                               }},
                                             InvalidConfigCase{"reconnect_max_delay_infinite",
                                                               [](auto &config) {
                                                                 config.reconnect_max_delay =
                                                                     std::chrono::duration<double>{
                                                                         std::numeric_limits<
                                                                             double>::infinity()};
                                                               }},
                                             InvalidConfigCase{"reconnect_max_delay_nan",
                                                               [](auto &config) {
                                                                 config.reconnect_max_delay =
                                                                     std::chrono::duration<double>{
                                                                         std::numeric_limits<
                                                                             double>::quiet_NaN()};
                                                               }},
                                             InvalidConfigCase{
                                                 "reconnect_max_delay_below_initial",
                                                 [](auto &config) {
                                                   config.reconnect_max_delay =
                                                       config.reconnect_initial_delay / 2.0;
                                                 }},
                                             InvalidConfigCase{"sample_rate_limit_negative",
                                                               [](auto &config) {
                                                                 config.sample_rate_limit_hz = -1.0;
                                                               }},
                                             InvalidConfigCase{"sample_rate_limit_infinite",
                                                               [](auto &config) {
                                                                 config.sample_rate_limit_hz =
                                                                     std::numeric_limits<
                                                                         double>::infinity();
                                                               }},
                                             InvalidConfigCase{"sample_rate_limit_nan",
                                                               [](auto &config) {
                                                                 config.sample_rate_limit_hz =
                                                                     std::numeric_limits<
                                                                         double>::quiet_NaN();
                                                               }}),
                         [](const auto &info) { return info.param.name; });

struct InvalidCalibrationCase {
  std::string name;
  std::function<void(netft::Calibration &)> apply;
};

class CalibrationValidation : public ::testing::TestWithParam<InvalidCalibrationCase> {};

TEST_P(CalibrationValidation, RejectsNonPositiveAndNonFiniteCounts) {
  netft::Calibration calibration{1.0, 1.0, netft::ForceUnit::Newton,
                                 netft::TorqueUnit::NewtonMeter};
  GetParam().apply(calibration);
  EXPECT_THROW(netft::validate(calibration), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidCounts, CalibrationValidation,
    ::testing::Values(
        InvalidCalibrationCase{"force_count_zero",
                               [](auto &calibration) { calibration.counts_per_force_unit = 0.0; }},
        InvalidCalibrationCase{"force_count_negative",
                               [](auto &calibration) { calibration.counts_per_force_unit = -1.0; }},
        InvalidCalibrationCase{"force_count_infinite",
                               [](auto &calibration) {
                                 calibration.counts_per_force_unit =
                                     std::numeric_limits<double>::infinity();
                               }},
        InvalidCalibrationCase{"force_count_nan",
                               [](auto &calibration) {
                                 calibration.counts_per_force_unit =
                                     std::numeric_limits<double>::quiet_NaN();
                               }},
        InvalidCalibrationCase{"torque_count_zero",
                               [](auto &calibration) { calibration.counts_per_torque_unit = 0.0; }},
        InvalidCalibrationCase{
            "torque_count_negative",
            [](auto &calibration) { calibration.counts_per_torque_unit = -1.0; }},
        InvalidCalibrationCase{"torque_count_infinite",
                               [](auto &calibration) {
                                 calibration.counts_per_torque_unit =
                                     std::numeric_limits<double>::infinity();
                               }},
        InvalidCalibrationCase{"torque_count_nan",
                               [](auto &calibration) {
                                 calibration.counts_per_torque_unit =
                                     std::numeric_limits<double>::quiet_NaN();
                               }}),
    [](const auto &info) { return info.param.name; });

TEST(Config, RejectsAnAmbiguousManualCalibration) {
  netft::Config config;
  config.calibration_override = netft::Calibration{
      1'000'000.0, 1'000'000.0, netft::ForceUnit::Newton, netft::TorqueUnit::Unknown};
  EXPECT_THROW(netft::validate(config), std::invalid_argument);
}

TEST(Units, ConvertsForceAndTorqueUnitStrings) {
  EXPECT_EQ(netft::to_string(netft::ForceUnit::PoundForce), "lbf");
  EXPECT_EQ(netft::to_string(netft::ForceUnit::Newton), "N");
  EXPECT_EQ(netft::to_string(netft::ForceUnit::KiloPoundForce), "klbf");
  EXPECT_EQ(netft::to_string(netft::ForceUnit::KiloNewton), "kN");
  EXPECT_EQ(netft::to_string(netft::ForceUnit::KilogramForce), "kgf");
  EXPECT_EQ(netft::force_unit_from_string("N"), netft::ForceUnit::Newton);
  EXPECT_EQ(netft::force_unit_from_string("invalid"), std::nullopt);

  EXPECT_EQ(netft::to_string(netft::TorqueUnit::PoundForceInch), "lbf-in");
  EXPECT_EQ(netft::to_string(netft::TorqueUnit::PoundForceFoot), "lbf-ft");
  EXPECT_EQ(netft::to_string(netft::TorqueUnit::NewtonMeter), "N-m");
  EXPECT_EQ(netft::to_string(netft::TorqueUnit::NewtonMillimeter), "N-mm");
  EXPECT_EQ(netft::to_string(netft::TorqueUnit::KilogramForceCentimeter), "kgf-cm");
  EXPECT_EQ(netft::to_string(netft::TorqueUnit::KiloNewtonMeter), "kN-m");
  EXPECT_EQ(netft::torque_unit_from_string("N-m"), netft::TorqueUnit::NewtonMeter);
  EXPECT_EQ(netft::torque_unit_from_string("invalid"), std::nullopt);
}

TEST(States, ConvertsStateAndFaultCodeStrings) {
  EXPECT_EQ(netft::to_string(netft::ClientState::Streaming), "streaming");
  EXPECT_EQ(netft::to_string(netft::FaultCode::SeriousStatus), "serious_status");
}
