#pragma once

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace netft::detail {
using SteadyClock = std::chrono::steady_clock;

inline SteadyClock::duration checked_duration(std::chrono::duration<double> duration) {
  const auto ticks = static_cast<long double>(duration.count()) * SteadyClock::period::den /
                     SteadyClock::period::num;
  if (!std::isfinite(ticks) || ticks < 0 ||
      ticks >= static_cast<long double>(SteadyClock::duration::max().count())) {
    throw std::invalid_argument("duration is outside the supported steady-clock range");
  }
  return SteadyClock::duration{static_cast<SteadyClock::rep>(ticks)};
}

inline SteadyClock::time_point checked_deadline(SteadyClock::time_point origin,
                                                std::chrono::duration<double> duration) {
  const auto ticks = checked_duration(duration);
  if (origin > SteadyClock::time_point::max() - ticks) {
    throw std::invalid_argument("deadline is outside the supported steady-clock range");
  }
  return origin + ticks;
}
} // namespace netft::detail
