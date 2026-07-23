#pragma once

#include "netft/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace netft_driver {

inline constexpr std::uint64_t kMalformedStormThreshold = 10;
inline constexpr std::array<const char *, 36> kDiagnosticValueKeys{
  "state", "sensor", "last_rdt_sequence", "last_ft_sequence", "last_ft_progress",
  "device_status", "active_status", "receive_rate_hz", "expected_receive_rate_hz",
  "rate_tolerance", "delivery_rate_hz", "received_count", "delivered_count",
  "rate_limited_count", "device_error_count", "lost_count", "duplicate_count",
  "out_of_order_count", "ft_stall_count", "ft_backward_count", "ft_restart_count",
  "malformed_count", "malformed_storm_threshold", "malformed_storm_window",
  "reconnect_count", "timeout_count", "callback_error_count", "last_record_age_s",
  "last_error", "configuration_source", "force_unit", "torque_unit",
  "configuration_revision", "calibration_change_count", "counts_per_force_unit",
  "counts_per_torque_unit"};

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
