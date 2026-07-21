#include <diagnostic_msgs/DiagnosticArray.h>
#include <diagnostic_msgs/DiagnosticStatus.h>
#include <diagnostic_msgs/KeyValue.h>
#include <geometry_msgs/WrenchStamped.h>
#include <ros/ros.h>
#include <std_srvs/Trigger.h>

#include "netft_driver/client.hpp"
#include "netft_driver/adapter_config.hpp"
#include "netft_driver/status.hpp"

#include <array>
#include <exception>
#include <string>

namespace {

template<typename T>
void parameter(ros::NodeHandle & node, const char * name, T & value, const T & fallback)
{
  node.param(name, value, fallback);
}

class NetFTRos1Node final {
public:
  NetFTRos1Node(ros::NodeHandle public_node, ros::NodeHandle private_node)
  : public_node_(std::move(public_node)), private_node_(std::move(private_node)), client_(read_config()), evaluator_(expected_rdt_rate_, rate_tolerance_)
  {
    wrench_publisher_ = public_node_.advertise<geometry_msgs::WrenchStamped>(wrench_topic_, 10);
    diagnostics_publisher_ = public_node_.advertise<diagnostic_msgs::DiagnosticArray>("/diagnostics", 10);
    bias_service_ = public_node_.advertiseService(bias_service_name_, &NetFTRos1Node::handle_bias, this);
    diagnostic_timer_ = public_node_.createTimer(ros::Duration{1.0 / diagnostics_rate_}, &NetFTRos1Node::publish_diagnostics, this);
  }

  void start() { client_.start([this](const netft_driver::WrenchSample & sample) { publish_wrench(sample); }); }
  void stop() noexcept { client_.stop(); }
  const netft_driver::ClientConfig & config() const { return config_; }
  const std::string & frame_id() const { return frame_id_; }
  const std::string & wrench_topic() const { return wrench_topic_; }
  const std::string & bias_service() const { return bias_service_name_; }
  double diagnostics_rate() const { return diagnostics_rate_; }
  double expected_rdt_rate() const { return expected_rdt_rate_; }
  double rate_tolerance() const { return rate_tolerance_; }

private:
  netft_driver::ClientConfig read_config()
  {
    netft_driver::AdapterParameters parameters;
    parameter(private_node_, "sensor_ip", parameters.sensor_ip, parameters.sensor_ip);
    parameter(private_node_, "sensor_port", parameters.sensor_port, parameters.sensor_port);
    parameter(private_node_, "frame_id", parameters.frame_id, parameters.frame_id);
    parameter(private_node_, "wrench_topic", parameters.wrench_topic, parameters.wrench_topic);
    parameter(private_node_, "bias_service", parameters.bias_service, parameters.bias_service);
    parameter(private_node_, "counts_per_force", parameters.counts_per_force, parameters.counts_per_force);
    parameter(private_node_, "counts_per_torque", parameters.counts_per_torque, parameters.counts_per_torque);
    parameter(private_node_, "publish_rate", parameters.publish_rate, parameters.publish_rate);
    parameter(private_node_, "receive_timeout", parameters.receive_timeout, parameters.receive_timeout);
    parameter(private_node_, "reconnect_initial_delay", parameters.reconnect_initial_delay, parameters.reconnect_initial_delay);
    parameter(private_node_, "reconnect_max_delay", parameters.reconnect_max_delay, parameters.reconnect_max_delay);
    parameter(private_node_, "diagnostics_rate", parameters.diagnostics_rate, parameters.diagnostics_rate);
    parameter(private_node_, "expected_rdt_rate", parameters.expected_rdt_rate, parameters.expected_rdt_rate);
    parameter(private_node_, "rate_tolerance", parameters.rate_tolerance, parameters.rate_tolerance);
    parameter(private_node_, "publish_on_error", parameters.publish_on_error, parameters.publish_on_error);
    const auto mapped = netft_driver::map_adapter_parameters(parameters);
    config_ = mapped.client;
    frame_id_ = mapped.frame_id;
    wrench_topic_ = mapped.wrench_topic;
    bias_service_name_ = mapped.bias_service;
    diagnostics_rate_ = mapped.diagnostics_rate;
    expected_rdt_rate_ = mapped.expected_rdt_rate;
    rate_tolerance_ = mapped.rate_tolerance;
    return config_;
  }

  void publish_wrench(const netft_driver::WrenchSample & sample)
  {
    geometry_msgs::WrenchStamped message;
    message.header.stamp = ros::Time::now();
    message.header.frame_id = frame_id_;
    message.wrench.force.x = sample.force[0]; message.wrench.force.y = sample.force[1]; message.wrench.force.z = sample.force[2];
    message.wrench.torque.x = sample.torque[0]; message.wrench.torque.y = sample.torque[1]; message.wrench.torque.z = sample.torque[2];
    wrench_publisher_.publish(message);
  }

  bool handle_bias(std_srvs::Trigger::Request &, std_srvs::Trigger::Response & response)
  {
    try { client_.bias(); response.success = true; response.message = "software bias command sent and RDT streaming restarted"; }
    catch (const std::exception & error) { response.success = false; response.message = error.what(); }
    return true;
  }

  void publish_diagnostics(const ros::TimerEvent &)
  {
    const auto snapshot = client_.health_snapshot();
    const auto report = evaluator_.evaluate(snapshot);
    diagnostic_msgs::DiagnosticStatus status;
    status.level = static_cast<std::uint8_t>(report.level);
    status.name = "netft_driver: connection";
    status.message = report.message;
    status.hardware_id = snapshot.sensor_host + ":" + std::to_string(snapshot.sensor_port);
    for (const auto & value : report.values) {
      diagnostic_msgs::KeyValue key_value;
      key_value.key = value.first;
      key_value.value = value.second;
      status.values.push_back(std::move(key_value));
    }
    diagnostic_msgs::DiagnosticArray array;
    array.header.stamp = ros::Time::now();
    array.status.push_back(std::move(status));
    diagnostics_publisher_.publish(array);
  }

  ros::NodeHandle public_node_, private_node_;
  netft_driver::ClientConfig config_;
  std::string frame_id_, wrench_topic_, bias_service_name_;
  double diagnostics_rate_{1.0}, expected_rdt_rate_{2000.0}, rate_tolerance_{0.2};
  netft_driver::NetFTClient client_;
  netft_driver::DiagnosticEvaluator evaluator_{2000.0, 0.2};
  ros::Publisher wrench_publisher_, diagnostics_publisher_;
  ros::ServiceServer bias_service_;
  ros::Timer diagnostic_timer_;
};

}  // namespace

#ifndef NETFT_ADAPTER_NO_MAIN
int main(int argc, char ** argv)
{
  ros::init(argc, argv, "netft");
  try {
    ros::NodeHandle public_node, private_node{"~"};
    NetFTRos1Node node{public_node, private_node};
    node.start();
    ros::spin();
    node.stop();
    return 0;
  } catch (const std::exception & error) {
    ROS_FATAL_STREAM("invalid Net F/T configuration: " << error.what());
    return 2;
  }
}
#endif
