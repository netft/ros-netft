#pragma once

#include "netft_driver/types.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

namespace netft_driver {

struct AdapterParameters {
  std::string sensor_ip{"192.168.31.100"};
  int sensor_port{49152};
  std::string frame_id{"netft_link"};
  std::string wrench_topic{"/netft/wrench"};
  std::string bias_service{"/netft/bias"};
  double counts_per_force{1000000.0};
  double counts_per_torque{1000000.0};
  double publish_rate{0.0};
  double receive_timeout{0.1};
  double reconnect_initial_delay{0.25};
  double reconnect_max_delay{5.0};
  double diagnostics_rate{1.0};
  double expected_rdt_rate{2000.0};
  double rate_tolerance{0.2};
  bool publish_on_error{false};
};

struct AdapterConfig {
  ClientConfig client;
  std::string frame_id;
  std::string wrench_topic;
  std::string bias_service;
  double diagnostics_rate;
  double expected_rdt_rate;
  double rate_tolerance;
};

inline void validate_adapter_config(const std::string & frame_id, const std::string & wrench_topic,
                                    const std::string & bias_service, const double diagnostics_rate)
{
  const auto non_empty = [](const char * name, const std::string & value) {
    if (value.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
      throw std::invalid_argument{std::string{name} + " must be non-empty"};
    }
  };
  non_empty("frame_id", frame_id);
  non_empty("wrench_topic", wrench_topic);
  non_empty("bias_service", bias_service);
  if (!std::isfinite(diagnostics_rate) || diagnostics_rate <= 0.0) {
    throw std::invalid_argument{"diagnostics_rate must be finite and greater than zero"};
  }
}

inline AdapterConfig map_adapter_parameters(const AdapterParameters & parameters)
{
  AdapterConfig mapped;
  mapped.client.sensor_host = parameters.sensor_ip;
  mapped.client.sensor_port = parameters.sensor_port;
  mapped.frame_id = parameters.frame_id;
  mapped.wrench_topic = parameters.wrench_topic;
  mapped.bias_service = parameters.bias_service;
  mapped.client.counts_per_force = parameters.counts_per_force;
  mapped.client.counts_per_torque = parameters.counts_per_torque;
  mapped.client.publish_rate = parameters.publish_rate;
  mapped.client.receive_timeout = std::chrono::duration<double>{parameters.receive_timeout};
  mapped.client.reconnect_initial_delay =
    std::chrono::duration<double>{parameters.reconnect_initial_delay};
  mapped.client.reconnect_max_delay =
    std::chrono::duration<double>{parameters.reconnect_max_delay};
  mapped.diagnostics_rate = parameters.diagnostics_rate;
  mapped.expected_rdt_rate = parameters.expected_rdt_rate;
  mapped.rate_tolerance = parameters.rate_tolerance;
  mapped.client.publish_on_error = parameters.publish_on_error;

  validate(mapped.client);
  validate_adapter_config(mapped.frame_id, mapped.wrench_topic, mapped.bias_service,
                          mapped.diagnostics_rate);
  if (!std::isfinite(mapped.expected_rdt_rate) || mapped.expected_rdt_rate <= 0.0) {
    throw std::invalid_argument{"expected_rdt_rate must be finite and greater than zero"};
  }
  if (!std::isfinite(mapped.rate_tolerance) || mapped.rate_tolerance < 0.0 ||
      mapped.rate_tolerance > 1.0) {
    throw std::invalid_argument{"rate_tolerance must be between 0 and 1"};
  }
  return mapped;
}

}  // namespace netft_driver
