#pragma once

#include <cstdint>

namespace netft::detail {

enum class SequenceKind { First, Contiguous, Gap, Duplicate, OutOfOrder };

struct SequenceObservation {
  SequenceKind kind;
  std::uint32_t gap{0};
};

class RdtSequenceTracker {
public:
  SequenceObservation observe(std::uint32_t current);
  void reset();
  [[nodiscard]] bool has_last() const { return has_last_; }
  [[nodiscard]] std::uint32_t last() const { return previous_; }

private:
  bool has_last_{false};
  std::uint32_t previous_{};
};

enum class FtSequenceKind { First, Forward, Stall, Backward, Restart };

struct FtSequenceObservation {
  FtSequenceKind kind;
};

class FtSequenceTracker {
public:
  FtSequenceObservation observe(std::uint32_t current);
  void begin_session();
  [[nodiscard]] bool has_last() const { return has_last_; }
  [[nodiscard]] std::uint32_t last() const { return previous_; }

private:
  bool has_last_{false};
  std::uint32_t previous_{};
  bool has_restart_candidate_{false};
  std::uint32_t restart_candidate_{};
};

} // namespace netft::detail
