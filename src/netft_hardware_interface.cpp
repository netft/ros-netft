#include "ros/diagnostics.hpp"
#include "ros/ros2_control_compat.hpp"
#include "ros/unit_conversion.hpp"

#include "netft/client.hpp"

#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>
#include <hardware_interface/handle.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <pluginlib/class_list_macros.hpp>
#include <realtime_tools/realtime_buffer.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/expand_topic_or_service_name.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef NETFT_ROS2_CONTROL_TESTING
#include "ros/ros2_control_test_access.hpp"

#if defined(_WIN32)
#define NETFT_ROS2_CONTROL_TEST_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define NETFT_ROS2_CONTROL_TEST_EXPORT __attribute__((visibility("default")))
#else
#define NETFT_ROS2_CONTROL_TEST_EXPORT
#endif
#endif

namespace netft_driver {

class NetFTHardwareInterface;

#ifdef NETFT_ROS2_CONTROL_TESTING
namespace ros2_control_test_access::detail {

void force_initial_sample_once() noexcept;
void fail_state_write_once_at(std::size_t axis) noexcept;
void latch_active_fault(FaultCode fault) noexcept;
bool read_active_instance() noexcept;
FaultCode active_fault_code() noexcept;
FaultCode active_client_fault_code() noexcept;
FaultCode active_latched_fault_code() noexcept;
ActivityCounters active_activity_counters() noexcept;
bool interface_write_fault_latched() noexcept;
void throw_executor_cancel_once() noexcept;
int auxiliary_thread_count() noexcept;

}  // namespace ros2_control_test_access::detail
#endif

namespace {

constexpr std::array<const char *, 6> kInterfaceNames{
  "force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z"};
constexpr double kQuietNaN = std::numeric_limits<double>::quiet_NaN();
constexpr char kEncodedNamePrefix[] = "netft_encoded_";

#ifdef NETFT_ROS2_CONTROL_TESTING
std::atomic<NetFTHardwareInterface *> g_test_instance{nullptr};
std::atomic<bool> g_test_return_initial_sample{false};
std::atomic<int> g_test_failed_write_axis{-1};
std::atomic<bool> g_test_throw_executor_cancel{false};
std::atomic<int> g_test_auxiliary_threads{0};
const SiSample kInitialSample{};
#endif

double parse_double(
  const std::unordered_map<std::string, std::string> & parameters,
  const char * field, double fallback)
{
  const auto found = parameters.find(field);
  if (found == parameters.end()) return fallback;
  try {
    std::size_t consumed = 0;
    const double value = std::stod(found->second, &consumed);
    if (consumed != found->second.size() || !std::isfinite(value)) {
      throw std::invalid_argument{"not finite"};
    }
    return value;
  } catch (const std::exception &) {
    throw std::invalid_argument{std::string{field} + " must be a finite number"};
  }
}

int parse_port(
  const std::unordered_map<std::string, std::string> & parameters,
  const char * field, int fallback)
{
  const auto found = parameters.find(field);
  if (found == parameters.end()) return fallback;
  try {
    std::size_t consumed = 0;
    const long value = std::stol(found->second, &consumed, 10);
    if (consumed != found->second.size() || value < 1 || value > 65535) {
      throw std::invalid_argument{"out of range"};
    }
    return static_cast<int>(value);
  } catch (const std::exception &) {
    throw std::invalid_argument{
      std::string{field} + " must be an integer between 1 and 65535"};
  }
}

bool parse_bool(
  const std::unordered_map<std::string, std::string> & parameters,
  const char * field, bool fallback)
{
  const auto found = parameters.find(field);
  if (found == parameters.end()) return fallback;
  if (found->second == "true" || found->second == "True") return true;
  if (found->second == "false" || found->second == "False") return false;
  throw std::invalid_argument{std::string{field} + " must be true or false"};
}

std::int64_t steady_nanoseconds(std::chrono::steady_clock::time_point time) noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    time.time_since_epoch()).count();
}

