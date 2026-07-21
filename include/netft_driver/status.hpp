#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "netft_driver/types.hpp"

namespace netft_driver {

enum class DiagnosticSeverity : int { Ok = 0, Warn = 1, Error = 2 };
DiagnosticSeverity classify_status(std::uint32_t status);
std::string decode_status(std::uint32_t status);

enum class SequenceKind { First, Contiguous, Gap, Duplicate, OutOfOrder };
struct SequenceObservation { SequenceKind kind; std::uint32_t gap{0}; };
class RdtSequenceTracker {
public:
  SequenceObservation observe(std::uint32_t current);
  void reset();
  bool has_last() const { return has_last_; }
  std::uint32_t last() const { return previous_; }
private:
  bool has_last_{false}; std::uint32_t previous_{};
};

enum class FtSequenceKind { First, Forward, Stall, Backward, Restart };
struct FtSequenceObservation { FtSequenceKind kind; };
class FtSequenceTracker {
public:
  FtSequenceObservation observe(std::uint32_t current);
  void begin_session();
  bool has_last() const { return has_last_; }
  std::uint32_t last() const { return previous_; }
private:
  bool has_last_{false}; std::uint32_t previous_{};
  bool has_restart_candidate_{false}; std::uint32_t restart_candidate_{};
};

inline constexpr std::uint64_t kMalformedStormThreshold = 10;
inline const std::vector<std::string> kDiagnosticValueKeys{
  "state", "sensor", "last_rdt_sequence", "last_ft_sequence", "last_ft_progress", "device_status", "active_status", "receive_rate_hz", "expected_receive_rate_hz", "rate_tolerance", "publish_rate_hz", "received_count", "published_count", "rate_dropped_count", "device_error_count", "lost_count", "duplicate_count", "out_of_order_count", "ft_stall_count", "ft_backward_count", "ft_restart_count", "malformed_count", "malformed_storm_threshold", "malformed_storm_window", "reconnect_count", "timeout_count", "callback_error_count", "last_record_age_s", "last_error"};

struct DiagnosticReport {
  int level{}; std::string message; std::vector<std::pair<std::string, std::string>> values; std::string log_key;
};
class DiagnosticEvaluator {
public:
  DiagnosticEvaluator(double expected_rdt_rate, double rate_tolerance);
  DiagnosticEvaluator(double expected_rdt_rate, bool rate_tolerance) = delete;
  DiagnosticReport evaluate(const HealthSnapshot & snapshot);
private:
  double expected_rdt_rate_; double rate_tolerance_;
  std::uint64_t lost_{}, device_error_{}, timeout_{}, malformed_{}, ft_stall_{}, ft_backward_{}, ft_restart_{};
};
class FaultLogThrottle {
public:
  explicit FaultLogThrottle(double repeat_interval = 10.0);
  bool should_log(const DiagnosticReport & report, double now);
private:
  double repeat_interval_; bool active_{false}; int level_{}; std::string key_; double last_log_{};
};

}  // namespace netft_driver
