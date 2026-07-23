#pragma once

#include <string_view>

#include "netft/types.hpp"

namespace netft::detail {

SensorConfiguration parse_sensor_configuration(std::string_view xml);

} // namespace netft::detail