bool is_ascii_letter(const unsigned char character) noexcept
{
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z');
}

bool is_ros_name_token(const std::string & sensor_name) noexcept
{
  if (sensor_name.empty()) return false;
  const auto first = static_cast<unsigned char>(sensor_name.front());
  if (!is_ascii_letter(first) && first != '_') return false;
  for (const unsigned char character : sensor_name) {
    if (!is_ascii_letter(character) &&
        !(character >= '0' && character <= '9') && character != '_') {
      return false;
    }
  }
  return true;
}

std::string collision_safe_name_token(const std::string & sensor_name)
{
  if (is_ros_name_token(sensor_name) && sensor_name.rfind(kEncodedNamePrefix, 0) != 0) {
    return sensor_name;
  }
  constexpr char kLowerHex[] = "0123456789abcdef";
  std::string result{kEncodedNamePrefix};
  result.reserve(result.size() + sensor_name.size() * 2);
  for (const unsigned char byte : sensor_name) {
    result.push_back(kLowerHex[byte >> 4]);
    result.push_back(kLowerHex[byte & 0x0f]);
  }
  return result;
}

std::string auxiliary_node_name(const std::string & ros_name_token)
{
  return "netft_" + ros_name_token + "_hardware";
}

}  // namespace

class NetFTHardwareInterface final : public hardware_interface::SensorInterface {
public:
  NetFTHardwareInterface()
  {
#ifdef NETFT_ROS2_CONTROL_TESTING
    g_test_instance.store(this, std::memory_order_release);
#endif
  }

  ~NetFTHardwareInterface() override
  {
    stop_auxiliary();
    if (client_) client_->stop();
    client_.reset();
#ifdef NETFT_ROS2_CONTROL_TESTING
    auto * expected = this;
    (void)g_test_instance.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel);
#endif
  }

  hardware_interface::CallbackReturn on_init(
    const ros2_control_compat::InitArgument & argument) override
  {
    if (hardware_interface::SensorInterface::on_init(argument) !=
        hardware_interface::CallbackReturn::SUCCESS) {
      return hardware_interface::CallbackReturn::ERROR;
    }
    try {
      const auto & info = ros2_control_compat::hardware_info(argument);
      validate_hardware_info(info);
      parse_parameters(info.hardware_parameters);
      invalidate_interfaces();
      return hardware_interface::CallbackReturn::SUCCESS;
    } catch (const std::exception & error) {
      RCLCPP_ERROR(
        rclcpp::get_logger("netft_driver.NetFTHardwareInterface"), "%s", error.what());
      return hardware_interface::CallbackReturn::ERROR;
    }
  }

#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  std::vector<hardware_interface::StateInterface> export_state_interfaces() override
  {
    std::vector<hardware_interface::StateInterface> interfaces;
    interfaces.reserve(kInterfaceNames.size());
    for (std::size_t index = 0; index < kInterfaceNames.size(); ++index) {
      interfaces.emplace_back(sensor_name_, kInterfaceNames[index], &state_values_[index]);
    }
    return interfaces;
  }
