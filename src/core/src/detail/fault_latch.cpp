#include "detail/fault_latch.hpp"

#include <utility>

namespace netft::detail {

bool FaultLatch::publish(const FaultCode code, std::string message, std::mutex &data_mutex,
                         HealthSnapshot &health) noexcept {
  try {
    std::scoped_lock data_lock(data_mutex);
    FaultCode expected = FaultCode::None;
    if (!code_.compare_exchange_strong(expected, code, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return false;
    }
    health.state = ClientState::Faulted;
    health.fault_code = code;
    health.last_error.swap(message);
    return true;
  } catch (...) {
    return false;
  }
}

void FaultLatch::reset() noexcept { code_.store(FaultCode::None, std::memory_order_release); }

bool FaultLatch::faulted() const noexcept {
  return code_.load(std::memory_order_acquire) != FaultCode::None;
}

FaultCode FaultLatch::code() const noexcept { return code_.load(std::memory_order_acquire); }

} // namespace netft::detail
