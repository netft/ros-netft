#include "detail/sequence.hpp"

namespace netft::detail {
namespace {

constexpr std::uint32_t kRestartLowMax = 0x0000ffffU;
constexpr std::uint32_t kRestartMinBaseline = kRestartLowMax + 1U;

} // namespace

SequenceObservation RdtSequenceTracker::observe(const std::uint32_t current) {
  if (!has_last_) {
    has_last_ = true;
    previous_ = current;
    return {SequenceKind::First, 0};
  }

  const std::uint32_t delta = current - previous_;
  if (delta == 0) {
    return {SequenceKind::Duplicate, 0};
  }
  if (delta < 0x80000000U) {
    previous_ = current;
    return delta == 1 ? SequenceObservation{SequenceKind::Contiguous, 0}
                      : SequenceObservation{SequenceKind::Gap, delta - 1};
  }
  return {SequenceKind::OutOfOrder, 0};
}

void RdtSequenceTracker::reset() { has_last_ = false; }

void FtSequenceTracker::begin_session() { has_restart_candidate_ = false; }

FtSequenceObservation FtSequenceTracker::observe(const std::uint32_t current) {
  if (!has_last_) {
    has_last_ = true;
    previous_ = current;
    return {FtSequenceKind::First};
  }
  if (current == previous_) {
    has_restart_candidate_ = false;
    return {FtSequenceKind::Stall};
  }

  const std::uint32_t delta = current - previous_;
  if (delta < 0x80000000U) {
    previous_ = current;
    has_restart_candidate_ = false;
    return {FtSequenceKind::Forward};
  }

  if (previous_ >= kRestartMinBaseline && has_restart_candidate_ &&
      restart_candidate_ <= kRestartLowMax && current <= kRestartLowMax) {
    const std::uint32_t candidate_delta = current - restart_candidate_;
    if (candidate_delta > 0 && candidate_delta < 0x80000000U) {
      previous_ = current;
      has_restart_candidate_ = false;
      return {FtSequenceKind::Restart};
    }
  }

  restart_candidate_ = current;
  has_restart_candidate_ = true;
  return {FtSequenceKind::Backward};
}

} // namespace netft::detail
