#pragma once

#include "netft_driver/protocol.hpp"

#include <array>
#include <atomic>
#include <netinet/in.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace netft_driver::test {

class FakeSensor {
public:
  struct CommandEvent { Command command; std::chrono::steady_clock::time_point at; };
  explicit FakeSensor(double rate_hz = 200.0);
  ~FakeSensor();
  FakeSensor(const FakeSensor &) = delete;
  FakeSensor & operator=(const FakeSensor &) = delete;

  const std::string & host() const noexcept { return host_; }
  int port() const noexcept { return port_; }
  void pause() noexcept;
  void resume() noexcept;
  void queue_record(std::uint32_t rdt_sequence, std::uint32_t status = 0,
                    std::uint32_t ft_sequence = 0,
                    std::array<std::int32_t, 6> axes = {100, -200, 300, 10, -20, 30});
  void queue_payload(std::vector<std::uint8_t> payload);
  void send_payload_now(std::vector<std::uint8_t> payload);
  void skip_rdt(unsigned count) noexcept;
  bool wait_for_command(Command command, unsigned count = 1,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) const;
  std::vector<Command> commands() const;
  std::vector<CommandEvent> command_events() const;

private:
  void run();
  void send_next();
  void record_command(const std::uint8_t * data, std::size_t size,
                      const ::sockaddr_in & peer);

  std::string host_{"127.0.0.1"};
  int port_{};
  int socket_{};
  double rate_hz_{};
  std::atomic<bool> stopping_{false}, enabled_{true}, streaming_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::vector<Command> commands_;
  std::vector<CommandEvent> command_events_;
  std::vector<std::vector<std::uint8_t>> payloads_;
  ::sockaddr_in client_{};
  bool has_client_{false};
  std::uint32_t rdt_{}, ft_{1000}, skip_{};
};

}  // namespace netft_driver::test
