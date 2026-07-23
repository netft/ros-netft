#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include "netft/export.hpp"
#include "netft/types.hpp"

namespace netft {

struct DiscoveryOptions {
  std::string sensor_host{"192.168.1.1"};
  int http_port{80};
  std::chrono::duration<double> connect_timeout{0.5};
  std::chrono::duration<double> total_timeout{1.0};
};

class NETFT_API DiscoveryError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

NETFT_API SensorConfiguration discover_sensor(const DiscoveryOptions &options);

} // namespace netft
