#define NETFT_ADAPTER_NO_MAIN
#include "../../src/ros2_node.cpp"

#include <gtest/gtest.h>

TEST(NetFTRos2Node, DeclaresAndMapsEveryAdapterParameter)
{
  rclcpp::init(0, nullptr);
  rclcpp::NodeOptions options;
  options.parameter_overrides({
    rclcpp::Parameter{"sensor_ip", "127.0.0.1"}, rclcpp::Parameter{"sensor_port", 49153},
    rclcpp::Parameter{"frame_id", "test_frame"}, rclcpp::Parameter{"wrench_topic", "/test/wrench"},
    rclcpp::Parameter{"bias_service", "/test/bias"}, rclcpp::Parameter{"counts_per_force", 2.0},
    rclcpp::Parameter{"counts_per_torque", 3.0}, rclcpp::Parameter{"publish_rate", 4.0},
    rclcpp::Parameter{"receive_timeout", 0.2}, rclcpp::Parameter{"reconnect_initial_delay", 0.3},
    rclcpp::Parameter{"reconnect_max_delay", 0.4}, rclcpp::Parameter{"diagnostics_rate", 5.0},
    rclcpp::Parameter{"expected_rdt_rate", 6.0}, rclcpp::Parameter{"rate_tolerance", 0.7},
    rclcpp::Parameter{"publish_on_error", true}});
  auto node = std::make_shared<NetFTRos2Node>(options);
  EXPECT_EQ(node->config().sensor_host, "127.0.0.1");
  EXPECT_EQ(node->config().sensor_port, 49153);
  EXPECT_EQ(node->frame_id(), "test_frame");
  EXPECT_EQ(node->wrench_topic(), "/test/wrench");
  EXPECT_EQ(node->bias_service(), "/test/bias");
  EXPECT_DOUBLE_EQ(node->config().counts_per_force, 2.0);
  EXPECT_DOUBLE_EQ(node->config().counts_per_torque, 3.0);
  EXPECT_DOUBLE_EQ(node->config().publish_rate, 4.0);
  EXPECT_DOUBLE_EQ(node->config().receive_timeout.count(), 0.2);
  EXPECT_DOUBLE_EQ(node->config().reconnect_initial_delay.count(), 0.3);
  EXPECT_DOUBLE_EQ(node->config().reconnect_max_delay.count(), 0.4);
  EXPECT_DOUBLE_EQ(node->diagnostics_rate(), 5.0);
  EXPECT_DOUBLE_EQ(node->expected_rdt_rate(), 6.0);
  EXPECT_DOUBLE_EQ(node->rate_tolerance(), 0.7);
  EXPECT_TRUE(node->config().publish_on_error);
  node->stop();
  node.reset();
  rclcpp::shutdown();
}
