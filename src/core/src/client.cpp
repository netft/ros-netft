#include "netft/client.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "detail/client_impl.hpp"

namespace netft {
namespace {

template <typename Value> class DeferredDestroyer {
public:
  DeferredDestroyer() : worker_(&DeferredDestroyer::run, this) {}

  ~DeferredDestroyer() {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_one();
    worker_.join();
  }

  void enqueue(std::unique_ptr<Value> value) {
    {
      std::scoped_lock lock(mutex_);
      pending_.push_back(std::move(value));
    }
    condition_.notify_one();
  }

private:
  void run() noexcept {
    for (;;) {
      std::unique_ptr<Value> value;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
        if (pending_.empty()) {
          return;
        }
        value = std::move(pending_.front());
        pending_.pop_front();
      }
      value->stop();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::unique_ptr<Value>> pending_;
  bool stopping_{};
  std::thread worker_;
};

} // namespace

Client::Client(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() {
  if (impl_ && impl_->called_from_worker_thread()) {
    impl_->stop();
    static DeferredDestroyer<Impl> deferred;
    deferred.enqueue(std::move(impl_));
    return;
  }
  stop();
}

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
