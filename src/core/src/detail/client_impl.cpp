#include "detail/client_impl.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

#include "detail/protocol.hpp"
#include "netft/discovery.hpp"
#include "netft/status.hpp"

namespace netft {
namespace {

constexpr std::uint64_t kMalformedStormThreshold = 10;

} // namespace

FaultCode Client::Impl::fault_for(const SessionResult result) noexcept {
  switch (result) {
  case Client::Impl::SessionResult::SensorConfiguration:
    return FaultCode::SensorConfiguration;
  case Client::Impl::SessionResult::Timeout:
    return FaultCode::Timeout;
  case Client::Impl::SessionResult::Socket:
    return FaultCode::Socket;
  case Client::Impl::SessionResult::SeriousStatus:
    return FaultCode::SeriousStatus;
  case Client::Impl::SessionResult::FtStall:
    return FaultCode::FtStall;
  case Client::Impl::SessionResult::FtBackward:
    return FaultCode::FtBackward;
  case Client::Impl::SessionResult::MalformedStorm:
    return FaultCode::MalformedStorm;
  case Client::Impl::SessionResult::Callback:
    return FaultCode::Callback;
  case Client::Impl::SessionResult::Stopped:
    return FaultCode::None;
  }
  return FaultCode::Socket;
}

const char *Client::Impl::message_for(const SessionResult result) noexcept {
  switch (result) {
  case Client::Impl::SessionResult::SensorConfiguration:
    return "sensor configuration discovery failed";
  case Client::Impl::SessionResult::Timeout:
    return "no valid RDT record before timeout";
  case Client::Impl::SessionResult::Socket:
    return "UDP socket failure";
  case Client::Impl::SessionResult::SeriousStatus:
    return "serious device status";
  case Client::Impl::SessionResult::FtStall:
    return "FT sequence stalled";
  case Client::Impl::SessionResult::FtBackward:
    return "FT sequence moved backward";
  case Client::Impl::SessionResult::MalformedStorm:
    return "malformed packet storm";
  case Client::Impl::SessionResult::Callback:
    return "sample callback failed";
  case Client::Impl::SessionResult::Stopped:
    return "";
  }
  return "UDP socket failure";
}

namespace {

bool same_calibration(const Calibration &left, const Calibration &right) {
  return left.counts_per_force_unit == right.counts_per_force_unit &&
         left.counts_per_torque_unit == right.counts_per_torque_unit &&
         left.force_unit == right.force_unit && left.torque_unit == right.torque_unit;
}

void prune_rate_window(std::deque<std::chrono::steady_clock::time_point> &times,
                       const std::chrono::steady_clock::time_point now) {
  while (!times.empty() && now - times.front() > std::chrono::seconds{1}) {
    times.pop_front();
  }
}

std::string ft_progress_name(const detail::FtSequenceKind kind) {
  switch (kind) {
  case detail::FtSequenceKind::First:
    return "first";
  case detail::FtSequenceKind::Forward:
    return "forward";
  case detail::FtSequenceKind::Stall:
    return "stall";
  case detail::FtSequenceKind::Backward:
    return "backward";
  case detail::FtSequenceKind::Restart:
    return "restart";
  }
  return "unknown";
}

} // namespace

Client::Impl::Impl(Config config) : config_(std::move(config)) {
  validate(config_);
  health_.sensor_host = config_.sensor_host;
  health_.rdt_port = config_.rdt_port;
}

Client::Impl::~Impl() { stop(); }

void Client::Impl::start(SampleCallback callback) {
  for (;;) {
    std::thread reaped_worker;
    {
      std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
      if (!worker_exited_.load(std::memory_order_acquire) &&
          active_worker_id_ == std::this_thread::get_id()) {
        throw std::logic_error("client is already running");
      }
      lifecycle_cv_.wait(lifecycle_lock, [this] { return !joining_; });
      if (worker_.joinable()) {
        if (!worker_exited_.load(std::memory_order_acquire)) {
          if (!faulted()) {
            throw std::logic_error("client is already running");
          }
          lifecycle_cv_.wait(lifecycle_lock, [this] {
            return worker_exited_.load(std::memory_order_acquire) || !faulted();
          });
          continue;
        }
        joining_ = true;
        reaped_worker = std::move(worker_);
      } else {
        const bool previous_stopping = stopping_.load();
        const bool previous_worker_exited = worker_exited_.load();
        const auto previous_fault = fault_code();
        auto previous_callback = std::move(callback_);
        std::chrono::steady_clock::time_point previous_last_delivery;
        {
          std::scoped_lock record_lock(record_mutex_);
          previous_last_delivery = last_delivery_at_;
          last_delivery_at_ = {};
        }

        HealthSnapshot previous_health;
        std::optional<Sample> previous_latest;
        std::chrono::steady_clock::time_point previous_last_record;
        std::uint64_t previous_generation{};
        std::uint64_t previous_delivered_generation{};
        std::deque<std::chrono::steady_clock::time_point> previous_receive_times;
        std::deque<std::chrono::steady_clock::time_point> previous_delivery_times;
        std::uint64_t previous_consecutive_malformed{};
        {
          std::scoped_lock data_lock(data_mutex_);
          previous_health = health_;
          previous_latest = latest_;
          previous_last_record = last_record_at_;
          previous_generation = generation_;
          previous_delivered_generation = delivered_generation_;
          previous_receive_times = receive_times_;
          previous_delivery_times = delivery_times_;
          previous_consecutive_malformed = consecutive_malformed_;

          fault_latch_.reset();
          health_.state = ClientState::Connecting;
          health_.fault_code = FaultCode::None;
          health_.last_error.clear();
          health_.receive_rate_hz = 0.0;
          health_.delivery_rate_hz = 0.0;
          latest_.reset();
          last_record_at_ = {};
          ++generation_;
          delivered_generation_ = 0;
          receive_times_.clear();
          delivery_times_.clear();
          consecutive_malformed_ = 0;
        }
        stopping_ = false;
        worker_exited_ = false;
        callback_ = std::move(callback);
        try {
          worker_ = create_worker_thread();
        } catch (...) {
          stopping_ = previous_stopping;
          worker_exited_ = previous_worker_exited;
          callback_ = std::move(previous_callback);
          {
            std::scoped_lock record_lock(record_mutex_);
            last_delivery_at_ = previous_last_delivery;
          }
          {
            std::scoped_lock data_lock(data_mutex_);
            health_ = previous_health;
            latest_ = previous_latest;
            last_record_at_ = previous_last_record;
            generation_ = previous_generation;
            delivered_generation_ = previous_delivered_generation;
            receive_times_ = std::move(previous_receive_times);
            delivery_times_ = std::move(previous_delivery_times);
            consecutive_malformed_ = previous_consecutive_malformed;
          }
          fault_latch_.reset();
          if (previous_fault != FaultCode::None) {
            static_cast<void>(fault_latch_.publish(previous_fault, previous_health.last_error,
                                                   data_mutex_, health_));
          }
          first_sample_cv_.notify_all();
          lifecycle_cv_.notify_all();
          throw;
        }
        active_worker_id_ = worker_.get_id();
        first_sample_cv_.notify_all();
        lifecycle_cv_.notify_all();
        return;
      }
    }

    reaped_worker.join();
    {
      std::scoped_lock lifecycle_lock(lifecycle_mutex_);
      active_worker_id_ = {};
      joining_ = false;
    }
    lifecycle_cv_.notify_all();
  }
}

std::thread Client::Impl::create_worker_thread() {
  if (thread_factory_ != nullptr) {
    return thread_factory_(this);
  }
  return std::thread([this] { run(); });
}

void Client::Impl::stop() noexcept {
  for (;;) {
    std::thread joining_worker;
    {
      std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
      stopping_ = true;
      first_sample_cv_.notify_all();
      lifecycle_cv_.notify_all();
      {
        std::scoped_lock command_lock(command_mutex_);
        if (session_started_) {
          try {
            transport_.send(detail::encode_request(detail::Command::StopStreaming));
          } catch (...) {
            static_cast<void>(0);
          }
          session_started_ = false;
        }
        transport_.shutdown();
      }

      if (!worker_exited_.load(std::memory_order_acquire) &&
          active_worker_id_ == std::this_thread::get_id()) {
        return;
      }
      if (joining_) {
        lifecycle_cv_.wait(lifecycle_lock, [this] { return !joining_; });
        continue;
      }
      if (!worker_.joinable()) {
        if (!faulted()) {
          std::scoped_lock data_lock(data_mutex_);
          health_.state = ClientState::Stopped;
        }
        return;
      }
      joining_ = true;
      joining_worker = std::move(worker_);
    }

    joining_worker.join();
    {
      std::scoped_lock lifecycle_lock(lifecycle_mutex_);
      if (!faulted()) {
        std::scoped_lock data_lock(data_mutex_);
        health_.state = ClientState::Stopped;
      }
      active_worker_id_ = {};
      joining_ = false;
    }
    lifecycle_cv_.notify_all();
    first_sample_cv_.notify_all();
    return;
  }
}

void Client::Impl::bias() {
  std::scoped_lock command_lock(command_mutex_);
  std::scoped_lock record_lock(record_mutex_);
  {
    std::scoped_lock data_lock(data_mutex_);
    if (stopping_ || !session_started_ || faulted() || health_.state != ClientState::Streaming) {
      throw NotConnectedError("client is not streaming");
    }
  }
  transport_.send(detail::encode_request(detail::Command::SetSoftwareBias));
  transport_.send(detail::encode_request(detail::Command::StartRealtime));
  rdt_sequence_.reset();
}

bool Client::Impl::wait_for_first_sample(const std::chrono::duration<double> timeout) {
  std::unique_lock<std::mutex> data_lock(data_mutex_);
  const auto captured_generation = generation_;
  if (captured_generation == 0) {
    return false;
  }
  first_sample_cv_.wait_for(data_lock, timeout, [this, captured_generation] {
    return generation_ != captured_generation || delivered_generation_ == captured_generation ||
           stopping_ || faulted();
  });
  if (const auto hook = wait_wake_test_hook_) {
    data_lock.unlock();
    hook(this, captured_generation);
    data_lock.lock();
  }
  if (generation_ != captured_generation) {
    return false;
  }
  return delivered_generation_ == captured_generation;
}

bool Client::Impl::faulted() const noexcept { return fault_latch_.faulted(); }

FaultCode Client::Impl::fault_code() const noexcept { return fault_latch_.code(); }

HealthSnapshot Client::Impl::health() const {
  std::scoped_lock data_lock(data_mutex_);
  const auto now = std::chrono::steady_clock::now();
  prune_rate_window(receive_times_, now);
  prune_rate_window(delivery_times_, now);
  auto snapshot = health_;
  snapshot.fault_code = fault_code();
  if (snapshot.fault_code != FaultCode::None) {
    snapshot.state = ClientState::Faulted;
  }
  snapshot.receive_rate_hz = static_cast<double>(receive_times_.size());
  snapshot.delivery_rate_hz = static_cast<double>(delivery_times_.size());
  if (last_record_at_ != std::chrono::steady_clock::time_point{}) {
    snapshot.last_record_age = now - last_record_at_;
  }
  return snapshot;
}

std::optional<Sample> Client::Impl::latest_sample() const {
  std::scoped_lock data_lock(data_mutex_);
  return latest_;
}

SensorConfiguration Client::Impl::configuration_for_session() {
  if (config_.calibration_override) {
    return SensorConfiguration{"", *config_.calibration_override, CalibrationSource::Override, 1};
  }
  DiscoveryOptions options;
  options.sensor_host = config_.sensor_host;
  options.http_port = config_.http_port;
  options.connect_timeout = config_.configuration_connect_timeout;
  options.total_timeout = config_.configuration_timeout;
  return discover_sensor(options);
}

void Client::Impl::apply_configuration(SensorConfiguration configuration) {
  std::scoped_lock data_lock(data_mutex_);
  if (health_.sensor_configuration) {
    const auto &previous = *health_.sensor_configuration;
    if (same_calibration(previous.calibration, configuration.calibration)) {
      configuration.revision = previous.revision;
    } else {
      configuration.revision = previous.revision + 1;
      ++health_.calibration_change_count;
    }
  } else {
    configuration.revision = 1;
  }
  health_.sensor_configuration = std::move(configuration);
}

void Client::Impl::run() noexcept {
  auto backoff = config_.reconnect_initial_delay;
  try {
    while (!stopping_ && !faulted()) {
      auto outcome = receive_session();
      close_session();

      if (outcome.message.empty()) {
        outcome.message = message_for(outcome.result);
      }
      if (outcome.result != SessionResult::Stopped &&
          config_.recovery_policy == RecoveryPolicy::FailStop) {
        set_fault(fault_for(outcome.result), std::move(outcome.message));
        break;
      }
      if (outcome.result == SessionResult::Stopped || stopping_) {
        break;
      }

      if (outcome.received_valid_record) {
        backoff = config_.reconnect_initial_delay;
      }
      {
        std::scoped_lock data_lock(data_mutex_);
        health_.last_error = std::move(outcome.message);
        health_.state = ClientState::Backoff;
        ++health_.reconnect_count;
      }

      std::unique_lock<std::mutex> data_lock(data_mutex_);
      first_sample_cv_.wait_for(data_lock, backoff, [this] { return stopping_.load(); });
      data_lock.unlock();
      backoff = std::min(backoff * 2.0, config_.reconnect_max_delay);
    }
  } catch (const std::exception &error) {
    if (!stopping_) {
      set_fault(FaultCode::Socket, error.what());
    }
  } catch (...) {
    if (!stopping_) {
      set_fault(FaultCode::Socket, "unknown client failure");
    }
  }
  finish_session();
}

Client::Impl::SessionOutcome Client::Impl::receive_session() {
  bool received_valid_record = false;
  {
    std::scoped_lock data_lock(data_mutex_);
    health_.state = ClientState::Connecting;
  }
  if (stopping_) {
    return {SessionResult::Stopped, {}, false};
  }

  try {
    apply_configuration(configuration_for_session());
  } catch (const DiscoveryError &error) {
    return {stopping_ ? SessionResult::Stopped : SessionResult::SensorConfiguration, error.what(),
            false};
  }
  if (stopping_) {
    return {SessionResult::Stopped, {}, false};
  }

  try {
    transport_.connect(config_.sensor_host, config_.rdt_port);
    std::scoped_lock command_lock(command_mutex_);
    if (stopping_) {
      return {SessionResult::Stopped, {}, false};
    }
    transport_.send(detail::encode_request(detail::Command::StartRealtime));
    session_started_ = true;
  } catch (const std::exception &error) {
    return {stopping_ ? SessionResult::Stopped : SessionResult::Socket, error.what(), false};
  }

  {
    std::scoped_lock record_lock(record_mutex_);
    rdt_sequence_.reset();
    ft_sequence_.begin_session();
  }

  const auto timeout = config_.receive_timeout;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 65'536> buffer{};
  while (!stopping_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      std::scoped_lock data_lock(data_mutex_);
      if (stopping_) {
        return {SessionResult::Stopped, {}, received_valid_record};
      }
      ++health_.timeout_count;
      return {SessionResult::Timeout, {}, received_valid_record};
    }

    std::size_t size{};
    try {
      size = transport_.receive(buffer.data(), buffer.size(), deadline - now);
    } catch (const std::exception &error) {
      return {stopping_ ? SessionResult::Stopped : SessionResult::Socket, error.what(),
              received_valid_record};
    }
    if (stopping_) {
      return {SessionResult::Stopped, {}, received_valid_record};
    }
    if (size == 0) {
      std::scoped_lock data_lock(data_mutex_);
      if (stopping_) {
        return {SessionResult::Stopped, {}, received_valid_record};
      }
      ++health_.timeout_count;
      return {SessionResult::Timeout, {}, received_valid_record};
    }

    detail::RawRecord record;
    try {
      record = detail::decode_record(buffer.data(), size);
    } catch (const detail::ProtocolError &) {
      std::scoped_lock data_lock(data_mutex_);
      ++health_.malformed_count;
      if (++consecutive_malformed_ >= kMalformedStormThreshold) {
        return {SessionResult::MalformedStorm, {}, received_valid_record};
      }
      continue;
    }

    const auto received_at = std::chrono::steady_clock::now();
    {
      std::scoped_lock data_lock(data_mutex_);
      consecutive_malformed_ = 0;
    }
    received_valid_record = true;
    deadline = received_at + timeout;
    if (const auto outcome = handle_record(record, received_at)) {
      return {*outcome, {}, received_valid_record};
    }
  }
  return {SessionResult::Stopped, {}, received_valid_record};
}

