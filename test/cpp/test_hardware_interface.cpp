#include "support/fake_sensor.hpp"

#include "netft_driver/ros2_control_compat.hpp"

#include <gtest/gtest.h>

#include <hardware_interface/resource_manager.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp_lifecycle/state.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

namespace netft_driver {
namespace {

constexpr const char * kHardwareName = "netft_hardware";
constexpr const char * kSensorName = "netft_sensor";

std::string urdf(
  const std::string & host, int port,
  const std::string & parameters = {},
  const std::vector<std::string> & interfaces = {
    "force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z"},
  const std::string & extra_component = {},
  const std::string & data_type = {})
{
  std::string state_interfaces;
  for (const auto & name : interfaces) {
    state_interfaces += "<state_interface name=\"" + name + "\"";
    if (!data_type.empty()) state_interfaces += " data_type=\"" + data_type + "\"";
    state_interfaces += "/>";
  }
  return "<?xml version=\"1.0\"?><robot name=\"test\">"
    "<link name=\"base\"/><ros2_control name=\"" + std::string{kHardwareName} +
    "\" type=\"sensor\"><hardware>"
    "<plugin>netft_driver/NetFTHardwareInterface</plugin>"
    "<param name=\"sensor_ip\">" + host + "</param>"
    "<param name=\"sensor_port\">" + std::to_string(port) + "</param>"
    "<param name=\"counts_per_force\">100</param>"
    "<param name=\"counts_per_torque\">10</param>"
    "<param name=\"receive_timeout\">0.15</param>"
    "<param name=\"activation_timeout\">0.5</param>"
    "<param name=\"diagnostics_rate\">1.0</param>"
    "<param name=\"expected_rdt_rate\">200</param>"
    "<param name=\"rate_tolerance\">0.2</param>" + parameters +
    "</hardware><sensor name=\"" + std::string{kSensorName} + "\">" +
    state_interfaces + "</sensor>" + extra_component + "</ros2_control></robot>";
}

std::unique_ptr<hardware_interface::ResourceManager> make_manager(const std::string & description)
{
  if (!rclcpp::ok()) {
    int argc = 0;
    rclcpp::init(argc, nullptr);
  }
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  return std::make_unique<hardware_interface::ResourceManager>(description, true, false);
#else
  auto clock = std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
  return std::make_unique<hardware_interface::ResourceManager>(
    description, clock, rclcpp::get_logger("netft_hardware_test"), false, 1000);
#endif
}

std::shared_ptr<std_srvs::srv::Trigger::Response> call_bias(
  const std::string & service_name, std::chrono::milliseconds timeout = 1s)
{
  auto node = std::make_shared<rclcpp::Node>("netft_hardware_test_client");
  auto client = node->create_client<std_srvs::srv::Trigger>(service_name);
  if (!client->wait_for_service(timeout)) return nullptr;
  auto future = client->async_send_request(
    std::make_shared<std_srvs::srv::Trigger::Request>());
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  if (executor.spin_until_future_complete(future, timeout) !=
      rclcpp::FutureReturnCode::SUCCESS) {
    return nullptr;
  }
  return future.get();
}

std::string two_sensor_urdf(
  int left_port, int right_port,
  const std::string & left_sensor_name = "left_ft",
  const std::string & right_sensor_name = "right_ft")
{
  const auto component = [](const std::string & hardware_name,
      const std::string & sensor_name, int port) {
    std::string interfaces;
    for (const auto * name :
         {"force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z"}) {
      interfaces += "<state_interface name=\"" + std::string{name} + "\"/>";
    }
    return "<ros2_control name=\"" + hardware_name + "\" type=\"sensor\">"
      "<hardware><plugin>netft_driver/NetFTHardwareInterface</plugin>"
      "<param name=\"sensor_ip\">127.0.0.1</param>"
      "<param name=\"sensor_port\">" + std::to_string(port) + "</param>"
      "<param name=\"counts_per_force\">100</param>"
      "<param name=\"counts_per_torque\">10</param>"
      "<param name=\"receive_timeout\">0.08</param>"
      "<param name=\"activation_timeout\">0.5</param>"
      "<param name=\"diagnostics_rate\">20</param>"
      "<param name=\"expected_rdt_rate\">200</param>"
      "<param name=\"rate_tolerance\">0.2</param></hardware>"
      "<sensor name=\"" + sensor_name + "\">" + interfaces + "</sensor>"
      "</ros2_control>";
  };
  return "<?xml version=\"1.0\"?><robot name=\"dual\"><link name=\"base\"/>" +
    component("left_hardware", left_sensor_name, left_port) +
    component("right_hardware", right_sensor_name, right_port) + "</robot>";
}

std::string urdf_with_sensor_name(
  const std::string & host, int port, const std::string & sensor_name)
{
  auto description = urdf(host, port);
  const auto position = description.find(kSensorName);
  if (position == std::string::npos) return {};
  description.replace(position, std::string{kSensorName}.size(), sensor_name);
  return description;
}

bool manager_rejects(const std::string & description)
{
  try {
    auto manager = make_manager(description);
    const auto statuses = manager->get_components_status();
    const auto found = statuses.find(kHardwareName);
    return found == statuses.end() ||
           found->second.state.id() !=
           lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED;
  } catch (const std::exception &) {
    return true;
  }
}

rclcpp_lifecycle::State inactive_state()
{
  return {lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE, "inactive"};
}

rclcpp_lifecycle::State active_state()
{
  return {lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE, "active"};
}

rclcpp_lifecycle::State unconfigured_state()
{
  return {lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED, "unconfigured"};
}

bool read_ok(hardware_interface::ResourceManager & manager)
{
  const auto now = rclcpp::Time{0, 0, RCL_STEADY_TIME};
  const auto period = rclcpp::Duration::from_seconds(0.001);
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  return manager.read(now, period).ok;
#else
  return manager.read(now, period).result == hardware_interface::return_type::OK;
#endif
}

bool status_ok(const hardware_interface::HardwareReadWriteStatus & status)
{
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  return status.ok;
#else
  return status.result == hardware_interface::return_type::OK;
#endif
}

double axis_value(const hardware_interface::LoanedStateInterface & axis)
{
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  return axis.get_value();
#else
  return axis.get_optional<double>(100).value_or(std::numeric_limits<double>::quiet_NaN());
#endif
}

bool eventually(const std::function<bool()> & predicate, std::chrono::milliseconds timeout = 1s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

void configure_and_activate(hardware_interface::ResourceManager & manager)
{
  auto inactive = inactive_state();
  ASSERT_EQ(
    manager.set_component_state(kHardwareName, inactive),
    hardware_interface::return_type::OK);
  auto active = active_state();
  ASSERT_EQ(
    manager.set_component_state(kHardwareName, active),
    hardware_interface::return_type::OK);
}

std::vector<hardware_interface::LoanedStateInterface> claim_axes(
  hardware_interface::ResourceManager & manager)
{
  std::vector<hardware_interface::LoanedStateInterface> axes;
  for (const auto * suffix :
       {"force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z"}) {
    axes.emplace_back(manager.claim_state_interface(std::string{kSensorName} + "/" + suffix));
  }
  return axes;
}

TEST(NetFTHardwareInterface, LoadsThroughResourceManagerWithExactlySixInterfaces)
{
  test::FakeSensor sensor{200};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  EXPECT_EQ(manager->sensor_components_size(), 1U);
  const auto keys = manager->state_interface_keys();
  EXPECT_EQ(keys, (std::vector<std::string>{
    "netft_sensor/force.x", "netft_sensor/force.y", "netft_sensor/force.z",
    "netft_sensor/torque.x", "netft_sensor/torque.y", "netft_sensor/torque.z"}));
}

TEST(NetFTHardwareInterface, RejectsInvalidSensorAndInterfaceContracts)
{
  test::FakeSensor sensor;
  const auto load_fails = [&](const std::vector<std::string> & names, const std::string & extra = {}) {
    EXPECT_TRUE(manager_rejects(urdf(sensor.host(), sensor.port(), {}, names, extra)));
  };
  load_fails({"force.x", "force.y", "force.z", "torque.x", "torque.y"});
  load_fails({"force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.y"});
  load_fails({"force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z", "temperature"});
  load_fails(
    {"force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z"},
    "<sensor name=\"second\"><state_interface name=\"force.x\"/></sensor>");
}

TEST(NetFTHardwareInterface, RejectsJointGpioAndNonDoubleStateInterfaces)
{
  test::FakeSensor sensor;
  const auto exact_interfaces = std::vector<std::string>{
    "force.x", "force.y", "force.z", "torque.x", "torque.y", "torque.z"};

  EXPECT_TRUE(manager_rejects(urdf(
    sensor.host(), sensor.port(), {}, exact_interfaces,
    "<joint name=\"extra_joint\"><state_interface name=\"position\"/></joint>")));
  EXPECT_TRUE(manager_rejects(urdf(
    sensor.host(), sensor.port(), {}, exact_interfaces,
    "<gpio name=\"extra_gpio\"><state_interface name=\"voltage\"/></gpio>")));
  const auto int32_description = urdf(
    sensor.host(), sensor.port(), {}, exact_interfaces, {}, "int32");
  const auto float_description = urdf(
    sensor.host(), sensor.port(), {}, exact_interfaces, {}, "float");
#ifdef NETFT_ROS2_CONTROL_LEGACY_API
  // Humble's parser does not retain the data_type attribute for sensor state interfaces.
  EXPECT_FALSE(manager_rejects(int32_description));
  EXPECT_FALSE(manager_rejects(float_description));
#else
  EXPECT_TRUE(manager_rejects(int32_description));
  EXPECT_TRUE(manager_rejects(float_description));
#endif
}

TEST(NetFTHardwareInterface, RejectsEveryInvalidNumericAndEndpointParameter)
{
  test::FakeSensor sensor;
  for (const auto & parameter : std::vector<std::string>{
      "<param name=\"sensor_ip\"> </param>",
      "<param name=\"sensor_port\">0</param>",
      "<param name=\"sensor_port\">abc</param>",
      "<param name=\"counts_per_force\">0</param>",
      "<param name=\"counts_per_torque\">nan</param>",
      "<param name=\"receive_timeout\">-1</param>",
      "<param name=\"activation_timeout\">0</param>",
      "<param name=\"diagnostics_rate\">inf</param>",
      "<param name=\"expected_rdt_rate\">0</param>",
      "<param name=\"rate_tolerance\">-0.1</param>",
      "<param name=\"rate_tolerance\">1.1</param>"}) {
    EXPECT_TRUE(manager_rejects(urdf(sensor.host(), sensor.port(), parameter)))
      << parameter;
  }
}

TEST(NetFTHardwareInterface, ActivationIsBoundedWithoutASample)
{
  test::FakeSensor sensor;
  sensor.pause();
  auto manager = make_manager(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"activation_timeout\">0.05</param>"
    "<param name=\"receive_timeout\">0.5</param>"));
  auto inactive = inactive_state();
  ASSERT_EQ(manager->set_component_state(kHardwareName, inactive), hardware_interface::return_type::OK);
  const auto started = std::chrono::steady_clock::now();
  auto active = active_state();
  EXPECT_EQ(manager->set_component_state(kHardwareName, active), hardware_interface::return_type::ERROR);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
}

TEST(NetFTHardwareInterface, ReadsCurrentCompleteSampleAndMayReuseIt)
{
  test::FakeSensor sensor{200};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  configure_and_activate(*manager);
  auto axes = claim_axes(*manager);
  ASSERT_TRUE(read_ok(*manager));
  EXPECT_DOUBLE_EQ(axis_value(axes[0]), 1.0);
  EXPECT_DOUBLE_EQ(axis_value(axes[1]), -2.0);
  EXPECT_DOUBLE_EQ(axis_value(axes[2]), 3.0);
  EXPECT_DOUBLE_EQ(axis_value(axes[3]), 1.0);
  EXPECT_DOUBLE_EQ(axis_value(axes[4]), -2.0);
  EXPECT_DOUBLE_EQ(axis_value(axes[5]), 3.0);
  EXPECT_TRUE(read_ok(*manager));
}

enum class FaultInjection { Timeout, SeriousStatus, FtStall, FtBackward, MalformedStorm };

FaultCode expected_fault_code(const FaultInjection injection)
{
  switch (injection) {
    case FaultInjection::Timeout: return FaultCode::Timeout;
    case FaultInjection::SeriousStatus: return FaultCode::SeriousStatus;
    case FaultInjection::FtStall: return FaultCode::FtStall;
    case FaultInjection::FtBackward: return FaultCode::FtBackward;
    case FaultInjection::MalformedStorm: return FaultCode::MalformedStorm;
  }
  return FaultCode::None;
}

class FatalFaultTest : public testing::TestWithParam<FaultInjection> {};

TEST_P(FatalFaultTest, WritesAllNaNsAndStaysLatchedAfterValidTraffic)
{
  test::FakeSensor sensor{200};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  configure_and_activate(*manager);
  auto axes = claim_axes(*manager);
  ASSERT_TRUE(read_ok(*manager));

  switch (GetParam()) {
    case FaultInjection::Timeout:
      sensor.pause();
      break;
    case FaultInjection::SeriousStatus:
      sensor.queue_record(100, 0x00000001, 5000);
      break;
    case FaultInjection::FtStall:
      sensor.queue_record(100, 0, 5000);
      sensor.queue_record(101, 0, 5000);
      break;
    case FaultInjection::FtBackward:
      sensor.queue_record(100, 0, 5000);
      sensor.queue_record(101, 0, 4999);
      break;
    case FaultInjection::MalformedStorm:
      for (unsigned index = 0; index < 10; ++index) sensor.queue_payload({0x01, 0x02});
      break;
  }

  ASSERT_TRUE(eventually([&] { return !read_ok(*manager); }, 1500ms));
  EXPECT_EQ(ros2_control_compat::test_active_fault_code(), expected_fault_code(GetParam()));
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));
  sensor.resume();
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(ros2_control_compat::test_read_active_instance());
  EXPECT_EQ(ros2_control_compat::test_active_fault_code(), expected_fault_code(GetParam()));
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));
  EXPECT_NE(
    manager->get_components_status().at(kHardwareName).state.id(),
    lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE);
}

