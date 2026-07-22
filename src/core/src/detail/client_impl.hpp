#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "detail/fault_latch.hpp"
#include "detail/posix_transport.hpp"
#include "detail/protocol.hpp"
#include "detail/sequence.hpp"
#include "netft/client.hpp"

namespace netft {

class NETFT_LOCAL Client::Impl {
public:
  explicit Impl(Config config);
  ~Impl();

  void start(SampleCallback callback);
  void stop() noexcept;
  void bias();
  bool wait_for_first_sample(std::chrono::duration<double> timeout);
  bool faulted() const noexcept;
  FaultCode fault_code() const noexcept;
  HealthSnapshot health() const;
  std::optional<Sample> latest_sample() const;

private:
  enum class SessionResult {
    Stopped,
    SensorConfiguration,
    Timeout,
    Socket,
    SeriousStatus,
    FtStall,
    FtBackward,
    MalformedStorm,
    Callback,
  };

  struct SessionOutcome {
    SessionResult result{SessionResult::Stopped};
    std::string message;
    bool received_valid_record{false};
  };

  void run() noexcept;
  std::thread create_worker_thread();
  static FaultCode fault_for(SessionResult result) noexcept;
  static const char *message_for(SessionResult result) noexcept;
  SessionOutcome receive_session();
  SensorConfiguration configuration_for_session();
  void apply_configuration(SensorConfiguration configuration);
  std::optional<SessionResult> handle_record(const detail::RawRecord &record,
                                             std::chrono::steady_clock::time_point received_at);
  void close_session() noexcept;
  void set_fault(FaultCode code, std::string message) noexcept;
  void record_callback_error(const char *message) noexcept;
  void finish_session() noexcept;

  using ThreadFactory = std::thread (*)(Impl *);
  using FaultPublishedHook = void (*)(Impl *) noexcept;
  using WaitWakeHook = void (*)(Impl *, std::uint64_t) noexcept;

  Config config_;
  mutable std::mutex lifecycle_mutex_;
  std::condition_variable lifecycle_cv_;
  // Locks are acquired in command -> record -> data order. Callbacks run after
  // all three have been released.
  mutable std::mutex command_mutex_;
  mutable std::mutex record_mutex_;
  mutable std::mutex data_mutex_;
  std::condition_variable first_sample_cv_;
  std::thread worker_;
  ThreadFactory thread_factory_{nullptr};
  // Internal synchronization hooks used only by lifecycle tests. Client's
  // installed API remains opaque and does not expose these seams.
  FaultPublishedHook fault_published_test_hook_{nullptr};
  WaitWakeHook wait_wake_test_hook_{nullptr};
  std::thread::id active_worker_id_;
  bool joining_{false};
  SampleCallback callback_;
  detail::PosixTransport transport_;
  detail::RdtSequenceTracker rdt_sequence_;
  detail::FtSequenceTracker ft_sequence_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> worker_exited_{true};
  bool session_started_{false};
  detail::FaultLatch fault_latch_;
  HealthSnapshot health_;
  std::optional<Sample> latest_;
  std::chrono::steady_clock::time_point last_delivery_at_;
  std::chrono::steady_clock::time_point last_record_at_;
  std::uint64_t generation_{0};
  std::uint64_t delivered_generation_{0};
  mutable std::deque<std::chrono::steady_clock::time_point> receive_times_;
  mutable std::deque<std::chrono::steady_clock::time_point> delivery_times_;
  std::uint64_t consecutive_malformed_{};
};

} // namespace netft
