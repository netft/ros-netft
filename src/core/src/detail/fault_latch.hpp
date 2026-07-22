#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "netft/types.hpp"

namespace netft::detail {

class FaultLatch {
public:
  FaultLatch() = default;
  FaultLatch(const FaultLatch &) = delete;
  FaultLatch &operator=(const FaultLatch &) = delete;

  bool publish(FaultCode code, std::string message, std::mutex &data_mutex,
               HealthSnapshot &health) noexcept;
  void reset() noexcept;
  [[nodiscard]] bool faulted() const noexcept;
  [[nodiscard]] FaultCode code() const noexcept;

private:
  std::atomic<FaultCode> code_{FaultCode::None};
};

} // namespace netft::detail