TEST(NetFTHardwareInterface, RejectsInitialSampleReturnedDuringBufferContention)
{
  test::FakeSensor sensor{1000};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  configure_and_activate(*manager);
  auto axes = claim_axes(*manager);

  ros2_control_compat::test_force_initial_sample_once();
  EXPECT_FALSE(ros2_control_compat::test_read_active_instance());
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));
}

#ifdef NETFT_ROS2_CONTROL_MODERN_API
TEST(NetFTHardwareInterface, StateWriteFailureIsPersistentAndInvalidatesEveryAxis)
{
  test::FakeSensor sensor{1000};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  configure_and_activate(*manager);
  auto axes = claim_axes(*manager);

  ros2_control_compat::test_fail_state_write_once_at(2);
  EXPECT_FALSE(ros2_control_compat::test_read_active_instance());
  EXPECT_TRUE(ros2_control_compat::test_interface_write_fault_latched());
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(ros2_control_compat::test_read_active_instance());
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));
}
#endif

TEST(NetFTHardwareInterface, DestroyingActiveResourceManagerStopsClient)
{
  test::FakeSensor sensor{1000};
  {
    auto manager = make_manager(urdf(sensor.host(), sensor.port()));
    configure_and_activate(*manager);
    ASSERT_TRUE(sensor.wait_for_command(Command::StartRealtime));
  }
  EXPECT_TRUE(sensor.wait_for_command(Command::StopStreaming));
}

