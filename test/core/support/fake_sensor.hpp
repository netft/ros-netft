#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include "detail/protocol.hpp"
#include "support/fake_http_server.hpp"

namespace netft::test {

class FakeSensor {
public:
  struct CommandEvent {
    detail::Command command;
    std::chrono::steady_clock::time_point at;
  };

  explicit FakeSensor(double rate_hz = 200.0);
  ~FakeSensor();
  FakeSensor(const FakeSensor &) = delete;
  FakeSensor &operator=(const FakeSensor &) = delete;

  const std::string &host() const noexcept { return host_; }
  int rdt_port() const noexcept { return rdt_port_; }
  int http_port() const noexcept { return http_.port(); }
  std::uint64_t http_request_count() const noexcept { return http_.request_count(); }

  void pause() noexcept;
  void resume() noexcept;
  void queue_record(std::uint32_t rdt_sequence, std::uint32_t status = 0,
                    std::uint32_t ft_sequence = 0,
                    std::array<std::int32_t, 6> axes = {100, -200, 300, 10, -20, 30});
  void send_record_now(std::uint32_t rdt_sequence, std::uint32_t status = 0,
                       std::uint32_t ft_sequence = 0,
                       std::array<std::int32_t, 6> axes = {100, -200, 300, 10, -20, 30});
  void queue_payload(std::vector<std::uint8_t> payload);
  void send_payload_now(std::vector<std::uint8_t> payload);
  void skip_rdt(unsigned count) noexcept;
  bool wait_for_command(detail::Command command, unsigned count = 1,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) const;
  bool wait_for_http_request(unsigned count = 1,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{
                                 1000}) const;
  std::vector<detail::Command> commands() const;
  std::vector<CommandEvent> command_events() const;
  void set_xml_configuration(std::string xml, int status = 200);
  void set_http_response_delay(std::chrono::milliseconds delay);

private:
  void run();
  void send_next();
  void record_command(const std::uint8_t *data, std::size_t size, const ::sockaddr_in &peer);

  std::string host_{"127.0.0.1"};
  FakeHttpServer http_;
  int rdt_port_{};
  int socket_{-1};
  double rate_hz_{};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> enabled_{true};
  std::atomic<bool> streaming_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::vector<detail::Command> commands_;
  std::vector<CommandEvent> command_events_;
  std::vector<std::vector<std::uint8_t>> payloads_;
  ::sockaddr_in client_{};
  bool has_client_{false};
  std::uint32_t rdt_{};
  std::uint32_t ft_{1000};
  std::uint32_t skip_{};
};

} // namespace netft::test
