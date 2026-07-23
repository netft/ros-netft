#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "netft/export.hpp"
#include "netft/types.hpp"

namespace netft {

class NETFT_API NotConnectedError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class NETFT_API Client {
public:
  using SampleCallback = std::function<void(const Sample &)>;

  explicit Client(Config config);
  ~Client();
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
  Client(Client &&) = delete;
  Client &operator=(Client &&) = delete;

  void start(SampleCallback callback);
  void stop() noexcept;
  void bias();
  bool wait_for_first_sample(std::chrono::duration<double> timeout);
  [[nodiscard]] bool faulted() const noexcept;
  [[nodiscard]] FaultCode fault_code() const noexcept;
  [[nodiscard]] HealthSnapshot health() const;
  [[nodiscard]] std::optional<Sample> latest_sample() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace netft
