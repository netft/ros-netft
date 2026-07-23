#pragma once

#include <hardware_interface/hardware_info.hpp>
#include <hardware_interface/sensor_interface.hpp>

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

}  // namespace netft_driver::ros2_control_compat
