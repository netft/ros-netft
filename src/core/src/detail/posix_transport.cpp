#include "detail/posix_transport.hpp"

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

} // namespace

PosixTransport::~PosixTransport() { close(); }

void PosixTransport::connect(const std::string &host, const int port) {
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
  if (socket_ >= 0) {
    ::close(socket_);
  }
  socket_ = connected_socket;
}

void PosixTransport::send(const std::array<std::uint8_t, 8> &request) {
  std::scoped_lock lock(mutex_);
  if (socket_ < 0) {
    throw std::runtime_error("UDP socket is not connected");
  }
  const auto sent = ::send(socket_, request.data(), request.size(), 0);
  if (sent < 0) {
    throw socket_error("failed to send UDP request");
  }
  if (static_cast<std::size_t>(sent) != request.size()) {
    throw std::runtime_error("short UDP request write");
  }
}

std::size_t PosixTransport::receive(std::uint8_t *data, const std::size_t capacity,
                                    const std::chrono::duration<double> timeout) {
  int socket = -1;
  {
    std::scoped_lock lock(mutex_);
    socket = socket_;
  }
  if (socket < 0) {
    throw std::runtime_error("UDP socket is not connected");
  }

  pollfd descriptor{socket, POLLIN, 0};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto remaining = timeout;
  int poll_result{};
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

void PosixTransport::shutdown() noexcept {
  std::scoped_lock lock(mutex_);
  if (socket_ >= 0) {
    static_cast<void>(::shutdown(socket_, SHUT_RDWR));
  }
}

void PosixTransport::close() noexcept {
  std::scoped_lock lock(mutex_);
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
}

} // namespace netft::detail
