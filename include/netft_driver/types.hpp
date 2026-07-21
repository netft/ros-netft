#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace netft_driver {

enum class RecoveryPolicy { Reconnect, FailStop };
enum class ClientState { Stopped, Connecting, Streaming, Backoff, Faulted };
enum class FaultCode { None, Timeout, Socket, SeriousStatus, FtStall,
                       FtBackward, MalformedStorm, Callback };

struct ClientConfig {
  std::string sensor_host{"192.168.31.100"};
  int sensor_port{49152};
  double counts_per_force{1000000.0};
  double counts_per_torque{1000000.0};
  double publish_rate{0.0};
  std::chrono::duration<double> receive_timeout{0.1};
  std::chrono::duration<double> reconnect_initial_delay{0.25};
  std::chrono::duration<double> reconnect_max_delay{5.0};
  bool publish_on_error{false};
  RecoveryPolicy recovery_policy{RecoveryPolicy::Reconnect};
};

struct WrenchSample {
  std::uint32_t rdt_sequence{}, ft_sequence{}, status{};
  std::array<double, 3> force{}, torque{};
  std::chrono::steady_clock::time_point received_at{};
};

struct HealthSnapshot {
  ClientState state{ClientState::Stopped};
  FaultCode fault_code{FaultCode::None};
  std::string sensor_host{};
  int sensor_port{};
  std::optional<std::uint32_t> last_rdt_sequence{};
  std::optional<std::uint32_t> last_ft_sequence{};
  std::uint32_t last_status{};
  double receive_rate{};
  double publish_rate{};
  std::uint64_t received_count{};
  std::uint64_t published_count{};
  std::uint64_t rate_dropped_count{};
  std::uint64_t device_error_count{};
  std::uint64_t warning_count{};
  std::uint64_t lost_count{};
  std::uint64_t duplicate_count{};
  std::uint64_t out_of_order_count{};
  std::uint64_t malformed_count{};
  std::uint64_t reconnect_count{};
  std::uint64_t timeout_count{};
  std::uint64_t callback_error_count{};
  std::optional<std::chrono::duration<double>> last_record_age{};
  std::string last_error{};
  std::uint64_t ft_stall_count{};
  std::uint64_t ft_backward_count{};
  std::uint64_t ft_restart_count{};
  std::string last_ft_progress{"unknown"};
};

inline void validate(const ClientConfig & config)
{
  if (config.sensor_host.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
    throw std::invalid_argument{"sensor_host must be non-empty"};
  }
  if (config.sensor_port < 1 || config.sensor_port > 65535) {
    throw std::invalid_argument{"sensor_port must be between 1 and 65535"};
  }

  const auto positive_finite = [](const char * field, double value) {
    if (!std::isfinite(value) || value <= 0.0) {
      throw std::invalid_argument{std::string{field} + " must be finite and greater than zero"};
    }
  };
  positive_finite("counts_per_force", config.counts_per_force);
  positive_finite("counts_per_torque", config.counts_per_torque);

  if (!std::isfinite(config.publish_rate) || config.publish_rate < 0.0) {
    throw std::invalid_argument{"publish_rate must be finite and non-negative"};
  }

  positive_finite("receive_timeout", config.receive_timeout.count());
  positive_finite("reconnect_initial_delay", config.reconnect_initial_delay.count());
  positive_finite("reconnect_max_delay", config.reconnect_max_delay.count());
  if (config.reconnect_max_delay < config.reconnect_initial_delay) {
    throw std::invalid_argument{
      "reconnect_max_delay must be at least reconnect_initial_delay"};
  }
}

}  // namespace netft_driver
