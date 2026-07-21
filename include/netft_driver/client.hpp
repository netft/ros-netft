#pragma once

#include "netft_driver/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <memory>
#include <deque>
#include <stdexcept>
#include <sys/socket.h>

namespace netft_driver {

struct NetFTClientTestAccess;

class NotConnectedError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class NetFTClient {
public:
  /**
   * The client object must outlive every callback invocation. In particular,
   * destroying the last owner from inside a callback is unsupported: C++17
   * cannot join the currently executing thread without deadlock or detach.
   */
  using SampleCallback = std::function<void(const WrenchSample &)>;
  enum class SessionResult { Stopped, Timeout, Socket, SeriousStatus, FtStall, FtBackward, MalformedStorm, Callback };
  explicit NetFTClient(ClientConfig config);
  ~NetFTClient();
  NetFTClient(const NetFTClient &) = delete;
  NetFTClient & operator=(const NetFTClient &) = delete;

  void start(SampleCallback callback);
  void stop() noexcept;
  void bias();
  bool wait_for_first_sample(std::chrono::duration<double> timeout);
  bool faulted() const noexcept { return fault_code_.load(std::memory_order_acquire) != FaultCode::None; }
  FaultCode fault_code() const noexcept { return fault_code_.load(std::memory_order_acquire); }
  HealthSnapshot health_snapshot() const;
  std::optional<WrenchSample> latest_sample() const;

private:
  using ThreadFactory = std::thread (*)(NetFTClient *);

  void run();
  std::thread create_worker_thread();
  SessionResult receive_session();
  std::optional<SessionResult> handle_record(const struct RawRecord & record,
                                              std::chrono::steady_clock::time_point received);
  void latch_fault(FaultCode code, const char * message) noexcept;
  void set_state(ClientState state);
  void set_error(const char * message);
  void record_callback_error(const char * message) noexcept;
  void close_session() noexcept;
  void send_command(std::uint16_t command);

  friend struct NetFTClientTestAccess;

  ClientConfig config_;
  mutable std::mutex lifecycle_mutex_, health_mutex_, socket_mutex_, record_mutex_;
  std::condition_variable lifecycle_cv_, first_sample_cv_;
  std::thread worker_;
  ThreadFactory thread_factory_{nullptr};
  std::thread::id active_worker_id_{};
  bool joining_{false};
  SampleCallback callback_;
  int socket_{-1};
  bool session_started_{false};
  sockaddr_storage endpoint_{};
  socklen_t endpoint_size_{};
  bool endpoint_ready_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> worker_exited_{true};
  std::atomic<bool> recovered_{false};
  std::atomic<FaultCode> fault_code_{FaultCode::None};
  HealthSnapshot health_;
  std::optional<WrenchSample> latest_;
  std::chrono::steady_clock::time_point last_publish_{};
  std::chrono::steady_clock::time_point last_valid_{};
  std::chrono::steady_clock::time_point session_started_at_{};
  std::uint64_t generation_{}, published_generation_{};
  mutable std::deque<std::chrono::steady_clock::time_point> receive_times_, publish_times_;
  std::unique_ptr<class RdtSequenceTracker> rdt_;
  std::unique_ptr<class FtSequenceTracker> ft_;
  std::uint64_t consecutive_malformed_{};
};

static_assert(std::atomic<FaultCode>::is_always_lock_free, "fault code must be lock-free");

}  // namespace netft_driver
