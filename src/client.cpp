#include "netft_driver/client.hpp"

#include "netft_driver/protocol.hpp"
#include "netft_driver/status.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

namespace netft_driver {
namespace {
FaultCode fault_for(NetFTClient::SessionResult result)
{
  switch (result) {
    case NetFTClient::SessionResult::Timeout: return FaultCode::Timeout;
    case NetFTClient::SessionResult::Socket: return FaultCode::Socket;
    case NetFTClient::SessionResult::SeriousStatus: return FaultCode::SeriousStatus;
    case NetFTClient::SessionResult::FtStall: return FaultCode::FtStall;
    case NetFTClient::SessionResult::FtBackward: return FaultCode::FtBackward;
    case NetFTClient::SessionResult::MalformedStorm: return FaultCode::MalformedStorm;
    case NetFTClient::SessionResult::Callback: return FaultCode::Callback;
    case NetFTClient::SessionResult::Stopped: return FaultCode::None;
  }
  return FaultCode::Socket;
}
const char * message_for(NetFTClient::SessionResult result)
{
  switch (result) {
    case NetFTClient::SessionResult::Timeout: return "no valid RDT record before timeout";
    case NetFTClient::SessionResult::Socket: return "UDP socket failure";
    case NetFTClient::SessionResult::SeriousStatus: return "serious device status";
    case NetFTClient::SessionResult::FtStall: return "FT sequence stalled";
    case NetFTClient::SessionResult::FtBackward: return "FT sequence moved backward";
    case NetFTClient::SessionResult::MalformedStorm: return "malformed packet storm";
    case NetFTClient::SessionResult::Callback: return "sample callback failed";
    case NetFTClient::SessionResult::Stopped: return "";
  }
  return "UDP socket failure";
}
}

NetFTClient::NetFTClient(ClientConfig config) : config_(std::move(config))
{
  validate(config_);
  rdt_ = std::make_unique<RdtSequenceTracker>();
  ft_ = std::make_unique<FtSequenceTracker>();
  health_.sensor_host = config_.sensor_host; health_.sensor_port = config_.sensor_port;
  addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
  addrinfo * result = nullptr;
  if (::getaddrinfo(config_.sensor_host.c_str(), std::to_string(config_.sensor_port).c_str(), &hints, &result) == 0) {
    std::memcpy(&endpoint_, result->ai_addr, result->ai_addrlen);
    endpoint_size_ = static_cast<socklen_t>(result->ai_addrlen);
    endpoint_ready_ = true;
    ::freeaddrinfo(result);
  }
}
NetFTClient::~NetFTClient() { stop(); }

void NetFTClient::start(SampleCallback callback)
{
  for (;;) {
    std::thread reaped_worker;
    {
      std::unique_lock<std::mutex> lock(lifecycle_mutex_);
      if (!worker_exited_.load(std::memory_order_acquire) && active_worker_id_ == std::this_thread::get_id()) throw std::runtime_error{"client is already running"};
      lifecycle_cv_.wait(lock, [&] { return !joining_; });
      if (worker_.joinable()) {
        if (!worker_exited_.load(std::memory_order_acquire)) throw std::runtime_error{"client is already running"};
        joining_ = true;
        reaped_worker = std::move(worker_);
      } else {
        const auto previous_stopping = stopping_.load(std::memory_order_relaxed);
        const auto previous_worker_exited = worker_exited_.load(std::memory_order_relaxed);
        const auto previous_fault_code = fault_code_.load(std::memory_order_relaxed);
        auto previous_callback = std::move(callback_);
        std::chrono::steady_clock::time_point previous_last_publish;
        {
          std::lock_guard<std::mutex> record_lock(record_mutex_);
          previous_last_publish = last_publish_;
          last_publish_ = {};
        }
        ClientState previous_state;
        FaultCode previous_health_fault_code;
        std::string previous_last_error;
        std::optional<WrenchSample> previous_latest;
        std::chrono::steady_clock::time_point previous_last_valid;
        std::chrono::steady_clock::time_point previous_session_started_at;
        std::uint64_t previous_generation;
        std::uint64_t previous_published_generation;
        std::deque<std::chrono::steady_clock::time_point> previous_receive_times;
        std::deque<std::chrono::steady_clock::time_point> previous_publish_times;
        double previous_receive_rate;
        double previous_publish_rate;
        {
          std::lock_guard<std::mutex> health_lock(health_mutex_);
          previous_state = health_.state;
          previous_health_fault_code = health_.fault_code;
          previous_last_error = std::move(health_.last_error);
          previous_latest = std::move(latest_);
          previous_last_valid = last_valid_;
          previous_session_started_at = session_started_at_;
          previous_generation = generation_;
          previous_published_generation = published_generation_;
          previous_receive_times = std::move(receive_times_);
          previous_publish_times = std::move(publish_times_);
          previous_receive_rate = health_.receive_rate;
          previous_publish_rate = health_.publish_rate;

          fault_code_.store(FaultCode::None, std::memory_order_release);
          health_.state = ClientState::Connecting;
          health_.fault_code = FaultCode::None;
          health_.last_error.clear();
          latest_.reset();
          last_valid_ = {};
          session_started_at_ = std::chrono::steady_clock::now();
          ++generation_;
          published_generation_ = 0;
          receive_times_.clear();
          publish_times_.clear();
          health_.receive_rate = 0;
          health_.publish_rate = 0;
        }
        stopping_ = false;
        worker_exited_ = false;
        callback_ = std::move(callback);
        try {
          worker_ = create_worker_thread();
        }
        catch (...) {
          stopping_.store(previous_stopping, std::memory_order_relaxed);
          worker_exited_.store(previous_worker_exited, std::memory_order_relaxed);
          fault_code_.store(previous_fault_code, std::memory_order_relaxed);
          callback_ = std::move(previous_callback);
          {
            std::lock_guard<std::mutex> record_lock(record_mutex_);
            last_publish_ = previous_last_publish;
          }
          {
            std::lock_guard<std::mutex> health_lock(health_mutex_);
            health_.state = previous_state;
            health_.fault_code = previous_health_fault_code;
            health_.last_error = std::move(previous_last_error);
            latest_ = std::move(previous_latest);
            last_valid_ = previous_last_valid;
            session_started_at_ = previous_session_started_at;
            generation_ = previous_generation;
            published_generation_ = previous_published_generation;
            receive_times_ = std::move(previous_receive_times);
            publish_times_ = std::move(previous_publish_times);
            health_.receive_rate = previous_receive_rate;
            health_.publish_rate = previous_publish_rate;
          }
          first_sample_cv_.notify_all();
          throw;
        }
        active_worker_id_ = worker_.get_id();
        return;
      }
    }
    reaped_worker.join();
    { std::lock_guard<std::mutex> lock(lifecycle_mutex_); active_worker_id_ = {}; joining_ = false; }
    lifecycle_cv_.notify_all();
  }
}
std::thread NetFTClient::create_worker_thread()
{
  if (thread_factory_) return thread_factory_(this);
  return std::thread(&NetFTClient::run, this);
}
void NetFTClient::stop() noexcept
{
  std::thread joining_worker;
  {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    stopping_ = true;
    first_sample_cv_.notify_all();
    { std::lock_guard<std::mutex> lock(socket_mutex_);
      if (socket_ >= 0) {
        if (session_started_) { const auto bytes = encode_request(Command::StopStreaming); (void)::send(socket_, bytes.data(), bytes.size(), 0); session_started_ = false; }
        ::shutdown(socket_, SHUT_RDWR);
      }
    }
    if (!worker_exited_.load(std::memory_order_acquire) && active_worker_id_ == std::this_thread::get_id()) return;
    if (joining_) { lifecycle_cv_.wait(lifecycle_lock, [&] { return !joining_; }); return; }
    if (!worker_.joinable()) { set_state(ClientState::Stopped); return; }
    joining_ = true;
    joining_worker = std::move(worker_);
  }
  joining_worker.join();
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    set_state(ClientState::Stopped);
    active_worker_id_ = {};
    joining_ = false;
  }
  lifecycle_cv_.notify_all();
}
void NetFTClient::bias()
{
  { std::lock_guard<std::mutex> lock(health_mutex_); if (health_.state != ClientState::Streaming) throw NotConnectedError{"client is not streaming"}; }
  std::lock_guard<std::mutex> record_lock(record_mutex_);
  send_command(static_cast<std::uint16_t>(Command::SetSoftwareBias));
  send_command(static_cast<std::uint16_t>(Command::StartRealtime));
  rdt_->reset();
}
bool NetFTClient::wait_for_first_sample(std::chrono::duration<double> timeout)
{
  std::unique_lock<std::mutex> lock(health_mutex_);
  const auto captured_generation = generation_;
  if (captured_generation == 0) return false;
  first_sample_cv_.wait_for(lock, timeout, [&] { return generation_ != captured_generation || published_generation_ == captured_generation || faulted() || stopping_; });
  if (generation_ != captured_generation) return false;
  return published_generation_ == captured_generation;
}
HealthSnapshot NetFTClient::health_snapshot() const
{
  std::lock_guard<std::mutex> lock(health_mutex_); const auto now = std::chrono::steady_clock::now();
  const auto prune = [&](auto & times) { while (!times.empty() && now - times.front() > std::chrono::seconds{1}) times.pop_front(); };
  prune(receive_times_); prune(publish_times_);
  auto snapshot = health_; snapshot.receive_rate = static_cast<double>(receive_times_.size()); snapshot.publish_rate = static_cast<double>(publish_times_.size()); snapshot.fault_code = fault_code();
  if (snapshot.fault_code != FaultCode::None) snapshot.state = ClientState::Faulted;
  if (last_valid_.time_since_epoch().count() != 0) snapshot.last_record_age = now - last_valid_;
  return snapshot;
}
std::optional<WrenchSample> NetFTClient::latest_sample() const { std::lock_guard<std::mutex> lock(health_mutex_); return latest_; }
void NetFTClient::set_state(ClientState state) { std::lock_guard<std::mutex> lock(health_mutex_); health_.state = state; }
void NetFTClient::set_error(const char * message) { std::lock_guard<std::mutex> lock(health_mutex_); health_.last_error = message; }
void NetFTClient::record_callback_error(const char * message) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(health_mutex_);
    ++health_.callback_error_count;
    try { health_.last_error = message; }
    catch (...) {}
  }
  catch (...) {}
}
void NetFTClient::latch_fault(FaultCode code, const char * message) noexcept
{
  FaultCode expected = FaultCode::None;
  const bool first_fault = fault_code_.compare_exchange_strong(expected, code, std::memory_order_acq_rel);
  try {
    std::lock_guard<std::mutex> lock(health_mutex_);
    health_.state = ClientState::Faulted;
    if (first_fault) {
      try { health_.last_error = message; }
      catch (...) {}
    }
  }
  catch (...) {}
  first_sample_cv_.notify_all();
}
void NetFTClient::send_command(std::uint16_t command)
{
  const auto bytes = encode_request(static_cast<Command>(command));
  std::lock_guard<std::mutex> lock(socket_mutex_);
  if (socket_ < 0 || ::send(socket_, bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size())) throw NotConnectedError{"client is not streaming"};
}
void NetFTClient::close_session() noexcept
{
  std::lock_guard<std::mutex> lock(socket_mutex_);
  if (socket_ < 0) return;
  if (session_started_) { const auto bytes = encode_request(Command::StopStreaming); (void)::send(socket_, bytes.data(), bytes.size(), 0); session_started_ = false; }
  ::close(socket_); socket_ = -1;
}
void NetFTClient::run()
{
  auto backoff = config_.reconnect_initial_delay;
  while (!stopping_ && !faulted()) {
    const auto result = receive_session();
    close_session();
    if (result != SessionResult::Stopped && config_.recovery_policy == RecoveryPolicy::FailStop) { latch_fault(fault_for(result), message_for(result)); break; }
    if (result == SessionResult::Stopped || stopping_) break;
    if (recovered_.exchange(false, std::memory_order_acq_rel)) backoff = config_.reconnect_initial_delay;
    set_error(message_for(result)); set_state(ClientState::Backoff);
    { std::lock_guard<std::mutex> lock(health_mutex_); ++health_.reconnect_count; }
    const auto until = std::chrono::steady_clock::now() + backoff;
    while (!stopping_ && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    backoff = std::min(backoff * 2, config_.reconnect_max_delay);
  }
  close_session();
  worker_exited_.store(true, std::memory_order_release);
  if (!faulted()) set_state(ClientState::Stopped);
  lifecycle_cv_.notify_all();
}
NetFTClient::SessionResult NetFTClient::receive_session()
{
  set_state(ClientState::Connecting);
  if (stopping_) return SessionResult::Stopped;
  if (!endpoint_ready_) return stopping_ ? SessionResult::Stopped : SessionResult::Socket;
  const int fd = ::socket(endpoint_.ss_family, SOCK_DGRAM, 0);
  if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr *>(&endpoint_), endpoint_size_) != 0) { if (fd >= 0) ::close(fd); return stopping_ ? SessionResult::Stopped : SessionResult::Socket; }
  { std::lock_guard<std::mutex> lock(socket_mutex_); socket_ = fd; session_started_ = true; }
  try { send_command(static_cast<std::uint16_t>(Command::StartRealtime)); } catch (...) { return stopping_ ? SessionResult::Stopped : SessionResult::Socket; }
  { std::lock_guard<std::mutex> record_lock(record_mutex_); rdt_->reset(); ft_->begin_session(); }
  const auto deadline_duration = config_.receive_timeout;
  auto deadline = std::chrono::steady_clock::now() + deadline_duration;
  std::array<std::uint8_t, 512> bytes{};
  while (!stopping_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      std::lock_guard<std::mutex> lock(health_mutex_);
      if (stopping_) return SessionResult::Stopped;
      ++health_.timeout_count;
      return SessionResult::Timeout;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd pollfd{fd, POLLIN, 0}; const int wait = std::max(1, std::min(50, static_cast<int>(remaining.count())));
    const int polled = ::poll(&pollfd, 1, wait);
    if (polled < 0) { if (errno == EINTR) continue; return stopping_ ? SessionResult::Stopped : SessionResult::Socket; }
    if (polled == 0) continue;
    if (stopping_) return SessionResult::Stopped;
    if ((pollfd.revents & POLLIN) == 0) return stopping_ ? SessionResult::Stopped : SessionResult::Socket;
    const auto size = ::recv(fd, bytes.data(), bytes.size(), 0);
    if (size < 0) return stopping_ ? SessionResult::Stopped : SessionResult::Socket;
    if (stopping_) return SessionResult::Stopped;
    const auto received = std::chrono::steady_clock::now();
    RawRecord record{};
    try { record = decode_record(bytes.data(), static_cast<std::size_t>(size)); }
    catch (const ProtocolError &) { std::lock_guard<std::mutex> lock(health_mutex_); ++health_.malformed_count; if (++consecutive_malformed_ >= kMalformedStormThreshold) return SessionResult::MalformedStorm; continue; }
    consecutive_malformed_ = 0; recovered_.store(true, std::memory_order_release); deadline = received + deadline_duration;
    if (const auto outcome = handle_record(record, received)) return *outcome;
  }
  return SessionResult::Stopped;
}
std::optional<NetFTClient::SessionResult> NetFTClient::handle_record(const RawRecord & record, std::chrono::steady_clock::time_point received)
{
  std::unique_lock<std::mutex> record_lock(record_mutex_);
  const auto rdt_observation = rdt_->observe(record.rdt_sequence); const auto ft_observation = ft_->observe(record.ft_sequence);
  const auto severity = classify_status(record.status);
  { std::lock_guard<std::mutex> lock(health_mutex_); ++health_.received_count; last_valid_ = received; receive_times_.push_back(received); while (!receive_times_.empty() && received - receive_times_.front() > std::chrono::seconds{1}) receive_times_.pop_front(); health_.receive_rate = static_cast<double>(receive_times_.size()); health_.last_rdt_sequence = record.rdt_sequence; health_.last_ft_sequence = record.ft_sequence; health_.last_status = record.status;
    if (rdt_observation.kind == SequenceKind::Gap) health_.lost_count += rdt_observation.gap;
    if (rdt_observation.kind == SequenceKind::Duplicate) ++health_.duplicate_count;
    if (rdt_observation.kind == SequenceKind::OutOfOrder) ++health_.out_of_order_count;
    if (severity == DiagnosticSeverity::Warn) ++health_.warning_count;
    if (severity == DiagnosticSeverity::Error) ++health_.device_error_count;
    if (ft_observation.kind == FtSequenceKind::Stall) { ++health_.ft_stall_count; health_.last_ft_progress = "stall"; }
    else if (ft_observation.kind == FtSequenceKind::Backward) { ++health_.ft_backward_count; health_.last_ft_progress = "backward"; }
    else if (ft_observation.kind == FtSequenceKind::Restart) { ++health_.ft_restart_count; health_.last_ft_progress = "restart"; }
    else health_.last_ft_progress = ft_observation.kind == FtSequenceKind::First ? "first" : "forward";
  }
  std::optional<SessionResult> outcome;
  if (severity == DiagnosticSeverity::Error) outcome = SessionResult::SeriousStatus;
  else if (ft_observation.kind == FtSequenceKind::Stall) outcome = SessionResult::FtStall;
  else if (ft_observation.kind == FtSequenceKind::Backward) outcome = SessionResult::FtBackward;
  const bool drop_reconnect_ft_discontinuity =
    (ft_observation.kind == FtSequenceKind::Stall || ft_observation.kind == FtSequenceKind::Backward) &&
    config_.recovery_policy == RecoveryPolicy::Reconnect;
  if (drop_reconnect_ft_discontinuity &&
      (outcome == SessionResult::FtStall || outcome == SessionResult::FtBackward)) outcome.reset();
  const bool ordered = rdt_observation.kind != SequenceKind::Duplicate && rdt_observation.kind != SequenceKind::OutOfOrder;
  set_state(ClientState::Streaming);
  const bool eligible = ordered && !drop_reconnect_ft_discontinuity && (!outcome || (outcome == SessionResult::SeriousStatus && config_.publish_on_error));
  if (!eligible) return outcome;
  WrenchSample sample{{record.rdt_sequence}, {record.ft_sequence}, {record.status}, {record.fx / config_.counts_per_force, record.fy / config_.counts_per_force, record.fz / config_.counts_per_force}, {record.tx / config_.counts_per_torque, record.ty / config_.counts_per_torque, record.tz / config_.counts_per_torque}, received};
  bool publish = true;
  if (config_.publish_rate > 0 && last_publish_.time_since_epoch().count() != 0 && received - last_publish_ < std::chrono::duration<double>{1.0 / config_.publish_rate}) publish = false;
  if (!publish) { std::lock_guard<std::mutex> lock(health_mutex_); ++health_.rate_dropped_count; return outcome; }
  record_lock.unlock();
  const auto callback_failure_outcome = [&] {
    if (outcome || config_.recovery_policy == RecoveryPolicy::Reconnect) return outcome;
    return std::optional<SessionResult>{SessionResult::Callback};
  };
  try {
    SampleCallback callback = callback_;
    if (callback) callback(sample);
  }
  catch (const std::exception & error) {
    record_callback_error(error.what());
    return callback_failure_outcome();
  }
  catch (...) {
    record_callback_error("sample callback threw");
    return callback_failure_outcome();
  }
  { std::lock_guard<std::mutex> lock(record_mutex_); last_publish_ = received; }
  { std::lock_guard<std::mutex> lock(health_mutex_); latest_ = sample; ++health_.published_count; publish_times_.push_back(received); while (!publish_times_.empty() && received - publish_times_.front() > std::chrono::seconds{1}) publish_times_.pop_front(); health_.publish_rate = static_cast<double>(publish_times_.size()); published_generation_ = generation_; }
  first_sample_cv_.notify_all();
  return outcome;
}
}  // namespace netft_driver