#endif

  hardware_interface::CallbackReturn on_configure(
    const rclcpp_lifecycle::State &) override
  {
    stop_auxiliary();
    if (client_) client_->stop();
    client_.reset();
    sample_generation_.store(0, std::memory_order_release);
    interface_write_fault_.store(false, std::memory_order_release);
    fatal_fault_.store(netft::FaultCode::None, std::memory_order_release);
    sample_buffer_.initRT(SiSample{});
    invalidate_interfaces();
    try {
      client_ = std::make_unique<netft::Client>(client_config_);
      start_auxiliary();
      return hardware_interface::CallbackReturn::SUCCESS;
    } catch (const std::exception &) {
      stop_auxiliary();
      client_.reset();
      return hardware_interface::CallbackReturn::ERROR;
    }
  }

  hardware_interface::CallbackReturn on_activate(
    const rclcpp_lifecycle::State &) override
  {
    if (!client_) return hardware_interface::CallbackReturn::ERROR;
#ifdef NETFT_ROS2_CONTROL_MODERN_API
    try {
      for (std::size_t index = 0; index < kInterfaceNames.size(); ++index) {
        state_handles_[index] = get_state_interface_handle(
          sensor_name_ + "/" + kInterfaceNames[index]);
      }
    } catch (const std::exception &) {
      invalidate_interfaces();
      return hardware_interface::CallbackReturn::ERROR;
    }
#endif
    sample_generation_.store(0, std::memory_order_release);
    invalidate_interfaces();
    try {
      client_->start([this](const netft::Sample & sample) {
        sample_buffer_.writeFromNonRT(to_si_sample(sample));
        sample_generation_.fetch_add(1, std::memory_order_release);
      });
    } catch (const std::exception &) {
      invalidate_interfaces();
      return hardware_interface::CallbackReturn::ERROR;
    }
    if (!client_->wait_for_first_sample(activation_timeout_) || client_->faulted() ||
        sample_generation_.load(std::memory_order_acquire) == 0) {
      client_->stop();
      invalidate_interfaces();
      return hardware_interface::CallbackReturn::ERROR;
    }
    return hardware_interface::CallbackReturn::SUCCESS;
  }

  hardware_interface::CallbackReturn on_deactivate(
    const rclcpp_lifecycle::State &) override
  {
    stop_and_invalidate();
    return hardware_interface::CallbackReturn::SUCCESS;
  }

  hardware_interface::CallbackReturn on_error(
    const rclcpp_lifecycle::State &) override
  {
    stop_and_invalidate();
    return hardware_interface::CallbackReturn::SUCCESS;
  }

  hardware_interface::CallbackReturn on_cleanup(
    const rclcpp_lifecycle::State &) override
  {
    stop_and_invalidate();
    stop_auxiliary();
    client_.reset();
    return hardware_interface::CallbackReturn::SUCCESS;
  }

  hardware_interface::CallbackReturn on_shutdown(
    const rclcpp_lifecycle::State &) override
  {
    stop_and_invalidate();
    stop_auxiliary();
    client_.reset();
    return hardware_interface::CallbackReturn::SUCCESS;
  }

  hardware_interface::return_type read(
    const rclcpp::Time &, const rclcpp::Duration &) override
  {
    const SiSample * sample = nullptr;
#ifdef NETFT_ROS2_CONTROL_TESTING
    if (g_test_return_initial_sample.exchange(false, std::memory_order_acq_rel)) {
      sample = &kInitialSample;
    } else {
      sample = sample_buffer_.readFromRT();
    }
#else
    sample = sample_buffer_.readFromRT();
#endif
    if (fault_latched() || interface_write_fault_.load(std::memory_order_acquire) ||
        sample == nullptr) {
      invalidate_interfaces();
      return hardware_interface::return_type::ERROR;
    }
    if (sample_is_stale(*sample)) {
      latch_fatal_fault(netft::FaultCode::Timeout);
      invalidate_interfaces();
      return hardware_interface::return_type::ERROR;
    }
    if (!write_axes(*sample)) {
      interface_write_fault_.store(true, std::memory_order_release);
      invalidate_interfaces();
      return hardware_interface::return_type::ERROR;
    }
    return hardware_interface::return_type::OK;
  }

private:
#ifdef NETFT_ROS2_CONTROL_TESTING
  friend ros2_control_test_access::FaultCode
    ros2_control_test_access::detail::active_fault_code() noexcept;
  friend ros2_control_test_access::FaultCode
    ros2_control_test_access::detail::active_client_fault_code() noexcept;
  friend ros2_control_test_access::FaultCode
    ros2_control_test_access::detail::active_latched_fault_code() noexcept;
  friend ros2_control_test_access::ActivityCounters
    ros2_control_test_access::detail::active_activity_counters() noexcept;
  friend bool ros2_control_test_access::detail::interface_write_fault_latched() noexcept;
  friend void ros2_control_test_access::detail::latch_active_fault(
    ros2_control_test_access::FaultCode) noexcept;
