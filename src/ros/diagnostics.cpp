#include "ros/diagnostics.hpp"

#include "netft/status.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace netft_driver {
namespace {

std::string number(const double value, const int precision)
{
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << value;
  return output.str();
}

std::string number(const std::uint64_t value)
{
  return std::to_string(value);
}

std::string calibration_number(const double value)
{
  std::ostringstream output;
  output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return output.str();
}

std::string sequence(const std::optional<std::uint32_t> & value)
{
  return value ? std::to_string(*value) : "none";
}

std::string status_code(const std::uint32_t status)
{
  std::ostringstream output;
  output << "0x" << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << status;
  return output.str();
}

std::string_view configuration_source(const netft::CalibrationSource source)
{
  return source == netft::CalibrationSource::Sensor ? "sensor" : "override";
}

std::uint64_t delta(const std::uint64_t current, std::uint64_t & previous)
{
  const auto difference = current >= previous ? current - previous : 0;
  previous = current;
  return difference;
}

}  // namespace

DiagnosticEvaluator::DiagnosticEvaluator(const double expected_rdt_rate, const double rate_tolerance)
: expected_rdt_rate_(expected_rdt_rate), rate_tolerance_(rate_tolerance)
{
  if (!std::isfinite(expected_rdt_rate) || expected_rdt_rate <= 0.0) {
    throw std::invalid_argument{"expected_rdt_rate must be finite and greater than zero"};
  }
  if (!std::isfinite(rate_tolerance) || rate_tolerance < 0.0 || rate_tolerance > 1.0) {
    throw std::invalid_argument{"rate_tolerance must be between 0 and 1"};
  }
}

