#define NETFT_ADAPTER_NO_MAIN
#include "../../src/ros2_node.cpp"

#include <gtest/gtest.h>

TEST(NetFTRos2Node, UsesAtiAndSensorDiscoveryDefaults)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<NetFTRos2Node>();

  EXPECT_EQ(node->config().sensor_host, "192.168.1.1");
  EXPECT_EQ(node->config().rdt_port, 49152);
  EXPECT_EQ(node->config().http_port, 80);
  EXPECT_DOUBLE_EQ(node->config().configuration_connect_timeout.count(), 0.5);
  EXPECT_DOUBLE_EQ(node->config().configuration_timeout.count(), 1.0);
  EXPECT_FALSE(node->config().calibration_override.has_value());
  node->stop();
  node.reset();
  rclcpp::shutdown();
}

TEST(NetFTRos2Node, DeclaresAndMapsEveryAdapterParameterWithManualCalibration)
{
  rclcpp::init(0, nullptr);
  rclcpp::NodeOptions options;
  options.parameter_overrides({
    rclcpp::Parameter{"sensor_ip", "127.0.0.1"}, rclcpp::Parameter{"sensor_port", 49153},
    rclcpp::Parameter{"http_port", 8080},
    rclcpp::Parameter{"frame_id", "test_frame"}, rclcpp::Parameter{"wrench_topic", "/test/wrench"},
    rclcpp::Parameter{"bias_service", "/test/bias"}, rclcpp::Parameter{"use_sensor_calibration", false},
    rclcpp::Parameter{"counts_per_force", 2.0},
    rclcpp::Parameter{"counts_per_torque", 3.0}, rclcpp::Parameter{"publish_rate", 4.0},
    rclcpp::Parameter{"receive_timeout", 0.2},
    rclcpp::Parameter{"configuration_connect_timeout", 0.25},
    rclcpp::Parameter{"configuration_timeout", 0.75},
    rclcpp::Parameter{"reconnect_initial_delay", 0.3},
    rclcpp::Parameter{"reconnect_max_delay", 0.4}, rclcpp::Parameter{"diagnostics_rate", 5.0},
    rclcpp::Parameter{"expected_rdt_rate", 6.0}, rclcpp::Parameter{"rate_tolerance", 0.7},
    rclcpp::Parameter{"publish_on_error", true}});
  auto node = std::make_shared<NetFTRos2Node>(options);
  EXPECT_EQ(node->config().sensor_host, "127.0.0.1");
  EXPECT_EQ(node->config().rdt_port, 49153);
  EXPECT_EQ(node->config().http_port, 8080);
  EXPECT_EQ(node->frame_id(), "test_frame");
  EXPECT_EQ(node->wrench_topic(), "/test/wrench");
  EXPECT_EQ(node->bias_service(), "/test/bias");
  ASSERT_TRUE(node->config().calibration_override.has_value());
  EXPECT_DOUBLE_EQ(node->config().calibration_override->counts_per_force_unit, 2.0);
  EXPECT_DOUBLE_EQ(node->config().calibration_override->counts_per_torque_unit, 3.0);
  EXPECT_EQ(node->config().calibration_override->force_unit, netft::ForceUnit::Newton);
  EXPECT_EQ(node->config().calibration_override->torque_unit, netft::TorqueUnit::NewtonMeter);
  EXPECT_DOUBLE_EQ(node->config().sample_rate_limit_hz, 4.0);
  EXPECT_DOUBLE_EQ(node->config().receive_timeout.count(), 0.2);
  EXPECT_DOUBLE_EQ(node->config().configuration_connect_timeout.count(), 0.25);
  EXPECT_DOUBLE_EQ(node->config().configuration_timeout.count(), 0.75);
  EXPECT_DOUBLE_EQ(node->config().reconnect_initial_delay.count(), 0.3);
  EXPECT_DOUBLE_EQ(node->config().reconnect_max_delay.count(), 0.4);
  EXPECT_DOUBLE_EQ(node->diagnostics_rate(), 5.0);
  EXPECT_DOUBLE_EQ(node->expected_rdt_rate(), 6.0);
  EXPECT_DOUBLE_EQ(node->rate_tolerance(), 0.7);
  EXPECT_TRUE(node->config().deliver_samples_with_error_status);
  node->stop();
  node.reset();
  rclcpp::shutdown();
}
