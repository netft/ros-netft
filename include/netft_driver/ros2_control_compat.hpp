#pragma once

#include "netft_driver/types.hpp"

#include <hardware_interface/hardware_info.hpp>
#include <hardware_interface/sensor_interface.hpp>

#include <cstddef>

#ifdef NETFT_ROS2_CONTROL_MODERN_API
#include <hardware_interface/types/hardware_component_interface_params.hpp>
#endif

namespace netft_driver::ros2_control_compat {

#if defined(NETFT_ROS2_CONTROL_LEGACY_API)
using InitArgument = hardware_interface::HardwareInfo;

inline const hardware_interface::HardwareInfo & hardware_info(
  const InitArgument & argument) noexcept
{
  return argument;
}
#elif defined(NETFT_ROS2_CONTROL_MODERN_API)
using InitArgument = hardware_interface::HardwareComponentInterfaceParams;

inline const hardware_interface::HardwareInfo & hardware_info(
  const InitArgument & argument) noexcept
{
  return argument.hardware_info;
}
#else
#error "A ros2_control API family must be selected by CMake"
#endif

#ifdef NETFT_ROS2_CONTROL_TESTING
void test_force_initial_sample_once() noexcept;
void test_fail_state_write_once_at(std::size_t axis) noexcept;
void test_break_active_socket() noexcept;
bool test_read_active_instance() noexcept;
FaultCode test_active_fault_code() noexcept;
bool test_interface_write_fault_latched() noexcept;
void test_throw_executor_cancel_once() noexcept;
int test_auxiliary_thread_count() noexcept;
#endif

}  // namespace netft_driver::ros2_control_compat
