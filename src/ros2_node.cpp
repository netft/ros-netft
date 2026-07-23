#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>
#include <geometry_msgs/msg/wrench_stamped.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/create_timer.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "netft/client.hpp"
#include "ros/adapter_config.hpp"
#include "ros/diagnostics.hpp"
#include "ros/unit_conversion.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <string>

class NetFTRos2Node final : public rclcpp::Node {
public:
  explicit NetFTRos2Node(const rclcpp::NodeOptions & options = rclcpp::NodeOptions{})
  : Node{"netft", options}, client_(read_config()), evaluator_(expected_rdt_rate_, rate_tolerance_)
  {
    wrench_publisher_ = create_publisher<geometry_msgs::msg::WrenchStamped>(wrench_topic_, rclcpp::SensorDataQoS{});
    diagnostics_publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>("/diagnostics", rclcpp::QoS{10}.reliable());
    bias_service_ = create_service<std_srvs::srv::Trigger>(bias_service_name_, [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>, std::shared_ptr<std_srvs::srv::Trigger::Response> response) { handle_bias(*response); });
    diagnostic_timer_ = rclcpp::create_timer(this, get_clock(), std::chrono::duration<double>{1.0 / diagnostics_rate_}, [this] { publish_diagnostics(); });
  }

  void start() { client_.start([this](const netft::Sample & sample) { publish_wrench(sample); }); }
  void stop() noexcept { client_.stop(); }
  const netft::Config & config() const { return config_; }
  const std::string & frame_id() const { return frame_id_; }
  const std::string & wrench_topic() const { return wrench_topic_; }
  const std::string & bias_service() const { return bias_service_name_; }
  double diagnostics_rate() const { return diagnostics_rate_; }
  double expected_rdt_rate() const { return expected_rdt_rate_; }
  double rate_tolerance() const { return rate_tolerance_; }

private:
  template<typename T>
  T parameter(const std::string & name, const T & fallback)
  {
    return declare_parameter<T>(name, fallback);
  }

  netft::Config read_config()
  {
    netft_driver::AdapterParameters parameters;
    parameters.sensor_ip = parameter<std::string>("sensor_ip", parameters.sensor_ip);
    parameters.sensor_port = parameter<int>("sensor_port", parameters.sensor_port);
    parameters.http_port = parameter<int>("http_port", parameters.http_port);
    parameters.frame_id = parameter<std::string>("frame_id", parameters.frame_id);
    parameters.wrench_topic = parameter<std::string>("wrench_topic", parameters.wrench_topic);
    parameters.bias_service = parameter<std::string>("bias_service", parameters.bias_service);
    parameters.use_sensor_calibration =
      parameter<bool>("use_sensor_calibration", parameters.use_sensor_calibration);
    parameters.counts_per_force = parameter<double>("counts_per_force", parameters.counts_per_force);
    parameters.counts_per_torque = parameter<double>("counts_per_torque", parameters.counts_per_torque);
    parameters.publish_rate = parameter<double>("publish_rate", parameters.publish_rate);
    parameters.receive_timeout = parameter<double>("receive_timeout", parameters.receive_timeout);
    parameters.configuration_connect_timeout = parameter<double>(
      "configuration_connect_timeout", parameters.configuration_connect_timeout);
    parameters.configuration_timeout =
      parameter<double>("configuration_timeout", parameters.configuration_timeout);
    parameters.reconnect_initial_delay =
      parameter<double>("reconnect_initial_delay", parameters.reconnect_initial_delay);
    parameters.reconnect_max_delay = parameter<double>("reconnect_max_delay", parameters.reconnect_max_delay);
    parameters.diagnostics_rate = parameter<double>("diagnostics_rate", parameters.diagnostics_rate);
    parameters.expected_rdt_rate = parameter<double>("expected_rdt_rate", parameters.expected_rdt_rate);
    parameters.rate_tolerance = parameter<double>("rate_tolerance", parameters.rate_tolerance);
    parameters.publish_on_error = parameter<bool>("publish_on_error", parameters.publish_on_error);
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

  void publish_wrench(const netft::Sample & sample)
  {
    const auto si = netft_driver::to_si_sample(sample);
    geometry_msgs::msg::WrenchStamped message;
    message.header.stamp = get_clock()->now();
    message.header.frame_id = frame_id_;
    message.wrench.force.x = si.force[0]; message.wrench.force.y = si.force[1]; message.wrench.force.z = si.force[2];
    message.wrench.torque.x = si.torque[0]; message.wrench.torque.y = si.torque[1]; message.wrench.torque.z = si.torque[2];
    wrench_publisher_->publish(message);
  }

  void handle_bias(std_srvs::srv::Trigger::Response & response)
  {
    try { client_.bias(); response.success = true; response.message = "software bias command sent and RDT streaming restarted"; }
    catch (const std::exception & error) { response.success = false; response.message = error.what(); }
  }

  void publish_diagnostics()
  {
    const auto snapshot = client_.health();
    const auto report = evaluator_.evaluate(snapshot);
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.level = static_cast<std::uint8_t>(report.level);
    status.name = "netft_driver: connection";
    status.message = report.message;
    status.hardware_id = snapshot.sensor_host + ":" + std::to_string(snapshot.rdt_port);
    for (const auto & value : report.values) {
      diagnostic_msgs::msg::KeyValue key_value;
      key_value.key = value.first;
      key_value.value = value.second;
      status.values.push_back(std::move(key_value));
    }
    diagnostic_msgs::msg::DiagnosticArray array;
    array.header.stamp = get_clock()->now();
    array.status.push_back(std::move(status));
    diagnostics_publisher_->publish(array);
  }

  netft::Config config_;
  std::string frame_id_, wrench_topic_, bias_service_name_;
  double diagnostics_rate_{1.0}, expected_rdt_rate_{2000.0}, rate_tolerance_{0.2};
  netft::Client client_;
  netft_driver::DiagnosticEvaluator evaluator_{2000.0, 0.2};
  rclcpp::Publisher<geometry_msgs::msg::WrenchStamped>::SharedPtr wrench_publisher_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_publisher_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr bias_service_;
  rclcpp::TimerBase::SharedPtr diagnostic_timer_;
};

#ifndef NETFT_ADAPTER_NO_MAIN
int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  std::shared_ptr<NetFTRos2Node> node;
  int result = 0;
  try {
    node = std::make_shared<NetFTRos2Node>();
    node->start();
    rclcpp::spin(node);
  } catch (const std::exception & error) {
    if (rclcpp::ok()) RCLCPP_FATAL(rclcpp::get_logger("netft"), "invalid Net F/T configuration: %s", error.what());
    else std::cerr << "invalid Net F/T configuration: " << error.what() << '\n';
    result = 2;
  }
  if (node) { node->stop(); node.reset(); }
  if (rclcpp::ok()) rclcpp::shutdown();
  return result;
}
#endif
