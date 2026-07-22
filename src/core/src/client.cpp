#include "netft/client.hpp"

#include <utility>

#include "detail/client_impl.hpp"

namespace netft {

Client::Client(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() { stop(); }

void Client::start(SampleCallback callback) { impl_->start(std::move(callback)); }

void Client::stop() noexcept { impl_->stop(); }

void Client::bias() { impl_->bias(); }

bool Client::wait_for_first_sample(const std::chrono::duration<double> timeout) {
  return impl_->wait_for_first_sample(timeout);
}

bool Client::faulted() const noexcept { return impl_->faulted(); }

FaultCode Client::fault_code() const noexcept { return impl_->fault_code(); }

HealthSnapshot Client::health() const { return impl_->health(); }

std::optional<Sample> Client::latest_sample() const { return impl_->latest_sample(); }

} // namespace netft
