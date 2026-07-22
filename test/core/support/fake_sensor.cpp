#include "support/fake_sensor.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace netft::test {
namespace {

constexpr std::string_view kDefaultXml = R"xml(
<netft><prodname>Fake Net F/T</prodname><cfgcpf>1000000</cfgcpf>
<cfgcpt>1000000</cfgcpt><scfgfu>N</scfgfu><scfgtu>Nm</scfgtu></netft>)xml";

void put_u32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(value >> (24U - 8U * index));
  }
}

std::vector<std::uint8_t> make_record(const std::uint32_t rdt_sequence, const std::uint32_t status,
                                      const std::uint32_t ft_sequence,
                                      const std::array<std::int32_t, 6> &axes) {
  std::vector<std::uint8_t> data(36);
  put_u32(data, 0, rdt_sequence);
  put_u32(data, 4, ft_sequence);
  put_u32(data, 8, status);
  for (std::size_t index = 0; index < axes.size(); ++index) {
    put_u32(data, 12 + 4 * index, static_cast<std::uint32_t>(axes[index]));
  }
  return data;
}

} // namespace

FakeSensor::FakeSensor(const double rate_hz) : http_(std::string{kDefaultXml}), rate_hz_(rate_hz) {
  socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_ < 0) {
    throw std::runtime_error("fake sensor socket failed");
  }
  const int flags = ::fcntl(socket_, F_GETFL, 0);
  static_cast<void>(::fcntl(socket_, F_SETFL, flags | O_NONBLOCK));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    ::close(socket_);
    socket_ = -1;
    throw std::runtime_error("fake sensor bind failed");
  }
  socklen_t address_size = sizeof(address);
  if (::getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &address_size) != 0) {
    ::close(socket_);
    socket_ = -1;
    throw std::runtime_error("fake sensor getsockname failed");
  }
  rdt_port_ = ntohs(address.sin_port);
  worker_ = std::thread(&FakeSensor::run, this);
}

FakeSensor::~FakeSensor() {
  stopping_ = true;
  if (socket_ >= 0) {
    ::shutdown(socket_, SHUT_RDWR);
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (socket_ >= 0) {
    ::close(socket_);
  }
}

void FakeSensor::pause() noexcept { enabled_ = false; }

void FakeSensor::resume() noexcept { enabled_ = true; }

void FakeSensor::queue_payload(std::vector<std::uint8_t> payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  payloads_.push_back(std::move(payload));
}

void FakeSensor::send_payload_now(std::vector<std::uint8_t> payload) {
  ::sockaddr_in peer{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_client_) {
      throw std::runtime_error("fake sensor has no client");
    }
    peer = client_;
  }
  static_cast<void>(::sendto(socket_, payload.data(), payload.size(), 0,
                             reinterpret_cast<sockaddr *>(&peer), sizeof(peer)));
}

void FakeSensor::queue_record(const std::uint32_t rdt_sequence, const std::uint32_t status,
                              std::uint32_t ft_sequence, const std::array<std::int32_t, 6> axes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ft_sequence == 0) {
    ft_sequence = ft_;
  }
  ft_ = ft_sequence + 4;
  payloads_.push_back(make_record(rdt_sequence, status, ft_sequence, axes));
}

void FakeSensor::send_record_now(const std::uint32_t rdt_sequence, const std::uint32_t status,
                                 std::uint32_t ft_sequence,
                                 const std::array<std::int32_t, 6> axes) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ft_sequence == 0) {
      ft_sequence = ft_;
    }
    ft_ = ft_sequence + 4;
  }
  send_payload_now(make_record(rdt_sequence, status, ft_sequence, axes));
}

void FakeSensor::skip_rdt(const unsigned count) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  skip_ += count;
}

bool FakeSensor::wait_for_command(const detail::Command command, const unsigned count,
                                  const std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    const auto observed = commands();
    if (std::count(observed.begin(), observed.end(), command) >=
        static_cast<std::ptrdiff_t>(count)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

bool FakeSensor::wait_for_http_request(const unsigned count,
                                       const std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (http_request_count() >= count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

std::vector<detail::Command> FakeSensor::commands() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commands_;
}

std::vector<FakeSensor::CommandEvent> FakeSensor::command_events() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return command_events_;
}

void FakeSensor::set_xml_configuration(std::string xml, const int status) {
  http_.set_response(std::move(xml), status);
}

void FakeSensor::set_http_response_delay(const std::chrono::milliseconds delay) {
  http_.set_response_delay(delay);
}

void FakeSensor::record_command(const std::uint8_t *data, const std::size_t size,
                                const ::sockaddr_in &peer) {
  if (size != 8 || data[0] != 0x12 || data[1] != 0x34) {
    return;
  }
  const auto command =
      static_cast<detail::Command>((static_cast<std::uint16_t>(data[2]) << 8U) | data[3]);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(command);
    command_events_.push_back({command, std::chrono::steady_clock::now()});
    if (command == detail::Command::StartRealtime) {
      client_ = peer;
      has_client_ = true;
      rdt_ = 0;
    }
  }
  if (command == detail::Command::StartRealtime) {
    streaming_ = true;
  }
  if (command == detail::Command::StopStreaming || command == detail::Command::SetSoftwareBias) {
    streaming_ = false;
  }
}

void FakeSensor::send_next() {
  if (!streaming_ || !enabled_) {
    return;
  }
  std::vector<std::uint8_t> data;
  ::sockaddr_in peer{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_client_) {
      return;
    }
    peer = client_;
    if (!payloads_.empty()) {
      data = std::move(payloads_.front());
      payloads_.erase(payloads_.begin());
    }
    if (data.empty()) {
      data.resize(36);
      rdt_ += 1 + skip_;
      skip_ = 0;
      put_u32(data, 0, rdt_);
      put_u32(data, 4, ft_);
      ft_ += 4;
      put_u32(data, 12, 100);
      put_u32(data, 16, static_cast<std::uint32_t>(-200));
      put_u32(data, 20, 300);
      put_u32(data, 24, 10);
      put_u32(data, 28, static_cast<std::uint32_t>(-20));
      put_u32(data, 32, 30);
    }
  }
  static_cast<void>(::sendto(socket_, data.data(), data.size(), 0,
                             reinterpret_cast<sockaddr *>(&peer), sizeof(peer)));
}

void FakeSensor::run() {
  auto next = std::chrono::steady_clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>{1.0 / rate_hz_});
  while (!stopping_) {
    ::sockaddr_in peer{};
    socklen_t peer_size = sizeof(peer);
    std::array<std::uint8_t, 64> data{};
    const auto size = ::recvfrom(socket_, data.data(), data.size(), 0,
                                 reinterpret_cast<sockaddr *>(&peer), &peer_size);
    if (size > 0) {
      record_command(data.data(), static_cast<std::size_t>(size), peer);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
      send_next();
      next += interval;
      if (next < now) {
        next = now + interval;
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds{100});
  }
}

} // namespace netft::test