INSTANTIATE_TEST_SUITE_P(
  EveryFatalClass, FatalFaultTest,
  testing::Values(
    FaultInjection::Timeout, FaultInjection::SeriousStatus, FaultInjection::FtStall,
    FaultInjection::FtBackward, FaultInjection::MalformedStorm));

TEST(NetFTHardwareInterface, SocketFailurePreventsActivationAndLeavesNaNs)
{
  int closed_port = 0;
  {
    test::FakeSensor sensor;
    closed_port = sensor.port();
  }
  auto manager = make_manager(urdf(
    "127.0.0.1", closed_port,
    "<param name=\"activation_timeout\">0.3</param>"));
  auto inactive = inactive_state();
  ASSERT_EQ(
    manager->set_component_state(kHardwareName, inactive),
    hardware_interface::return_type::OK);
  auto axes = claim_axes(*manager);
  auto active = active_state();
  EXPECT_EQ(
    manager->set_component_state(kHardwareName, active),
    hardware_interface::return_type::ERROR);
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));
}

TEST(NetFTHardwareInterface, ActiveSocketFailureLatchesAndKeepsEveryAxisInvalid)
{
  test::FakeSensor sensor{1000};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  configure_and_activate(*manager);
  auto axes = claim_axes(*manager);

  ros2_control_compat::test_break_active_socket();
  ASSERT_TRUE(eventually([&] {
    return ros2_control_compat::test_active_fault_code() == FaultCode::Socket;
  }));
  EXPECT_FALSE(read_ok(*manager));
  EXPECT_EQ(ros2_control_compat::test_active_fault_code(), FaultCode::Socket);
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));

  sensor.send_payload_now(std::vector<std::uint8_t>(36));
  EXPECT_FALSE(ros2_control_compat::test_read_active_instance());
  EXPECT_EQ(ros2_control_compat::test_active_fault_code(), FaultCode::Socket);
  for (const auto & axis : axes) EXPECT_TRUE(std::isnan(axis_value(axis)));
}

