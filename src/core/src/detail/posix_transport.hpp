#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace netft::detail {

class PosixTransport {
public:
  PosixTransport() = default;
  ~PosixTransport();
  PosixTransport(const PosixTransport &) = delete;
  PosixTransport &operator=(const PosixTransport &) = delete;

  void connect(const std::string &host, int port);
  void send(const std::array<std::uint8_t, 8> &request);
  std::size_t receive(std::uint8_t *data, std::size_t capacity,
                      std::chrono::duration<double> timeout);
  void shutdown() noexcept;
  void close() noexcept;

private:
  mutable std::mutex mutex_;
  int socket_{-1};
};

} // namespace netft::detail
