#pragma once

#include "netft/types.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace netft_driver {

inline constexpr std::uint64_t kMalformedStormThreshold = 10;

struct DiagnosticReport {
  int level{};
  std::string message;
  std::vector<std::pair<std::string, std::string>> values;
  std::string log_key;
};

class DiagnosticEvaluator {
public:
  DiagnosticEvaluator(double expected_rdt_rate, double rate_tolerance);
  DiagnosticEvaluator(double expected_rdt_rate, bool rate_tolerance) = delete;

  DiagnosticReport evaluate(const netft::HealthSnapshot & snapshot);

private:
  double expected_rdt_rate_;
  double rate_tolerance_;
  std::uint64_t lost_{};
  std::uint64_t device_error_{};
  std::uint64_t timeout_{};
  std::uint64_t malformed_{};
  std::uint64_t ft_stall_{};
  std::uint64_t ft_backward_{};
  std::uint64_t ft_restart_{};
  std::uint64_t callback_error_{};
};

class FaultLogThrottle {
public:
  explicit FaultLogThrottle(double repeat_interval = 10.0);

  bool should_log(const DiagnosticReport & report, double now);

private:
  double repeat_interval_;
  bool active_{false};
  int level_{};
  std::string key_;
  double last_log_{};
};

}  // namespace netft_driver
