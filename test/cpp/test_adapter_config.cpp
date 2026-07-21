#include "netft_driver/adapter_config.hpp"

#include <gtest/gtest.h>
#include <limits>

TEST(AdapterConfig, MapsEveryAdapterParameterExactlyWithoutRos)
{
  netft_driver::AdapterParameters parameters;
  parameters.sensor_ip = "127.0.0.7";
  parameters.sensor_port = 49153;
  parameters.frame_id = "test_frame";
  parameters.wrench_topic = "relative_wrench";
  parameters.bias_service = "relative_bias";
  parameters.counts_per_force = 2.0;
  parameters.counts_per_torque = 3.0;
  parameters.publish_rate = 4.0;
  parameters.receive_timeout = 0.2;
  parameters.reconnect_initial_delay = 0.3;
  parameters.reconnect_max_delay = 0.4;
  parameters.diagnostics_rate = 5.0;
  parameters.expected_rdt_rate = 6.0;
  parameters.rate_tolerance = 0.7;
  parameters.publish_on_error = true;

  const auto mapped = netft_driver::map_adapter_parameters(parameters);

  EXPECT_EQ(mapped.client.sensor_host, "127.0.0.7");
  EXPECT_EQ(mapped.client.sensor_port, 49153);
  EXPECT_EQ(mapped.frame_id, "test_frame");
  EXPECT_EQ(mapped.wrench_topic, "relative_wrench");
  EXPECT_EQ(mapped.bias_service, "relative_bias");
  EXPECT_DOUBLE_EQ(mapped.client.counts_per_force, 2.0);
  EXPECT_DOUBLE_EQ(mapped.client.counts_per_torque, 3.0);
  EXPECT_DOUBLE_EQ(mapped.client.publish_rate, 4.0);
  EXPECT_DOUBLE_EQ(mapped.client.receive_timeout.count(), 0.2);
  EXPECT_DOUBLE_EQ(mapped.client.reconnect_initial_delay.count(), 0.3);
  EXPECT_DOUBLE_EQ(mapped.client.reconnect_max_delay.count(), 0.4);
  EXPECT_DOUBLE_EQ(mapped.diagnostics_rate, 5.0);
  EXPECT_DOUBLE_EQ(mapped.expected_rdt_rate, 6.0);
  EXPECT_DOUBLE_EQ(mapped.rate_tolerance, 0.7);
  EXPECT_TRUE(mapped.client.publish_on_error);
}

TEST(AdapterConfig, AcceptsNonEmptyNamesAndPositiveFiniteDiagnosticsRate)
{
  EXPECT_NO_THROW(netft_driver::validate_adapter_config("netft_link", "wrench", "bias", 1.0));
}

TEST(AdapterConfig, RejectsBlankNamesAndInvalidDiagnosticsRateWithFieldNames)
{
  try { netft_driver::validate_adapter_config(" \t", "wrench", "bias", 1.0); FAIL(); }
  catch (const std::invalid_argument & error) { EXPECT_NE(std::string{error.what()}.find("frame_id"), std::string::npos); }
  try { netft_driver::validate_adapter_config("frame", "\n", "bias", 1.0); FAIL(); }
  catch (const std::invalid_argument & error) { EXPECT_NE(std::string{error.what()}.find("wrench_topic"), std::string::npos); }
  try { netft_driver::validate_adapter_config("frame", "wrench", " ", 1.0); FAIL(); }
  catch (const std::invalid_argument & error) { EXPECT_NE(std::string{error.what()}.find("bias_service"), std::string::npos); }
  for (const double value : {0.0, std::numeric_limits<double>::infinity()}) {
    try { netft_driver::validate_adapter_config("frame", "wrench", "bias", value); FAIL(); }
    catch (const std::invalid_argument & error) { EXPECT_NE(std::string{error.what()}.find("diagnostics_rate"), std::string::npos); }
  }
}