TEST(NetFTHardwareInterface, CleanupConfigureActivateClearsFaultAndSequenceBaseline)
{
  test::FakeSensor sensor{200};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  configure_and_activate(*manager);
  auto axes = claim_axes(*manager);
  sensor.pause();
  ASSERT_TRUE(eventually([&] { return !read_ok(*manager); }, 1s));
  axes.clear();

  auto unconfigured = unconfigured_state();
  ASSERT_EQ(
    manager->set_component_state(kHardwareName, unconfigured),
    hardware_interface::return_type::OK);
  sensor.resume();
  configure_and_activate(*manager);
  auto recovered_axes = claim_axes(*manager);
  ASSERT_TRUE(read_ok(*manager));
  for (const auto & axis : recovered_axes) EXPECT_TRUE(std::isfinite(axis_value(axis)));
}

TEST(NetFTHardwareInterface, BiasServiceIsInstanceLocalAndRequiresActiveStreaming)
{
  test::FakeSensor sensor{500};
  auto manager = make_manager(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"bias_service\">/tool_ft/zero</param>"));
  auto inactive = inactive_state();
  ASSERT_EQ(
    manager->set_component_state(kHardwareName, inactive),
    hardware_interface::return_type::OK);

  const auto inactive_response = call_bias("/tool_ft/zero");
  ASSERT_NE(inactive_response, nullptr);
  EXPECT_FALSE(inactive_response->success);
  const auto inactive_commands = sensor.commands();
  EXPECT_EQ(std::count(
    inactive_commands.begin(), inactive_commands.end(), Command::SetSoftwareBias), 0);

  auto active = active_state();
  ASSERT_EQ(
    manager->set_component_state(kHardwareName, active),
    hardware_interface::return_type::OK);
  const auto commands_before = sensor.commands();
  const auto bias_count_before = std::count(
    commands_before.begin(), commands_before.end(), Command::SetSoftwareBias);
  const auto start_count_before = std::count(
    commands_before.begin(), commands_before.end(), Command::StartRealtime);
  const auto active_response = call_bias("/tool_ft/zero");
  ASSERT_NE(active_response, nullptr);
  EXPECT_TRUE(active_response->success) << active_response->message;
  ASSERT_TRUE(sensor.wait_for_command(
    Command::SetSoftwareBias, static_cast<unsigned>(bias_count_before + 1)));
  ASSERT_TRUE(sensor.wait_for_command(
    Command::StartRealtime, static_cast<unsigned>(start_count_before + 1)));
  const auto commands_after = sensor.commands();
  ASSERT_EQ(commands_after.size(), commands_before.size() + 2);
  EXPECT_EQ(commands_after[commands_before.size()], Command::SetSoftwareBias);
  EXPECT_EQ(commands_after[commands_before.size() + 1], Command::StartRealtime);
}

