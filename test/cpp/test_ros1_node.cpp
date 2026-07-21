#define NETFT_ADAPTER_NO_MAIN
#include "../../src/ros1_node.cpp"

#include <gtest/gtest.h>

int main(int argc, char ** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(NetFTRos1Node, PublishesTheConfiguredGraph)
{
  int argc = 0;
  ros::init(argc, nullptr, "netft_ros1_node_test", ros::init_options::AnonymousName);
  if (!ros::master::check()) {
    ros::shutdown();
    GTEST_SKIP() << "a ROS master is required for ROS 1 graph assertions";
  }
  ros::NodeHandle private_node{"~"};
  private_node.setParam("sensor_ip", "127.0.0.1");
  private_node.setParam("sensor_port", 49153);
  private_node.setParam("frame_id", "test_frame");
  private_node.setParam("wrench_topic", "/test/wrench");
  private_node.setParam("bias_service", "/test/bias");
  private_node.setParam("counts_per_force", 2.0);
  private_node.setParam("counts_per_torque", 3.0);
  private_node.setParam("publish_rate", 4.0);
  private_node.setParam("receive_timeout", 0.2);
  private_node.setParam("reconnect_initial_delay", 0.3);
  private_node.setParam("reconnect_max_delay", 0.4);
  private_node.setParam("diagnostics_rate", 5.0);
  private_node.setParam("expected_rdt_rate", 6.0);
  private_node.setParam("rate_tolerance", 0.7);
  private_node.setParam("publish_on_error", true);

  ros::NodeHandle public_node;
  NetFTRos1Node node{public_node, private_node};
  EXPECT_EQ(node.config().sensor_host, "127.0.0.1");
  EXPECT_EQ(node.config().sensor_port, 49153);
  EXPECT_EQ(node.frame_id(), "test_frame");
  EXPECT_EQ(node.wrench_topic(), "/test/wrench");
  EXPECT_EQ(node.bias_service(), "/test/bias");
  EXPECT_DOUBLE_EQ(node.config().counts_per_force, 2.0);
  EXPECT_DOUBLE_EQ(node.config().counts_per_torque, 3.0);
  EXPECT_DOUBLE_EQ(node.config().publish_rate, 4.0);
  EXPECT_DOUBLE_EQ(node.config().receive_timeout.count(), 0.2);
  EXPECT_DOUBLE_EQ(node.config().reconnect_initial_delay.count(), 0.3);
  EXPECT_DOUBLE_EQ(node.config().reconnect_max_delay.count(), 0.4);
  EXPECT_DOUBLE_EQ(node.diagnostics_rate(), 5.0);
  EXPECT_DOUBLE_EQ(node.expected_rdt_rate(), 6.0);
  EXPECT_DOUBLE_EQ(node.rate_tolerance(), 0.7);
  EXPECT_TRUE(node.config().publish_on_error);
  node.stop();
  ros::shutdown();
}