std::optional<Client::Impl::SessionResult>
Client::Impl::handle_record(const detail::RawRecord &record,
                            const std::chrono::steady_clock::time_point received_at) {
  SensorConfiguration configuration;
  bool deliver = true;
  std::optional<SessionResult> outcome;
  {
    std::unique_lock<std::mutex> record_lock(record_mutex_);
    const auto rdt = rdt_sequence_.observe(record.rdt_sequence);
    const auto ft = ft_sequence_.observe(record.ft_sequence);
    const auto severity = classify_status(record.status);

    {
      std::scoped_lock data_lock(data_mutex_);
      if (!health_.sensor_configuration) {
        throw std::logic_error("sensor configuration is unavailable while streaming");
      }
      configuration = *health_.sensor_configuration;
      health_.state = ClientState::Streaming;
      ++health_.received_count;
      receive_times_.push_back(received_at);
      prune_rate_window(receive_times_, received_at);
      health_.receive_rate_hz = static_cast<double>(receive_times_.size());
      health_.last_rdt_sequence = record.rdt_sequence;
      health_.last_ft_sequence = record.ft_sequence;
      health_.last_status = record.status;
      last_record_at_ = received_at;

      switch (rdt.kind) {
      case detail::SequenceKind::Gap:
        health_.lost_count += rdt.gap;
        break;
      case detail::SequenceKind::Duplicate:
        ++health_.duplicate_count;
        deliver = false;
        break;
      case detail::SequenceKind::OutOfOrder:
        ++health_.out_of_order_count;
        deliver = false;
        break;
      case detail::SequenceKind::First:
      case detail::SequenceKind::Contiguous:
        break;
      }

      health_.last_ft_progress = ft_progress_name(ft.kind);
      switch (ft.kind) {
      case detail::FtSequenceKind::Stall:
        ++health_.ft_stall_count;
        break;
      case detail::FtSequenceKind::Backward:
        ++health_.ft_backward_count;
        break;
      case detail::FtSequenceKind::Restart:
        ++health_.ft_restart_count;
        break;
      case detail::FtSequenceKind::First:
      case detail::FtSequenceKind::Forward:
        break;
      }

      if (severity == StatusSeverity::Warn) {
        ++health_.warning_count;
      } else if (severity == StatusSeverity::Error) {
        ++health_.device_error_count;
        deliver = config_.deliver_samples_with_error_status && deliver;
        outcome = SessionResult::SeriousStatus;
      }

      if (!outcome && ft.kind == detail::FtSequenceKind::Stall) {
        outcome = SessionResult::FtStall;
      } else if (!outcome && ft.kind == detail::FtSequenceKind::Backward) {
        outcome = SessionResult::FtBackward;
      }

      const bool drop_reconnect_ft_discontinuity =
          config_.recovery_policy == RecoveryPolicy::Reconnect &&
          (ft.kind == detail::FtSequenceKind::Stall || ft.kind == detail::FtSequenceKind::Backward);
      if (drop_reconnect_ft_discontinuity) {
        deliver = false;
        if (outcome == SessionResult::FtStall || outcome == SessionResult::FtBackward) {
          outcome.reset();
        }
      }

      if (deliver && config_.sample_rate_limit_hz > 0.0 &&
          last_delivery_at_ != std::chrono::steady_clock::time_point{} &&
          received_at - last_delivery_at_ <
              std::chrono::duration<double>{1.0 / config_.sample_rate_limit_hz}) {
        deliver = false;
        ++health_.rate_limited_count;
      }
    }
    record_lock.unlock();
  }

  if (!deliver) {
    return outcome;
  }

  const auto &calibration = configuration.calibration;
  Sample sample;
  sample.rdt_sequence = record.rdt_sequence;
  sample.ft_sequence = record.ft_sequence;
  sample.status = record.status;
  sample.force = {record.fx / calibration.counts_per_force_unit,
                  record.fy / calibration.counts_per_force_unit,
                  record.fz / calibration.counts_per_force_unit};
  sample.torque = {record.tx / calibration.counts_per_torque_unit,
                   record.ty / calibration.counts_per_torque_unit,
                   record.tz / calibration.counts_per_torque_unit};
  sample.force_unit = calibration.force_unit;
  sample.torque_unit = calibration.torque_unit;
  sample.configuration_revision = configuration.revision;
  sample.received_at = received_at;

  const auto callback_failure_outcome = [&] {
    if (outcome || config_.recovery_policy == RecoveryPolicy::Reconnect) {
      return outcome;
    }
    return std::optional<SessionResult>{SessionResult::Callback};
  };

  try {
    SampleCallback callback = callback_;
    if (callback) {
      callback(sample);
    }
  } catch (const std::exception &error) {
    record_callback_error(error.what());
    return callback_failure_outcome();
  } catch (...) {
    record_callback_error("sample callback failed");
    return callback_failure_outcome();
  }

  {
    std::scoped_lock record_lock(record_mutex_);
    last_delivery_at_ = received_at;
    std::scoped_lock data_lock(data_mutex_);
    latest_ = sample;
    ++health_.delivered_count;
    delivery_times_.push_back(received_at);
    prune_rate_window(delivery_times_, received_at);
    health_.delivery_rate_hz = static_cast<double>(delivery_times_.size());
    delivered_generation_ = generation_;
  }
  first_sample_cv_.notify_all();
  lifecycle_cv_.notify_all();
  return outcome;
}