TEST(NetFTHardwareInterface, SanitizesDefaultBiasServiceAndValidatesOverrideDuringInit)
{
  test::FakeSensor sensor{500};
  auto description = urdf_with_sensor_name(sensor.host(), sensor.port(), "tool-ft");
  ASSERT_FALSE(description.empty());
  auto manager = make_manager(description);
  auto inactive = inactive_state();
  ASSERT_EQ(
    manager->set_component_state(kHardwareName, inactive),
    hardware_interface::return_type::OK);
  ASSERT_NE(call_bias("/netft_encoded_746f6f6c2d6674/bias"), nullptr);

  for (const auto & invalid : {
      "<param name=\"bias_service\"></param>",
      "<param name=\"bias_service\">   </param>",
      "<param name=\"bias_service\">/bad//service</param>",
      "<param name=\"bias_service\">/9invalid</param>"}) {
    EXPECT_TRUE(manager_rejects(urdf(sensor.host(), sensor.port(), invalid))) << invalid;
  }
  EXPECT_FALSE(manager_rejects(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"bias_service\">/custom/zero</param>")));
}

TEST(NetFTHardwareInterface, DefaultNamesUseAnInjectiveRosTokenEncoding)
{
  struct Case {
    std::string sensor_name;
    std::string token;
  };
  const std::vector<Case> cases{
    {"tool_ft", "tool_ft"},
    {"tool-ft", "netft_encoded_746f6f6c2d6674"},
    {"netft_encoded_", "netft_encoded_6e657466745f656e636f6465645f"},
    {"9tool", "netft_encoded_39746f6f6c"},
    {std::string{"\xE5\x8A\x9B"}, "netft_encoded_e58a9b"},
  };

  for (const auto & item : cases) {
    test::FakeSensor sensor{500};
    auto manager = make_manager(
      urdf_with_sensor_name(sensor.host(), sensor.port(), item.sensor_name));
    auto inactive = inactive_state();
    ASSERT_EQ(
      manager->set_component_state(kHardwareName, inactive),
      hardware_interface::return_type::OK) << item.sensor_name;
    ASSERT_NE(call_bias("/" + item.token + "/bias"), nullptr) << item.sensor_name;
  }
}