#endif

  void validate_hardware_info(const hardware_interface::HardwareInfo & info)
  {
    if (!info.joints.empty() || !info.gpios.empty() || !info.transmissions.empty()) {
      throw std::invalid_argument{
        "sensor hardware must not declare joints, GPIOs, or transmissions"};
    }
    if (info.sensors.size() != 1) {
      throw std::invalid_argument{"exactly one sensor is required"};
    }
    const auto & sensor = info.sensors.front();
    if (sensor.name.empty()) throw std::invalid_argument{"sensor name must be non-empty"};
    if (sensor.state_interfaces.size() != kInterfaceNames.size()) {
      throw std::invalid_argument{"sensor must declare exactly six state interfaces"};
    }
    for (std::size_t index = 0; index < kInterfaceNames.size(); ++index) {
      const auto & interface = sensor.state_interfaces[index];
      if (interface.name != kInterfaceNames[index]) {
        throw std::invalid_argument{
          "state interfaces must be force.x, force.y, force.z, torque.x, torque.y, torque.z"};
      }
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
      if (!interface.data_type.empty() && interface.data_type != "double") {
#else
      if (interface.data_type != "double") {
#endif
        throw std::invalid_argument{"all state interfaces must use data_type double"};
      }
#ifdef NETFT_ROS2_CONTROL_MODERN_API
      if (interface.size != 1) {
        throw std::invalid_argument{"all state interfaces must be scalar (size 1)"};
      }
#endif
      if (!interface.initial_value.empty()) {
        try {
          std::size_t consumed = 0;
          const auto initial_value = std::stod(interface.initial_value, &consumed);
          if (consumed != interface.initial_value.size() || !std::isfinite(initial_value)) {
            throw std::invalid_argument{"not finite"};
          }
        } catch (const std::exception &) {
          throw std::invalid_argument{
            "state interface initial_value must be a finite double"};
        }
      }
    }
    sensor_name_ = sensor.name;
  }

  void parse_parameters(
    const std::unordered_map<std::string, std::string> & parameters)
  {
    netft::Config config;
    const auto host = parameters.find("sensor_ip");
    if (host != parameters.end()) config.sensor_host = host->second;
    if (config.sensor_host.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
      throw std::invalid_argument{"sensor_ip must be non-empty"};
    }
    config.rdt_port = parse_port(parameters, "sensor_port", config.rdt_port);
    config.http_port = parse_port(parameters, "http_port", config.http_port);
    config.receive_timeout = std::chrono::duration<double>{parse_double(
      parameters, "receive_timeout", config.receive_timeout.count())};
    config.configuration_connect_timeout = std::chrono::duration<double>{parse_double(
      parameters, "configuration_connect_timeout",
      config.configuration_connect_timeout.count())};
    config.configuration_timeout = std::chrono::duration<double>{parse_double(
      parameters, "configuration_timeout", config.configuration_timeout.count())};
    if (!parse_bool(parameters, "use_sensor_calibration", true)) {
      config.calibration_override = netft::Calibration{
        parse_double(parameters, "counts_per_force", 1000000.0),
        parse_double(parameters, "counts_per_torque", 1000000.0),
        netft::ForceUnit::Newton,
        netft::TorqueUnit::NewtonMeter,
      };
    }
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    netft::validate(config);

    activation_timeout_ = std::chrono::duration<double>{parse_double(
      parameters, "activation_timeout", 2.0)};
    diagnostics_rate_ = parse_double(parameters, "diagnostics_rate", 1.0);
    expected_rdt_rate_ = parse_double(parameters, "expected_rdt_rate", 2000.0);
    rate_tolerance_ = parse_double(parameters, "rate_tolerance", 0.2);
    const auto bias_service = parameters.find("bias_service");
    ros_name_token_ = collision_safe_name_token(sensor_name_);
    bias_service_name_ = bias_service == parameters.end() ?
      "/" + ros_name_token_ + "/bias" : bias_service->second;
    if (activation_timeout_.count() <= 0.0) {
      throw std::invalid_argument{"activation_timeout must be greater than zero"};
    }
    if (diagnostics_rate_ <= 0.0) {
      throw std::invalid_argument{"diagnostics_rate must be greater than zero"};
    }
    if (expected_rdt_rate_ <= 0.0) {
      throw std::invalid_argument{"expected_rdt_rate must be greater than zero"};
    }
    if (rate_tolerance_ < 0.0 || rate_tolerance_ > 1.0) {
      throw std::invalid_argument{"rate_tolerance must be between zero and one"};
    }
    try {
      (void)rclcpp::expand_topic_or_service_name(
        bias_service_name_, auxiliary_node_name(ros_name_token_), "/", true);
    } catch (const std::exception & error) {
      throw std::invalid_argument{
        "bias_service must be a valid ROS service name: " + std::string{error.what()}};
    }
    client_config_ = std::move(config);
  }

  void start_auxiliary()
  {
    DiagnosticEvaluator evaluator{expected_rdt_rate_, rate_tolerance_};
    evaluator_ = std::make_unique<DiagnosticEvaluator>(std::move(evaluator));
    const auto options = rclcpp::NodeOptions{}.use_global_arguments(false);
    auxiliary_node_ = std::make_shared<rclcpp::Node>(
      auxiliary_node_name(ros_name_token_), options);
    diagnostics_publisher_ = auxiliary_node_->create_publisher<
      diagnostic_msgs::msg::DiagnosticArray>("/diagnostics", rclcpp::QoS{10}.reliable());
    bias_service_ = auxiliary_node_->create_service<std_srvs::srv::Trigger>(
      bias_service_name_,
      [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        try {
          if (!client_) throw std::runtime_error{"client is not configured"};
          client_->bias();
          response->success = true;
          response->message = "software bias command sent and RDT streaming restarted";
        } catch (const std::exception & error) {
          response->success = false;
          response->message = error.what();
        }
      });
    diagnostic_timer_ = auxiliary_node_->create_wall_timer(
      std::chrono::duration<double>{1.0 / diagnostics_rate_},
      [this] { publish_diagnostics(); });
    auxiliary_executor_ = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    auxiliary_executor_->add_node(auxiliary_node_);
    auxiliary_stopping_.store(false, std::memory_order_release);
    auxiliary_thread_ = std::thread([this] {
#ifdef NETFT_ROS2_CONTROL_TESTING
      g_test_auxiliary_threads.fetch_add(1, std::memory_order_acq_rel);
#endif
      try {
        while (!auxiliary_stopping_.load(std::memory_order_acquire)) {
          auxiliary_executor_->spin_some(std::chrono::milliseconds{20});
        }
      } catch (const std::exception &) {
      }
#ifdef NETFT_ROS2_CONTROL_TESTING
      g_test_auxiliary_threads.fetch_sub(1, std::memory_order_acq_rel);
#endif
    });
  }

  void stop_auxiliary() noexcept
  {
    auxiliary_stopping_.store(true, std::memory_order_release);
    if (auxiliary_executor_) {
      try {
#ifdef NETFT_ROS2_CONTROL_TESTING
        if (g_test_throw_executor_cancel.exchange(false, std::memory_order_acq_rel)) {
          throw std::runtime_error{"injected executor cancel failure"};
        }
#endif
        auxiliary_executor_->cancel();
      } catch (const std::exception &) {
      }
    }
    if (auxiliary_thread_.joinable()) auxiliary_thread_.join();
    if (auxiliary_executor_ && auxiliary_node_) {
      try {
        auxiliary_executor_->remove_node(auxiliary_node_);
      } catch (const std::exception &) {
      }
    }
    diagnostic_timer_.reset();
    bias_service_.reset();
    diagnostics_publisher_.reset();
    auxiliary_executor_.reset();
    auxiliary_node_.reset();
    evaluator_.reset();
  }

  void publish_diagnostics()
  {
    if (!client_ || !evaluator_ || !diagnostics_publisher_) return;
    auto snapshot = client_->health();
    const auto plugin_fault = fatal_fault_.load(std::memory_order_acquire);
    if (plugin_fault != netft::FaultCode::None &&
        snapshot.fault_code == netft::FaultCode::None) {
      snapshot.state = netft::ClientState::Faulted;
      snapshot.fault_code = plugin_fault;
      if (snapshot.last_error.empty() && plugin_fault == netft::FaultCode::Timeout) {
        snapshot.last_error = "no valid RDT record before timeout";
      }
    }
    const auto report = evaluator_->evaluate(snapshot);
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.level = static_cast<std::uint8_t>(report.level);
    status.name = "netft_driver: " + sensor_name_;
    status.message = report.message;
    status.hardware_id = snapshot.sensor_host + ":" + std::to_string(snapshot.rdt_port);
    status.values.reserve(report.values.size());
    for (const auto & value : report.values) {
      diagnostic_msgs::msg::KeyValue item;
      item.key = value.first;
      item.value = value.second;
      status.values.push_back(std::move(item));
    }
    diagnostic_msgs::msg::DiagnosticArray message;
    message.header.stamp = auxiliary_node_->now();
    message.status.push_back(std::move(status));
    diagnostics_publisher_->publish(message);
  }

  void latch_fatal_fault(const netft::FaultCode fault) noexcept
  {
    auto expected = netft::FaultCode::None;
    (void)fatal_fault_.compare_exchange_strong(
      expected, fault, std::memory_order_acq_rel);
  }

  bool fault_latched() noexcept
  {
    if (!client_) return true;
    if (client_->faulted() && client_->fault_code() != netft::FaultCode::None) {
      latch_fatal_fault(client_->fault_code());
    }
    return fatal_fault_.load(std::memory_order_acquire) != netft::FaultCode::None;
  }

  bool sample_is_stale(const SiSample & sample) const noexcept
  {
    const auto received_ns = steady_nanoseconds(sample.received_at);
    const auto now_ns = steady_nanoseconds(std::chrono::steady_clock::now());
    const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      client_config_.receive_timeout).count();
    return received_ns <= 0 || now_ns - received_ns > timeout_ns;
  }

  bool write_axes(const SiSample & sample) noexcept
  {
    const std::array<double, 6> values{
      sample.force[0], sample.force[1], sample.force[2],
      sample.torque[0], sample.torque[1], sample.torque[2]};
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
    state_values_ = values;
    return true;
#else
    for (std::size_t index = 0; index < values.size(); ++index) {
#ifdef NETFT_ROS2_CONTROL_TESTING
      int expected = static_cast<int>(index);
      if (g_test_failed_write_axis.compare_exchange_strong(
          expected, -1, std::memory_order_acq_rel)) {
        return false;
      }
#endif
      if (!set_state(state_handles_[index], values[index], false)) return false;
    }
    return true;
#endif
  }

  void invalidate_interfaces() noexcept
  {
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
    state_values_.fill(kQuietNaN);
#else
    for (const auto & handle : state_handles_) {
      if (handle) (void)set_state(handle, kQuietNaN, false);
    }
#endif
  }

  void stop_and_invalidate() noexcept
  {
    if (client_) client_->stop();
    sample_generation_.store(0, std::memory_order_release);
    invalidate_interfaces();
  }

  netft::Config client_config_{};
  std::chrono::duration<double> activation_timeout_{2.0};
  double diagnostics_rate_{1.0};
  double expected_rdt_rate_{2000.0};
  double rate_tolerance_{0.2};
  std::string sensor_name_;
  std::string ros_name_token_;
  std::string bias_service_name_;
  std::unique_ptr<netft::Client> client_;
  std::unique_ptr<DiagnosticEvaluator> evaluator_;
  std::shared_ptr<rclcpp::Node> auxiliary_node_;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> auxiliary_executor_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_publisher_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr bias_service_;
  rclcpp::TimerBase::SharedPtr diagnostic_timer_;
  std::thread auxiliary_thread_;
  std::atomic<bool> auxiliary_stopping_{false};
  realtime_tools::RealtimeBuffer<SiSample> sample_buffer_;
  std::atomic<std::uint64_t> sample_generation_{0};
  std::atomic<bool> interface_write_fault_{false};
  std::atomic<netft::FaultCode> fatal_fault_{netft::FaultCode::None};
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  std::array<double, 6> state_values_{};
#else
  std::array<hardware_interface::StateInterface::SharedPtr, 6> state_handles_{};
