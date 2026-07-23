#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "netft/export.hpp"

namespace netft {

enum class ForceUnit { Unknown, PoundForce, Newton, KiloPoundForce, KiloNewton, KilogramForce };
enum class TorqueUnit {
  Unknown,
  PoundForceInch,
  PoundForceFoot,
  NewtonMeter,
  NewtonMillimeter,
  KilogramForceCentimeter,
  KiloNewtonMeter
};
enum class CalibrationSource { Sensor, Override };
enum class RecoveryPolicy { Reconnect, FailStop };
enum class ClientState { Stopped, Connecting, Streaming, Backoff, Faulted };
enum class FaultCode {
  None,
  SensorConfiguration,
  Timeout,
  Socket,
  SeriousStatus,
  FtStall,
  FtBackward,
  MalformedStorm,
  Callback
};

struct Calibration {
  double counts_per_force_unit{};
  double counts_per_torque_unit{};
  ForceUnit force_unit{ForceUnit::Unknown};
  TorqueUnit torque_unit{TorqueUnit::Unknown};
};

struct SensorConfiguration {
  std::string product_name;
  Calibration calibration;
  CalibrationSource source{CalibrationSource::Sensor};
  std::uint64_t revision{1};
};

struct Config {
  std::string sensor_host{"192.168.1.1"};
  int rdt_port{49152};
  int http_port{80};
  std::chrono::duration<double> receive_timeout{0.1};
  std::chrono::duration<double> configuration_connect_timeout{0.5};
  std::chrono::duration<double> configuration_timeout{1.0};
  std::chrono::duration<double> reconnect_initial_delay{0.25};
  std::chrono::duration<double> reconnect_max_delay{5.0};
  double sample_rate_limit_hz{0.0};
  bool deliver_samples_with_error_status{false};
  RecoveryPolicy recovery_policy{RecoveryPolicy::Reconnect};
  std::optional<Calibration> calibration_override;
};

struct Sample {
  std::uint32_t rdt_sequence{}, ft_sequence{}, status{};
  std::array<double, 3> force{}, torque{};
  ForceUnit force_unit{ForceUnit::Unknown};
  TorqueUnit torque_unit{TorqueUnit::Unknown};
  std::uint64_t configuration_revision{};
  std::chrono::steady_clock::time_point received_at;
};

struct HealthSnapshot {
  ClientState state{ClientState::Stopped};
  FaultCode fault_code{FaultCode::None};
  std::string sensor_host;
  int rdt_port{};
  std::optional<SensorConfiguration> sensor_configuration;
  std::optional<std::uint32_t> last_rdt_sequence, last_ft_sequence;
  std::uint32_t last_status{};
  double receive_rate_hz{}, delivery_rate_hz{};
  std::uint64_t received_count{}, delivered_count{}, rate_limited_count{};
  std::uint64_t device_error_count{}, warning_count{}, lost_count{};
  std::uint64_t duplicate_count{}, out_of_order_count{}, malformed_count{};
  std::uint64_t reconnect_count{}, timeout_count{}, callback_error_count{};
  std::uint64_t ft_stall_count{}, ft_backward_count{}, ft_restart_count{};
  std::uint64_t calibration_change_count{};
  std::optional<std::chrono::duration<double>> last_record_age;
  std::string last_error;
  std::string last_ft_progress{"unknown"};
};

NETFT_API void validate(const Calibration &calibration);
NETFT_API void validate(const Config &config);
NETFT_API std::string_view to_string(ForceUnit unit) noexcept;
NETFT_API std::string_view to_string(TorqueUnit unit) noexcept;
NETFT_API std::optional<ForceUnit> force_unit_from_string(std::string_view value) noexcept;
NETFT_API std::optional<TorqueUnit> torque_unit_from_string(std::string_view value) noexcept;
NETFT_API std::string_view to_string(ClientState state) noexcept;
NETFT_API std::string_view to_string(FaultCode code) noexcept;

} // namespace netft