TEST(NetFTHardwareInterface, CollidingLegacyNamesKeepDefaultBiasServicesIsolated)
{
  test::FakeSensor encoded_sensor{500};
  test::FakeSensor ordinary_sensor{500};
  auto manager = make_manager(two_sensor_urdf(
    encoded_sensor.port(), ordinary_sensor.port(), "tool-ft", "tool_ft"));

  for (const auto & component : {"left_hardware", "right_hardware"}) {
    auto inactive = inactive_state();
    ASSERT_EQ(
      manager->set_component_state(component, inactive),
      hardware_interface::return_type::OK);
    auto active = active_state();
    ASSERT_EQ(
      manager->set_component_state(component, active),
      hardware_interface::return_type::OK);
  }

  const auto encoded_response = call_bias(
    "/netft_encoded_746f6f6c2d6674/bias");
  ASSERT_NE(encoded_response, nullptr);
  ASSERT_TRUE(encoded_response->success) << encoded_response->message;
  ASSERT_TRUE(encoded_sensor.wait_for_command(Command::SetSoftwareBias));
  const auto ordinary_commands_before = ordinary_sensor.commands();
  EXPECT_EQ(std::count(
    ordinary_commands_before.begin(), ordinary_commands_before.end(),
    Command::SetSoftwareBias), 0);

  const auto ordinary_response = call_bias("/tool_ft/bias");
  ASSERT_NE(ordinary_response, nullptr);
  ASSERT_TRUE(ordinary_response->success) << ordinary_response->message;
  ASSERT_TRUE(ordinary_sensor.wait_for_command(Command::SetSoftwareBias));
  const auto encoded_commands_after = encoded_sensor.commands();
  const auto ordinary_commands_after = ordinary_sensor.commands();
  EXPECT_EQ(std::count(
    encoded_commands_after.begin(), encoded_commands_after.end(),
    Command::SetSoftwareBias), 1);
  EXPECT_EQ(std::count(
    ordinary_commands_after.begin(), ordinary_commands_after.end(),
    Command::SetSoftwareBias), 1);
}

TEST(NetFTHardwareInterface, BiasWithoutResumedDataFailStopsTheInstance)
{
  test::FakeSensor sensor{500};
  auto manager = make_manager(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"receive_timeout\">0.05</param>"));
  configure_and_activate(*manager);
  sensor.pause();

  const auto response = call_bias("/netft_sensor/bias");
  ASSERT_NE(response, nullptr);
  ASSERT_TRUE(response->success) << response->message;
  EXPECT_TRUE(eventually([&] { return !read_ok(*manager); }, 500ms));
  EXPECT_EQ(ros2_control_compat::test_active_fault_code(), FaultCode::Timeout);
}

TEST(NetFTHardwareInterface, PublishesInstanceNamedDiagnosticsWithEndpointHardwareId)
{
  test::FakeSensor sensor{200};
  auto manager = make_manager(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"diagnostics_rate\">20</param>"));
  configure_and_activate(*manager);

  auto node = std::make_shared<rclcpp::Node>("netft_diagnostics_test_client");
  diagnostic_msgs::msg::DiagnosticStatus received;
  std::atomic<bool> seen{false};
  auto subscription = node->create_subscription<diagnostic_msgs::msg::DiagnosticArray>(
    "/diagnostics", rclcpp::QoS{10}.reliable(),
    [&](const diagnostic_msgs::msg::DiagnosticArray & message) {
      for (const auto & status : message.status) {
        if (status.name == "netft_driver: netft_sensor") {
          received = status;
          seen.store(true, std::memory_order_release);
        }
      }
    });
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!seen.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    executor.spin_some();
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(seen.load(std::memory_order_acquire));
  EXPECT_EQ(received.hardware_id, sensor.host() + ":" + std::to_string(sensor.port()));
}