#endif
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<netft::FaultCode>::is_always_lock_free);

#ifdef NETFT_ROS2_CONTROL_TESTING
namespace ros2_control_test_access::detail {
namespace {

FaultCode to_test_fault(const netft::FaultCode fault) noexcept
{
  switch (fault) {
    case netft::FaultCode::None: return FaultCode::None;
    case netft::FaultCode::SensorConfiguration: return FaultCode::SensorConfiguration;
    case netft::FaultCode::Timeout: return FaultCode::Timeout;
    case netft::FaultCode::Socket: return FaultCode::Socket;
    case netft::FaultCode::SeriousStatus: return FaultCode::SeriousStatus;
    case netft::FaultCode::FtStall: return FaultCode::FtStall;
    case netft::FaultCode::FtBackward: return FaultCode::FtBackward;
    case netft::FaultCode::MalformedStorm: return FaultCode::MalformedStorm;
    case netft::FaultCode::Callback: return FaultCode::Callback;
  }
  return FaultCode::None;
}

netft::FaultCode to_client_fault(const FaultCode fault) noexcept
{
  switch (fault) {
    case FaultCode::None: return netft::FaultCode::None;
    case FaultCode::SensorConfiguration: return netft::FaultCode::SensorConfiguration;
    case FaultCode::Timeout: return netft::FaultCode::Timeout;
    case FaultCode::Socket: return netft::FaultCode::Socket;
    case FaultCode::SeriousStatus: return netft::FaultCode::SeriousStatus;
    case FaultCode::FtStall: return netft::FaultCode::FtStall;
    case FaultCode::FtBackward: return netft::FaultCode::FtBackward;
    case FaultCode::MalformedStorm: return netft::FaultCode::MalformedStorm;
    case FaultCode::Callback: return netft::FaultCode::Callback;
  }
  return netft::FaultCode::None;
}

}  // namespace

void force_initial_sample_once() noexcept
{
  g_test_return_initial_sample.store(true, std::memory_order_release);
}

void fail_state_write_once_at(const std::size_t axis) noexcept
{
  g_test_failed_write_axis.store(static_cast<int>(axis), std::memory_order_release);
}

void latch_active_fault(const FaultCode fault) noexcept
{
  auto * instance = g_test_instance.load(std::memory_order_acquire);
  if (instance != nullptr) instance->latch_fatal_fault(to_client_fault(fault));
}

bool read_active_instance() noexcept
{
  auto * instance = g_test_instance.load(std::memory_order_acquire);
  if (instance == nullptr) return false;
  return instance->read(
    rclcpp::Time{0, 0, RCL_STEADY_TIME}, rclcpp::Duration::from_seconds(0.001)) ==
         hardware_interface::return_type::OK;
}

FaultCode active_fault_code() noexcept
{
  const auto * instance = g_test_instance.load(std::memory_order_acquire);
  if (instance == nullptr) return FaultCode::None;
  const auto latched = instance->fatal_fault_.load(std::memory_order_acquire);
  if (latched != netft::FaultCode::None || !instance->client_) {
    return to_test_fault(latched);
  }
  return to_test_fault(instance->client_->fault_code());
}

FaultCode active_client_fault_code() noexcept
{
  const auto * instance = g_test_instance.load(std::memory_order_acquire);
  if (instance == nullptr || !instance->client_) return FaultCode::None;
  return to_test_fault(instance->client_->fault_code());
}

FaultCode active_latched_fault_code() noexcept
{
  const auto * instance = g_test_instance.load(std::memory_order_acquire);
  if (instance == nullptr) return FaultCode::None;
  return to_test_fault(instance->fatal_fault_.load(std::memory_order_acquire));
}

ActivityCounters active_activity_counters() noexcept
{
  const auto * instance = g_test_instance.load(std::memory_order_acquire);
  if (instance == nullptr) return {};
  return {
    instance->sample_generation_.load(std::memory_order_acquire),
  };
}

bool interface_write_fault_latched() noexcept
{
  const auto * instance = g_test_instance.load(std::memory_order_acquire);
  return instance != nullptr &&
         instance->interface_write_fault_.load(std::memory_order_acquire);
}

void throw_executor_cancel_once() noexcept
{
  g_test_throw_executor_cancel.store(true, std::memory_order_release);
}

int auxiliary_thread_count() noexcept
{
  return g_test_auxiliary_threads.load(std::memory_order_acquire);
}

}  // namespace ros2_control_test_access::detail
#endif

}  // namespace netft_driver