DiagnosticReport DiagnosticEvaluator::evaluate(const netft::HealthSnapshot & snapshot)
{
  std::vector<std::pair<std::string, std::string>> values{
    {"state", std::string{netft::to_string(snapshot.state)}},
    {"sensor", snapshot.sensor_host + ":" + std::to_string(snapshot.rdt_port)},
    {"last_rdt_sequence", sequence(snapshot.last_rdt_sequence)},
    {"last_ft_sequence", sequence(snapshot.last_ft_sequence)},
    {"last_ft_progress", snapshot.last_ft_progress},
    {"device_status", status_code(snapshot.last_status)},
    {"active_status", netft::decode_status(snapshot.last_status)},
    {"receive_rate_hz", number(snapshot.receive_rate_hz, 1)},
    {"expected_receive_rate_hz", number(expected_rdt_rate_, 1)},
    {"rate_tolerance", number(rate_tolerance_, 3)},
    {"delivery_rate_hz", number(snapshot.delivery_rate_hz, 1)},
    {"received_count", number(snapshot.received_count)},
    {"delivered_count", number(snapshot.delivered_count)},
    {"rate_limited_count", number(snapshot.rate_limited_count)},
    {"device_error_count", number(snapshot.device_error_count)},
    {"lost_count", number(snapshot.lost_count)},
    {"duplicate_count", number(snapshot.duplicate_count)},
    {"out_of_order_count", number(snapshot.out_of_order_count)},
    {"ft_stall_count", number(snapshot.ft_stall_count)},
    {"ft_backward_count", number(snapshot.ft_backward_count)},
    {"ft_restart_count", number(snapshot.ft_restart_count)},
    {"malformed_count", number(snapshot.malformed_count)},
    {"malformed_storm_threshold", number(kMalformedStormThreshold)},
    {"malformed_storm_window", "between_diagnostic_updates"},
    {"reconnect_count", number(snapshot.reconnect_count)},
    {"timeout_count", number(snapshot.timeout_count)},
    {"callback_error_count", number(snapshot.callback_error_count)},
    {"last_record_age_s",
      snapshot.last_record_age ? number(snapshot.last_record_age->count(), 3) : "none"},
    {"last_error", snapshot.last_error},
  };

  if (snapshot.sensor_configuration) {
    const auto & configuration = *snapshot.sensor_configuration;
    values.emplace_back("configuration_source",
      std::string{configuration_source(configuration.source)});
    values.emplace_back("force_unit",
      std::string{netft::to_string(configuration.calibration.force_unit)});
    values.emplace_back("torque_unit",
      std::string{netft::to_string(configuration.calibration.torque_unit)});
    values.emplace_back("configuration_revision", number(configuration.revision));
    values.emplace_back("calibration_change_count", number(snapshot.calibration_change_count));
    values.emplace_back("counts_per_force_unit",
      calibration_number(configuration.calibration.counts_per_force_unit));
    values.emplace_back("counts_per_torque_unit",
      calibration_number(configuration.calibration.counts_per_torque_unit));
  }

  const auto report = [&](const int level, std::string message, const char * log_key) {
    return DiagnosticReport{level, std::move(message), values, log_key};
  };

  if (snapshot.state == netft::ClientState::Faulted ||
    snapshot.fault_code != netft::FaultCode::None)
  {
    const auto detail = snapshot.last_error.empty() ?
      std::string{netft::to_string(snapshot.fault_code)} : snapshot.last_error;
    return report(2, "faulted: " + detail, "faulted");
  }

  const auto lost = delta(snapshot.lost_count, lost_);
  const auto device_error = delta(snapshot.device_error_count, device_error_);
  const auto timeout = delta(snapshot.timeout_count, timeout_);
  const auto malformed = delta(snapshot.malformed_count, malformed_);
  const auto ft_stall = delta(snapshot.ft_stall_count, ft_stall_);
  const auto ft_backward = delta(snapshot.ft_backward_count, ft_backward_);
  const auto ft_restart = delta(snapshot.ft_restart_count, ft_restart_);
  const auto callback_error = delta(snapshot.callback_error_count, callback_error_);

  if (snapshot.state == netft::ClientState::Backoff) {
    return report(2, "connection lost; reconnecting", "backoff");
  }
  if (snapshot.state == netft::ClientState::Stopped) {
    return report(2, "client stopped", "stopped");
  }

  const auto status_severity = netft::classify_status(snapshot.last_status);
  if (status_severity == netft::StatusSeverity::Error) {
    return report(2, "device error: " + netft::decode_status(snapshot.last_status), "device_error");
  }
  if (device_error != 0) {
    return report(2, "serious device status observed since last diagnostic", "device_error_event");
  }
  if (timeout != 0) {
    return report(2, "receive timeout observed since last diagnostic", "receive_timeout");
  }
  if (malformed >= kMalformedStormThreshold) {
    return report(2, "malformed-packet storm observed since last diagnostic", "malformed_storm");
  }
  if (ft_stall != 0) {
    return report(2, "FT sequence stalled since last diagnostic", "ft_stall");
  }
  if (ft_backward != 0) {
    return report(2, "FT sequence moved backward since last diagnostic", "ft_backward");
  }
  if (snapshot.state == netft::ClientState::Connecting) {
    return report(1, "waiting for first RDT record", "connecting");
  }
  if (status_severity == netft::StatusSeverity::Warn) {
    return report(1, "monitor condition latched", "condition_latch");
  }
  if (ft_restart != 0) {
    return report(1, "FT device counter restarted since last diagnostic", "ft_restart");
  }
  if (lost != 0) {
    return report(1, "RDT records lost since last diagnostic", "packet_loss");
  }

  const auto lower_rate = expected_rdt_rate_ * (1.0 - rate_tolerance_);
  const auto upper_rate = expected_rdt_rate_ * (1.0 + rate_tolerance_);
  if (snapshot.receive_rate_hz < lower_rate || snapshot.receive_rate_hz > upper_rate) {
    return report(1, "receive rate outside configured tolerance", "receive_rate");
  }
  if (callback_error != 0) {
    return report(1, "sample callback failed since last diagnostic", "callback_error");
  }
  return report(0, "streaming normally", "healthy");
}

FaultLogThrottle::FaultLogThrottle(const double repeat_interval)
: repeat_interval_(repeat_interval)
{
  if (!std::isfinite(repeat_interval) || repeat_interval <= 0.0) {
    throw std::invalid_argument{"repeat_interval must be finite and greater than zero"};
  }
}

bool FaultLogThrottle::should_log(const DiagnosticReport & report, const double now)
{
  if (report.level <= 0) {
    active_ = false;
    return false;
  }

  if (!active_ || report.level != level_ || report.log_key != key_ ||
    now - last_log_ >= repeat_interval_)
  {
    active_ = true;
    level_ = report.level;
    key_ = report.log_key;
    last_log_ = now;
    return true;
  }
  return false;
}

}  // namespace netft_driver