TEST(NetFTHardwareInterface, PluginLatchedTimeoutRemainsVisibleInDiagnostics)
{
  test::FakeSensor sensor{500};
  auto manager = make_manager(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"diagnostics_rate\">50</param>"));
  configure_and_activate(*manager);

  auto node = std::make_shared<rclcpp::Node>("netft_plugin_fault_diagnostics_client");
  std::mutex mutex;
  diagnostic_msgs::msg::DiagnosticStatus received;
  bool seen = false;
  auto subscription = node->create_subscription<diagnostic_msgs::msg::DiagnosticArray>(
    "/diagnostics", rclcpp::QoS{10}.reliable(),
    [&](const diagnostic_msgs::msg::DiagnosticArray & message) {
      std::lock_guard<std::mutex> lock{mutex};
      for (const auto & status : message.status) {
        if (status.name == "netft_driver: netft_sensor" && status.level == 2) {
          received = status;
          seen = true;
        }
      }
    });
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  ros2_control_compat::test_force_initial_sample_once();
  ASSERT_FALSE(ros2_control_compat::test_read_active_instance());
  ASSERT_EQ(ros2_control_compat::test_active_fault_code(), FaultCode::Timeout);
  ASSERT_TRUE(eventually([&] {
    executor.spin_some();
    std::lock_guard<std::mutex> lock{mutex};
    return seen;
  }, 1s));
  std::lock_guard<std::mutex> lock{mutex};
  EXPECT_FALSE(received.message.empty());
}

class PersistentFaultDiagnosticsTest :
  public testing::TestWithParam<FaultCode> {};

TEST_P(PersistentFaultDiagnosticsTest, PublishesErrorForMultipleTimerCycles)
{
  test::FakeSensor sensor{500};
  auto manager = make_manager(urdf(
    sensor.host(), sensor.port(),
    "<param name=\"diagnostics_rate\">50</param>"
    "<param name=\"receive_timeout\">0.05</param>"));
  configure_and_activate(*manager);
  auto node = std::make_shared<rclcpp::Node>("netft_persistent_fault_diagnostics_client");
  std::mutex mutex;
  std::vector<diagnostic_msgs::msg::DiagnosticStatus> statuses;
  auto subscription = node->create_subscription<diagnostic_msgs::msg::DiagnosticArray>(
    "/diagnostics", rclcpp::QoS{10}.reliable(),
    [&](const diagnostic_msgs::msg::DiagnosticArray & message) {
      std::lock_guard<std::mutex> lock{mutex};
      for (const auto & status : message.status) {
        if (status.name == "netft_driver: netft_sensor" && status.level == 2) {
          statuses.push_back(status);
        }
      }
    });
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  if (GetParam() == FaultCode::Socket) {
    ros2_control_compat::test_break_active_socket();
  } else {
    sensor.pause();
  }
  ASSERT_TRUE(eventually([&] {
    executor.spin_some();
    return ros2_control_compat::test_active_fault_code() == GetParam();
  }, 1s));
  ASSERT_TRUE(eventually([&] {
    executor.spin_some();
    std::lock_guard<std::mutex> lock{mutex};
    return statuses.size() >= 3;
  }, 1s));
  std::lock_guard<std::mutex> lock{mutex};
  for (const auto & status : statuses) {
    EXPECT_EQ(status.level, 2);
    EXPECT_FALSE(status.message.empty());
  }
}

INSTANTIATE_TEST_SUITE_P(
  SocketAndTimeout, PersistentFaultDiagnosticsTest,
  testing::Values(FaultCode::Socket, FaultCode::Timeout));

TEST(NetFTHardwareInterface, ExecutorCancelExceptionStillJoinsAuxiliaryThread)
{
  test::FakeSensor sensor{500};
  auto manager = make_manager(urdf(sensor.host(), sensor.port()));
  auto inactive = inactive_state();
  ASSERT_EQ(
    manager->set_component_state(kHardwareName, inactive),
    hardware_interface::return_type::OK);
  ASSERT_TRUE(eventually([] {
    return ros2_control_compat::test_auxiliary_thread_count() == 1;
  }));
  ros2_control_compat::test_throw_executor_cancel_once();
  manager.reset();
  EXPECT_EQ(ros2_control_compat::test_auxiliary_thread_count(), 0);
}

