#pragma once

#include "netft/types.hpp"

#include <array>
#include <chrono>
#include <cstdint>

namespace netft_driver {

struct SiSample {
  std::uint32_t rdt_sequence{};
  std::uint32_t ft_sequence{};
  std::uint32_t status{};
  std::array<double, 3> force{};
  std::array<double, 3> torque{};
  std::uint64_t configuration_revision{};
  std::chrono::steady_clock::time_point received_at{};
};

double force_scale_to_newtons(netft::ForceUnit unit);
double torque_scale_to_newton_metres(netft::TorqueUnit unit);
SiSample to_si_sample(const netft::Sample & sample);

}  // namespace netft_driver
