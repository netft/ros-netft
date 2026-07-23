#include "ros/adapter_config.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace {

TEST(AdapterConfig, UsesSensorCalibrationByDefault)
{
  netft_driver::AdapterParameters parameters;
  parameters.sensor_ip = "127.0.0.1";
  parameters.sensor_port = 50000;
  parameters.http_port = 8080;
  parameters.configuration_connect_timeout = 0.25;
  parameters.configuration_timeout = 0.75;
  parameters.publish_rate = 100.0;
  parameters.publish_on_error = true;

  const auto mapped = netft_driver::map_adapter_parameters(parameters);

  EXPECT_EQ(mapped.client.sensor_host, "127.0.0.1");
  EXPECT_EQ(mapped.client.rdt_port, 50000);
  EXPECT_EQ(mapped.client.http_port, 8080);
  EXPECT_DOUBLE_EQ(mapped.client.configuration_connect_timeout.count(), 0.25);
  EXPECT_DOUBLE_EQ(mapped.client.configuration_timeout.count(), 0.75);
  EXPECT_DOUBLE_EQ(mapped.client.sample_rate_limit_hz, 100.0);
  EXPECT_TRUE(mapped.client.deliver_samples_with_error_status);
  EXPECT_FALSE(mapped.client.calibration_override.has_value());
}

TEST(AdapterConfig, MapsCompleteManualCalibrationAsNewtonAndNewtonMetre)
{
  netft_driver::AdapterParameters parameters;
  parameters.use_sensor_calibration = false;
  parameters.counts_per_force = 2000000.0;
  parameters.counts_per_torque = 4000000.0;

  const auto mapped = netft_driver::map_adapter_parameters(parameters);

  ASSERT_TRUE(mapped.client.calibration_override.has_value());
  EXPECT_DOUBLE_EQ(mapped.client.calibration_override->counts_per_force_unit, 2000000.0);
  EXPECT_DOUBLE_EQ(mapped.client.calibration_override->counts_per_torque_unit, 4000000.0);
  EXPECT_EQ(mapped.client.calibration_override->force_unit, netft::ForceUnit::Newton);
  EXPECT_EQ(mapped.client.calibration_override->torque_unit, netft::TorqueUnit::NewtonMeter);
}

TEST(AdapterConfig, MapsEveryClientAndAdapterParameter)
{
  netft_driver::AdapterParameters parameters;
  parameters.sensor_ip = "127.0.0.7";
  parameters.sensor_port = 49153;
  parameters.http_port = 8081;
  parameters.frame_id = "test_frame";
  parameters.wrench_topic = "relative_wrench";
  parameters.bias_service = "relative_bias";
  parameters.receive_timeout = 0.2;
  parameters.configuration_connect_timeout = 0.3;
  parameters.configuration_timeout = 0.4;
  parameters.reconnect_initial_delay = 0.5;
  parameters.reconnect_max_delay = 0.6;
  parameters.publish_rate = 7.0;
  parameters.publish_on_error = true;
  parameters.diagnostics_rate = 8.0;
  parameters.expected_rdt_rate = 9.0;
  parameters.rate_tolerance = 0.7;

  const auto mapped = netft_driver::map_adapter_parameters(parameters);

  EXPECT_EQ(mapped.client.sensor_host, "127.0.0.7");
  EXPECT_EQ(mapped.client.rdt_port, 49153);
  EXPECT_EQ(mapped.client.http_port, 8081);
  EXPECT_DOUBLE_EQ(mapped.client.receive_timeout.count(), 0.2);
  EXPECT_DOUBLE_EQ(mapped.client.configuration_connect_timeout.count(), 0.3);
  EXPECT_DOUBLE_EQ(mapped.client.configuration_timeout.count(), 0.4);
  EXPECT_DOUBLE_EQ(mapped.client.reconnect_initial_delay.count(), 0.5);
  EXPECT_DOUBLE_EQ(mapped.client.reconnect_max_delay.count(), 0.6);
  EXPECT_DOUBLE_EQ(mapped.client.sample_rate_limit_hz, 7.0);
  EXPECT_TRUE(mapped.client.deliver_samples_with_error_status);
  EXPECT_EQ(mapped.frame_id, "test_frame");
  EXPECT_EQ(mapped.wrench_topic, "relative_wrench");
  EXPECT_EQ(mapped.bias_service, "relative_bias");
  EXPECT_DOUBLE_EQ(mapped.diagnostics_rate, 8.0);
  EXPECT_DOUBLE_EQ(mapped.expected_rdt_rate, 9.0);
  EXPECT_DOUBLE_EQ(mapped.rate_tolerance, 0.7);
}

TEST(AdapterConfig, AcceptsNonEmptyNamesAndPositiveFiniteDiagnosticsRate)
{
  EXPECT_NO_THROW(netft_driver::validate_adapter_config("netft_link", "wrench", "bias", 1.0));
}

TEST(AdapterConfig, RejectsBlankNamesAndInvalidDiagnosticsRate)
{
  EXPECT_THROW(netft_driver::validate_adapter_config(" \t", "wrench", "bias", 1.0),
               std::invalid_argument);
  EXPECT_THROW(netft_driver::validate_adapter_config("frame", "\n", "bias", 1.0),
               std::invalid_argument);
  EXPECT_THROW(netft_driver::validate_adapter_config("frame", "wrench", " ", 1.0),
               std::invalid_argument);
  for (const double value : {0.0, std::numeric_limits<double>::infinity()}) {
    EXPECT_THROW(netft_driver::validate_adapter_config("frame", "wrench", "bias", value),
                 std::invalid_argument);
  }
}

TEST(AdapterConfig, RejectsInvalidReceiveAndReconnectTimeouts)
{
  netft_driver::AdapterParameters parameters;
  parameters.receive_timeout = 0.0;
  EXPECT_THROW(netft_driver::map_adapter_parameters(parameters), std::invalid_argument);

  parameters.receive_timeout = 0.1;
  parameters.reconnect_initial_delay = 2.0;
  parameters.reconnect_max_delay = 1.0;
  EXPECT_THROW(netft_driver::map_adapter_parameters(parameters), std::invalid_argument);
}

TEST(AdapterConfig, RejectsInvalidSampleRateLimit)
{
  netft_driver::AdapterParameters parameters;
  parameters.publish_rate = -1.0;

  EXPECT_THROW(netft_driver::map_adapter_parameters(parameters), std::invalid_argument);
}

TEST(AdapterConfig, ValidatesManualCountsOnlyWhenSensorCalibrationIsDisabled)
{
  netft_driver::AdapterParameters parameters;
  parameters.counts_per_force = 0.0;
  parameters.counts_per_torque = std::numeric_limits<double>::infinity();

  EXPECT_NO_THROW(netft_driver::map_adapter_parameters(parameters));

  parameters.use_sensor_calibration = false;
  EXPECT_THROW(netft_driver::map_adapter_parameters(parameters), std::invalid_argument);
}

}  // namespace