TEST(NetFTHardwareInterface, TwoInstancesHaveIsolatedServicesDiagnosticsAndFaults)
{
  test::FakeSensor left{200};
  test::FakeSensor right{200};
  auto manager = make_manager(two_sensor_urdf(left.port(), right.port()));
  for (const auto * component : {"left_hardware", "right_hardware"}) {
    auto inactive = inactive_state();
    ASSERT_EQ(manager->set_component_state(component, inactive), hardware_interface::return_type::OK);
    auto active = active_state();
    ASSERT_EQ(manager->set_component_state(component, active), hardware_interface::return_type::OK);
  }

  const auto left_bias = call_bias("/left_ft/bias");
  ASSERT_NE(left_bias, nullptr);
  ASSERT_TRUE(left_bias->success);
  EXPECT_TRUE(left.wait_for_command(Command::SetSoftwareBias));
  EXPECT_FALSE(right.wait_for_command(Command::SetSoftwareBias, 1, 50ms));
  const auto right_bias = call_bias("/right_ft/bias");
  ASSERT_NE(right_bias, nullptr);
  ASSERT_TRUE(right_bias->success);
  EXPECT_TRUE(right.wait_for_command(Command::SetSoftwareBias));

  auto node = std::make_shared<rclcpp::Node>("netft_multi_diagnostics_test_client");
  std::mutex diagnostics_mutex;
  std::unordered_map<std::string, diagnostic_msgs::msg::DiagnosticStatus> diagnostics;
  auto subscription = node->create_subscription<diagnostic_msgs::msg::DiagnosticArray>(
    "/diagnostics", rclcpp::QoS{10}.reliable(),
    [&](const diagnostic_msgs::msg::DiagnosticArray & message) {
      std::lock_guard<std::mutex> lock{diagnostics_mutex};
      for (const auto & status : message.status) diagnostics[status.name] = status;
    });
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  ASSERT_TRUE(eventually([&] {
    executor.spin_some();
    std::lock_guard<std::mutex> lock{diagnostics_mutex};
    return diagnostics.count("netft_driver: left_ft") != 0 &&
           diagnostics.count("netft_driver: right_ft") != 0;
  }));
  {
    std::lock_guard<std::mutex> lock{diagnostics_mutex};
    EXPECT_EQ(
      diagnostics["netft_driver: left_ft"].hardware_id,
      left.host() + ":" + std::to_string(left.port()));
    EXPECT_EQ(
      diagnostics["netft_driver: right_ft"].hardware_id,
      right.host() + ":" + std::to_string(right.port()));
  }

  auto left_axis = manager->claim_state_interface("left_ft/force.x");
  auto right_axis = manager->claim_state_interface("right_ft/force.x");
  ASSERT_TRUE(read_ok(*manager));
  ASSERT_DOUBLE_EQ(axis_value(right_axis), 1.0);
  const auto right_commands_before_fault = right.commands();
  left.pause();
  right.queue_record(900, 0, 9000, {900, -800, 700, 60, -50, 40});
  hardware_interface::HardwareReadWriteStatus read_status;
  ASSERT_TRUE(eventually([&] {
    read_status = manager->read(
      rclcpp::Time{0, 0, RCL_STEADY_TIME},
      rclcpp::Duration::from_seconds(0.001));
    return !status_ok(read_status) && std::isnan(axis_value(left_axis)) &&
           axis_value(right_axis) == 9.0;
  }, 1s));
  EXPECT_NE(std::find(
    read_status.failed_hardware_names.begin(), read_status.failed_hardware_names.end(),
    "left_hardware"), read_status.failed_hardware_names.end());
  EXPECT_EQ(std::find(
    read_status.failed_hardware_names.begin(), read_status.failed_hardware_names.end(),
    "right_hardware"), read_status.failed_hardware_names.end());
  EXPECT_EQ(axis_value(right_axis), 9.0);
  EXPECT_TRUE(std::isnan(axis_value(left_axis)));
  ASSERT_TRUE(eventually([&] {
    executor.spin_some();
    std::lock_guard<std::mutex> lock{diagnostics_mutex};
    const auto left_status = diagnostics.find("netft_driver: left_ft");
    const auto right_status = diagnostics.find("netft_driver: right_ft");
    return left_status != diagnostics.end() && left_status->second.level == 2 &&
           right_status != diagnostics.end() && right_status->second.level != 2;
  }, 1s));
  EXPECT_EQ(right.commands(), right_commands_before_fault);
  EXPECT_TRUE(right.wait_for_command(Command::StartRealtime));
}

}  // namespace
}  // namespace netft_driver
