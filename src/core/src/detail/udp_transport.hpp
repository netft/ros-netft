#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace netft::detail {

#ifdef _WIN32
class WinSockRuntime {
public:
  WinSockRuntime();
  ~WinSockRuntime();
  WinSockRuntime(const WinSockRuntime &) = delete;
  WinSockRuntime &operator=(const WinSockRuntime &) = delete;
};
#endif

class UdpTransport {
public:
  using WaitStartedTestHook = void (*)(void *) noexcept;

  UdpTransport() = default;
  ~UdpTransport();
  UdpTransport(const UdpTransport &) = delete;
  UdpTransport &operator=(const UdpTransport &) = delete;

  void connect(const std::string &host, int port);
  void send(std::array<std::uint8_t, 8> request);
  std::size_t receive(std::uint8_t *data, std::size_t capacity,
                      std::chrono::duration<double> timeout);
  void shutdown() noexcept;
  void close() noexcept;

  void set_wait_started_test_hook(WaitStartedTestHook hook, void *context) {
    std::scoped_lock lock(mutex_);
    wait_started_test_hook_ = hook;
    wait_started_test_context_ = context;
  }

private:
#ifdef _WIN32
  WinSockRuntime runtime_;
#endif
  mutable std::mutex mutex_;
  std::uintptr_t socket_{~std::uintptr_t{0}};
  bool shutdown_requested_{false};
  WaitStartedTestHook wait_started_test_hook_{nullptr};
  void *wait_started_test_context_{nullptr};
};

} // namespace netft::detail