void Client::Impl::set_fault(FaultCode code, std::string message) noexcept {
  const bool published = fault_latch_.publish(code, std::move(message), data_mutex_, health_);
  first_sample_cv_.notify_all();
  lifecycle_cv_.notify_all();
  if (published) {
    if (const auto hook = fault_published_test_hook_) {
      hook(this);
    }
  }
}

void Client::Impl::record_callback_error(const char *message) noexcept {
  try {
    std::scoped_lock data_lock(data_mutex_);
    ++health_.callback_error_count;
    try {
      health_.last_error = message;
    } catch (...) {
      static_cast<void>(0);
    }
  } catch (...) {
    static_cast<void>(0);
  }
}

void Client::Impl::close_session() noexcept {
  std::scoped_lock command_lock(command_mutex_);
  if (session_started_) {
    try {
      transport_.send(detail::encode_request(detail::Command::StopStreaming));
    } catch (...) {
      static_cast<void>(0);
    }
    session_started_ = false;
  }
  transport_.close();
}

void Client::Impl::finish_session() noexcept {
  close_session();
  worker_exited_.store(true, std::memory_order_release);
  if (!faulted()) {
    try {
      std::scoped_lock data_lock(data_mutex_);
      health_.state = ClientState::Stopped;
    } catch (...) {
      static_cast<void>(0);
    }
  }
  first_sample_cv_.notify_all();
  lifecycle_cv_.notify_all();
}

} // namespace netft
