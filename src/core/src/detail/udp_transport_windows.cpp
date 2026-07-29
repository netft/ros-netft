#include "detail/udp_transport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace netft::detail {
namespace {

constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0};

std::runtime_error winsock_error(const char *operation, const int error) {
  return std::runtime_error(std::string{operation} + " (WinSock error " + std::to_string(error) +
                            ")");
}

std::runtime_error last_winsock_error(const char *operation) {
  return winsock_error(operation, ::WSAGetLastError());
}

int timeout_milliseconds(const std::chrono::duration<double> timeout) {
  const auto milliseconds = std::ceil(timeout.count() * 1000.0);
  if (milliseconds >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(milliseconds);
}

SOCKET native_socket(const std::uintptr_t socket) { return static_cast<SOCKET>(socket); }

int socket_wait_error(const SOCKET socket, const short events) {
  if ((events & POLLNVAL) != 0) {
    return WSAENOTSOCK;
  }

  int error = 0;
  int error_size = sizeof(error);
  if ((events & POLLERR) != 0 &&
      ::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &error_size) ==
          0 &&
      error != 0) {
    return error;
  }
  return WSAECONNRESET;
}

} // namespace

WinSockRuntime::WinSockRuntime() {
  WSADATA data{};
  const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    throw winsock_error("failed to initialize WinSock", result);
  }
}

WinSockRuntime::~WinSockRuntime() { ::WSACleanup(); }

UdpTransport::~UdpTransport() { close(); }

void UdpTransport::connect(const std::string &host, const int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *addresses = nullptr;
  const auto service = std::to_string(port);
  const int result = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (result != 0) {
    throw winsock_error("failed to resolve sensor", result);
  }

  SOCKET connected_socket = INVALID_SOCKET;
  int last_error = WSAEHOSTUNREACH;
  for (auto *address = addresses; address != nullptr; address = address->ai_next) {
    connected_socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (connected_socket == INVALID_SOCKET) {
      last_error = ::WSAGetLastError();
      continue;
    }
    if (::connect(connected_socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
      break;
    }
    last_error = ::WSAGetLastError();
    ::closesocket(connected_socket);
    connected_socket = INVALID_SOCKET;
  }
  ::freeaddrinfo(addresses);

  if (connected_socket == INVALID_SOCKET) {
    throw winsock_error("failed to connect UDP socket", last_error);
  }

  std::scoped_lock lock(mutex_);
  if (socket_ != kInvalidSocket) {
    ::closesocket(native_socket(socket_));
  }
  socket_ = static_cast<std::uintptr_t>(connected_socket);
  shutdown_requested_ = false;
}

void UdpTransport::send(const std::array<std::uint8_t, 8> request) {
  std::scoped_lock lock(mutex_);
  if (socket_ == kInvalidSocket) {
    throw std::runtime_error("UDP socket is not connected");
  }
  const auto sent = ::send(native_socket(socket_), reinterpret_cast<const char *>(request.data()),
                           static_cast<int>(request.size()), 0);
  if (sent == SOCKET_ERROR) {
    throw last_winsock_error("failed to send UDP request");
  }
  if (static_cast<std::size_t>(sent) != request.size()) {
    throw std::runtime_error("short UDP request write");
  }
}

std::size_t UdpTransport::receive(std::uint8_t *data, const std::size_t capacity,
                                  const std::chrono::duration<double> timeout) {
  SOCKET socket = INVALID_SOCKET;
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
    socket = native_socket(socket_);
    wait_started_hook = wait_started_test_hook_;
    wait_started_context = wait_started_test_context_;
  }

  WSAPOLLFD descriptor{socket, POLLRDNORM, 0};
  if (wait_started_hook != nullptr) {
    wait_started_hook(wait_started_context);
  }
  const auto bounded_timeout = std::max(timeout, std::chrono::duration<double>::zero());
  const auto deadline = std::chrono::steady_clock::now() + bounded_timeout;
  constexpr int kShutdownCheckIntervalMilliseconds = 50;
  constexpr auto kShutdownCheckInterval =
      std::chrono::milliseconds{kShutdownCheckIntervalMilliseconds};
  for (;;) {
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_requested_) {
        return 0;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const auto remaining = deadline - now;
    const auto poll_timeout = remaining > std::chrono::steady_clock::duration::zero()
                                  ? std::min(std::chrono::duration<double>{remaining},
                                             std::chrono::duration<double>{kShutdownCheckInterval})
                                  : std::chrono::duration<double>::zero();
    descriptor.revents = 0;
    const int poll_result =
        ::WSAPoll(&descriptor, 1,
                  std::min(kShutdownCheckIntervalMilliseconds, timeout_milliseconds(poll_timeout)));
    if (poll_result == SOCKET_ERROR) {
      const int error = ::WSAGetLastError();
      if (error == WSAEINTR) {
        return 0;
      }
      throw winsock_error("failed to wait for UDP record", error);
    }
    if (poll_result > 0) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return 0;
    }
  }
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_) {
      return 0;
    }
  }
  if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    throw winsock_error("failed to wait for UDP record",
                        socket_wait_error(socket, descriptor.revents));
  }

  const auto bounded_capacity =
      std::min(capacity, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  const auto received =
      ::recv(socket, reinterpret_cast<char *>(data), static_cast<int>(bounded_capacity), 0);
  if (received == SOCKET_ERROR) {
    const int error = ::WSAGetLastError();
    {
      std::scoped_lock lock(mutex_);
      if (shutdown_requested_) {
        return 0;
      }
    }
    if (error == WSAEINTR || error == WSAESHUTDOWN) {
      return 0;
    }
    throw winsock_error("failed to receive UDP record", error);
  }
  return static_cast<std::size_t>(received);
}

void UdpTransport::shutdown() noexcept {
  std::scoped_lock lock(mutex_);
  shutdown_requested_ = true;
  if (socket_ != kInvalidSocket) {
    static_cast<void>(::shutdown(native_socket(socket_), SD_BOTH));
  }
}

void UdpTransport::close() noexcept {
  std::scoped_lock lock(mutex_);
  shutdown_requested_ = true;
  if (socket_ != kInvalidSocket) {
    static_cast<void>(::closesocket(native_socket(socket_)));
    socket_ = kInvalidSocket;
  }
}

} // namespace netft::detail
