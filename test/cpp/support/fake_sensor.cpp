#include "fake_sensor.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace netft_driver::test {
namespace {
void put32(std::vector<std::uint8_t> & bytes, std::size_t offset, std::uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}
}

FakeSensor::FakeSensor(const double rate_hz) : rate_hz_(rate_hz)
{
  socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_ < 0) throw std::runtime_error{"fake sensor socket failed"};
  (void)::fcntl(socket_, F_SETFL, ::fcntl(socket_, F_GETFL, 0) | O_NONBLOCK);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { ::close(socket_); socket_ = -1; throw std::runtime_error{"fake sensor bind failed"}; }
  socklen_t size = sizeof(address);
  ::getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &size);
  port_ = ntohs(address.sin_port);
  worker_ = std::thread(&FakeSensor::run, this);
}
FakeSensor::~FakeSensor() { stopping_ = true; ::shutdown(socket_, SHUT_RDWR); if (worker_.joinable()) worker_.join(); ::close(socket_); }
void FakeSensor::pause() noexcept { enabled_ = false; }
void FakeSensor::resume() noexcept { enabled_ = true; }
void FakeSensor::queue_payload(std::vector<std::uint8_t> payload) { std::lock_guard<std::mutex> lock(mutex_); payloads_.push_back(std::move(payload)); }
void FakeSensor::send_payload_now(std::vector<std::uint8_t> payload)
{
  ::sockaddr_in peer{};
  { std::lock_guard<std::mutex> lock(mutex_); if (!has_client_) throw std::runtime_error{"fake sensor has no client"}; peer = client_; }
  (void)::sendto(socket_, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}
void FakeSensor::queue_record(
  std::uint32_t rdt_sequence, std::uint32_t status, std::uint32_t ft_sequence,
  const std::array<std::int32_t, 6> axes)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (ft_sequence == 0) ft_sequence = ft_;
  std::vector<std::uint8_t> data(36); put32(data, 0, rdt_sequence); put32(data, 4, ft_sequence); put32(data, 8, status);
  for (std::size_t index = 0; index < axes.size(); ++index) {
    put32(data, 12 + 4 * index, static_cast<std::uint32_t>(axes[index]));
  }
  ft_ = ft_sequence + 4; payloads_.push_back(std::move(data));
}
void FakeSensor::skip_rdt(unsigned count) noexcept { std::lock_guard<std::mutex> lock(mutex_); skip_ += count; }
bool FakeSensor::wait_for_command(Command command, unsigned count, std::chrono::milliseconds timeout) const
{
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < until) { const auto seen = commands(); if (std::count(seen.begin(), seen.end(), command) >= static_cast<long>(count)) return true; std::this_thread::sleep_for(std::chrono::milliseconds{2}); }
  return false;
}
std::vector<Command> FakeSensor::commands() const { std::lock_guard<std::mutex> lock(mutex_); return commands_; }
std::vector<FakeSensor::CommandEvent> FakeSensor::command_events() const { std::lock_guard<std::mutex> lock(mutex_); return command_events_; }
void FakeSensor::record_command(const std::uint8_t * data, std::size_t size, const ::sockaddr_in & peer)
{
  if (size != 8 || data[0] != 0x12 || data[1] != 0x34) return;
  const auto command = static_cast<Command>((static_cast<std::uint16_t>(data[2]) << 8) | data[3]);
  { std::lock_guard<std::mutex> lock(mutex_); commands_.push_back(command); command_events_.push_back({command, std::chrono::steady_clock::now()}); if (command == Command::StartRealtime) { client_ = peer; has_client_ = true; rdt_ = 0; } }
  if (command == Command::StartRealtime) streaming_ = true;
  if (command == Command::StopStreaming || command == Command::SetSoftwareBias) streaming_ = false;
}
void FakeSensor::send_next()
{
  if (!streaming_ || !enabled_) return;
  std::vector<std::uint8_t> data;
  ::sockaddr_in peer{};
  { std::lock_guard<std::mutex> lock(mutex_); if (!has_client_) return; peer = client_; if (!payloads_.empty()) { data = std::move(payloads_.front()); payloads_.erase(payloads_.begin()); }
    if (data.empty()) { data.resize(36); rdt_ += 1 + skip_; skip_ = 0; put32(data, 0, rdt_); put32(data, 4, ft_); ft_ += 4; put32(data, 12, 100); put32(data, 16, static_cast<std::uint32_t>(-200)); put32(data, 20, 300); put32(data, 24, 10); put32(data, 28, static_cast<std::uint32_t>(-20)); put32(data, 32, 30); }
  }
  ::sendto(socket_, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}
void FakeSensor::run()
{
  auto next = std::chrono::steady_clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>{1.0 / rate_hz_});
  while (!stopping_) { ::sockaddr_in peer{}; socklen_t peer_size = sizeof(peer); std::array<std::uint8_t, 64> data{}; const auto size = ::recvfrom(socket_, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_size); if (size > 0) record_command(data.data(), static_cast<std::size_t>(size), peer); const auto now = std::chrono::steady_clock::now(); if (now >= next) { send_next(); next += interval; if (next < now) next = now + interval; } std::this_thread::sleep_for(std::chrono::microseconds{100}); }
}
}  // namespace netft_driver::test
