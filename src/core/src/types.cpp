#include "netft/types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace netft {
namespace {

bool is_blank(std::string_view value) {
  return value.empty() || std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isspace(character) != 0;
         });
}

void require_positive_finite(std::string_view name, double value) {
  if (!std::isfinite(value) || value <= 0.0) {
    throw std::invalid_argument(std::string{name} + " must be finite and positive");
  }
}

void require_port(std::string_view name, int value) {
  if (value < 1 || value > 65535) {
    throw std::invalid_argument(std::string{name} + " must be in the range 1..65535");
  }
}

} // namespace

void validate(const Calibration &calibration) {
  require_positive_finite("counts_per_force_unit", calibration.counts_per_force_unit);
  require_positive_finite("counts_per_torque_unit", calibration.counts_per_torque_unit);
  if (calibration.force_unit == ForceUnit::Unknown ||
      calibration.torque_unit == TorqueUnit::Unknown) {
    throw std::invalid_argument("calibration units must be known");
  }
}

void validate(const Config &config) {
  if (is_blank(config.sensor_host)) {
    throw std::invalid_argument("sensor_host must not be blank");
  }
  require_port("rdt_port", config.rdt_port);
  require_port("http_port", config.http_port);
  require_positive_finite("receive_timeout", config.receive_timeout.count());
  require_positive_finite("configuration_connect_timeout",
                          config.configuration_connect_timeout.count());
  require_positive_finite("configuration_timeout", config.configuration_timeout.count());
  require_positive_finite("reconnect_initial_delay", config.reconnect_initial_delay.count());
  require_positive_finite("reconnect_max_delay", config.reconnect_max_delay.count());
  if (config.reconnect_max_delay < config.reconnect_initial_delay) {
    throw std::invalid_argument("reconnect_max_delay must not be below reconnect_initial_delay");
  }
  if (!std::isfinite(config.sample_rate_limit_hz) || config.sample_rate_limit_hz < 0.0) {
    throw std::invalid_argument("sample_rate_limit_hz must be finite and non-negative");
  }
  if (config.calibration_override.has_value()) {
    validate(*config.calibration_override);
  }
}

std::string_view to_string(ForceUnit unit) noexcept {
  switch (unit) {
  case ForceUnit::PoundForce:
    return "lbf";
  case ForceUnit::Newton:
    return "N";
  case ForceUnit::KiloPoundForce:
    return "klbf";
  case ForceUnit::KiloNewton:
    return "kN";
  case ForceUnit::KilogramForce:
    return "kgf";
  case ForceUnit::Unknown:
    return "unknown";
  }
  return "unknown";
}

std::string_view to_string(TorqueUnit unit) noexcept {
  switch (unit) {
  case TorqueUnit::PoundForceInch:
    return "lbf-in";
  case TorqueUnit::PoundForceFoot:
    return "lbf-ft";
  case TorqueUnit::NewtonMeter:
    return "N-m";
  case TorqueUnit::NewtonMillimeter:
    return "N-mm";
  case TorqueUnit::KilogramForceCentimeter:
    return "kgf-cm";
  case TorqueUnit::KiloNewtonMeter:
    return "kN-m";
  case TorqueUnit::Unknown:
    return "unknown";
  }
  return "unknown";
}

std::optional<ForceUnit> force_unit_from_string(std::string_view value) noexcept {
  for (const auto unit :
       {ForceUnit::Unknown, ForceUnit::PoundForce, ForceUnit::Newton, ForceUnit::KiloPoundForce,
        ForceUnit::KiloNewton, ForceUnit::KilogramForce}) {
    if (to_string(unit) == value) {
      return unit;
    }
  }
  return std::nullopt;
}

std::optional<TorqueUnit> torque_unit_from_string(std::string_view value) noexcept {
  for (const auto unit :
       {TorqueUnit::Unknown, TorqueUnit::PoundForceInch, TorqueUnit::PoundForceFoot,
        TorqueUnit::NewtonMeter, TorqueUnit::NewtonMillimeter, TorqueUnit::KilogramForceCentimeter,
        TorqueUnit::KiloNewtonMeter}) {
    if (to_string(unit) == value) {
      return unit;
    }
  }
  return std::nullopt;
}

std::string_view to_string(ClientState state) noexcept {
  switch (state) {
  case ClientState::Stopped:
    return "stopped";
  case ClientState::Connecting:
    return "connecting";
  case ClientState::Streaming:
    return "streaming";
  case ClientState::Backoff:
    return "backoff";
  case ClientState::Faulted:
    return "faulted";
  }
  return "unknown";
}

std::string_view to_string(FaultCode code) noexcept {
  switch (code) {
  case FaultCode::None:
    return "none";
  case FaultCode::SensorConfiguration:
    return "sensor_configuration";
  case FaultCode::Timeout:
    return "timeout";
  case FaultCode::Socket:
    return "socket";
  case FaultCode::SeriousStatus:
    return "serious_status";
  case FaultCode::FtStall:
    return "ft_stall";
  case FaultCode::FtBackward:
    return "ft_backward";
  case FaultCode::MalformedStorm:
    return "malformed_storm";
  case FaultCode::Callback:
    return "callback";
  }
  return "unknown";
}

} // namespace netft