#ifdef NETFT_ROS2_CONTROL_TESTING
extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT void
netft_ros2_control_test_force_initial_sample_once() noexcept
{
  netft_driver::ros2_control_test_access::detail::force_initial_sample_once();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT void netft_ros2_control_test_fail_state_write_once_at(
  const std::size_t axis) noexcept
{
  netft_driver::ros2_control_test_access::detail::fail_state_write_once_at(axis);
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT void netft_ros2_control_test_latch_active_fault(
  const netft_driver::ros2_control_test_access::FaultCode fault) noexcept
{
  netft_driver::ros2_control_test_access::detail::latch_active_fault(fault);
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT bool
netft_ros2_control_test_read_active_instance() noexcept
{
  return netft_driver::ros2_control_test_access::detail::read_active_instance();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT netft_driver::ros2_control_test_access::FaultCode
netft_ros2_control_test_active_fault_code() noexcept
{
  return netft_driver::ros2_control_test_access::detail::active_fault_code();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT netft_driver::ros2_control_test_access::FaultCode
netft_ros2_control_test_active_client_fault_code() noexcept
{
  return netft_driver::ros2_control_test_access::detail::active_client_fault_code();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT netft_driver::ros2_control_test_access::FaultCode
netft_ros2_control_test_active_latched_fault_code() noexcept
{
  return netft_driver::ros2_control_test_access::detail::active_latched_fault_code();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT netft_driver::ros2_control_test_access::ActivityCounters
netft_ros2_control_test_active_activity_counters() noexcept
{
  return netft_driver::ros2_control_test_access::detail::active_activity_counters();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT bool
netft_ros2_control_test_interface_write_fault_latched() noexcept
{
  return netft_driver::ros2_control_test_access::detail::interface_write_fault_latched();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT void
netft_ros2_control_test_throw_executor_cancel_once() noexcept
{
  netft_driver::ros2_control_test_access::detail::throw_executor_cancel_once();
}

extern "C" NETFT_ROS2_CONTROL_TEST_EXPORT int
netft_ros2_control_test_auxiliary_thread_count() noexcept
{
  return netft_driver::ros2_control_test_access::detail::auxiliary_thread_count();
}

#undef NETFT_ROS2_CONTROL_TEST_EXPORT
#endif

PLUGINLIB_EXPORT_CLASS(
  netft_driver::NetFTHardwareInterface, hardware_interface::SensorInterface)
