#include "detail/udp_transport.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace netft::detail {
namespace {

std::runtime_error socket_error(const char *operation) {
  return std::runtime_error(std::string{operation} + ": " + std::strerror(errno));
}

int timeout_milliseconds(const std::chrono::duration<double> timeout) {
  const auto milliseconds = std::ceil(timeout.count() * 1000.0);
  if (milliseconds >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(milliseconds);
}

constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0};

} // namespace

UdpTransport::~UdpTransport() { close(); }

void UdpTransport::connect(const std::string &host, const int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *addresses = nullptr;
  const auto service = std::to_string(port);
  const int result = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (result != 0) {
    throw std::runtime_error(std::string{"failed to resolve sensor: "} + ::gai_strerror(result));
  }

  int connected_socket = -1;
  int last_error = 0;
  for (auto *address = addresses; address != nullptr; address = address->ai_next) {
    connected_socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (connected_socket < 0) {
      last_error = errno;
      continue;
    }
    if (::connect(connected_socket, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    last_error = errno;
    ::close(connected_socket);
    connected_socket = -1;
  }
  ::freeaddrinfo(addresses);

  if (connected_socket < 0) {
    errno = last_error;
    throw socket_error("failed to connect UDP socket");
  }

  std::scoped_lock lock(mutex_);
  if (socket_ != kInvalidSocket) {
    ::close(static_cast<int>(socket_));
  }
  socket_ = static_cast<std::uintptr_t>(connected_socket);
  shutdown_requested_ = false;
}

void UdpTransport::send(const std::array<std::uint8_t, 8> request) {
  std::scoped_lock lock(mutex_);
  if (socket_ == kInvalidSocket) {
    throw std::runtime_error("UDP socket is not connected");
  }
  const auto sent = ::send(static_cast<int>(socket_), request.data(), request.size(), 0);
  if (sent < 0) {
    throw socket_error("failed to send UDP request");
  }
  if (static_cast<std::size_t>(sent) != request.size()) {
    throw std::runtime_error("short UDP request write");
  }
}

std::size_t UdpTransport::receive(std::uint8_t *data, const std::size_t capacity,
                                  const std::chrono::duration<double> timeout) {
  int socket = -1;
  WaitStartedTestHook wait_started_hook = nullptr;
  void *wait_started_context = nullptr;
  {
    std::scoped_lock lock(mutex_);
    if (socket_ == kInvalidSocket) {
      throw std::runtime_error("UDP socket is not connected");
    }
    if (shutdown_requested_) {
      return 0;
    }
    socket = static_cast<int>(socket_);
    wait_started_hook = wait_started_test_hook_;
    wait_started_context = wait_started_test_context_;
  }

  pollfd descriptor{socket, POLLIN, 0};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto remaining = timeout;
  int poll_result{};
  if (wait_started_hook != nullptr) {
    wait_started_hook(wait_started_context);
  }
  while (true) {
    poll_result = ::poll(&descriptor, 1, timeout_milliseconds(remaining));
    if (poll_result >= 0) {
      break;
    }
    if (errno != EINTR) {
      throw socket_error("failed to wait for UDP record");
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return 0;
    }
    remaining = deadline - now;
  }
  if (poll_result == 0) {
    return 0;
  }
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_) {
      return 0;
    }
  }
  if ((descriptor.revents & POLLNVAL) != 0) {
    throw std::runtime_error("UDP socket became invalid");
  }

  const auto received = ::recv(socket, data, capacity, 0);
  if (received < 0) {
    if (errno == EINTR) {
      return 0;
    }
    throw socket_error("failed to receive UDP record");
  }
  return static_cast<std::size_t>(received);
}

void UdpTransport::shutdown() noexcept {
  std::scoped_lock lock(mutex_);
  shutdown_requested_ = true;
  if (socket_ != kInvalidSocket) {
    static_cast<void>(::shutdown(static_cast<int>(socket_), SHUT_RDWR));
  }
}

void UdpTransport::close() noexcept {
  std::scoped_lock lock(mutex_);
  shutdown_requested_ = true;
  if (socket_ != kInvalidSocket) {
    ::close(static_cast<int>(socket_));
    socket_ = kInvalidSocket;
  }
}

} // namespace netft::detail
