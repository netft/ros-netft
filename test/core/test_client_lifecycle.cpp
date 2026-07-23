#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "detail/protocol.hpp"
#define private public
#include "detail/client_impl.hpp"
#undef private
#include "support/fake_sensor.hpp"

namespace {

using namespace std::chrono_literals;

constexpr auto kChangedConfiguration = R"xml(
<netft><prodname>Fake Net F/T</prodname><cfgcpf>1000000</cfgcpf>
<cfgcpt>1000</cfgcpt><scfgfu>N</scfgfu><scfgtu>N-mm</scfgtu></netft>)xml";

netft::Config config_for(const netft::test::FakeSensor &sensor) {
  netft::Config config;
  config.sensor_host = sensor.host();
  config.rdt_port = sensor.rdt_port();
  config.http_port = sensor.http_port();
  config.receive_timeout = 120ms;
  config.configuration_connect_timeout = 100ms;
  config.configuration_timeout = 250ms;
  config.reconnect_initial_delay = 5ms;
  config.reconnect_max_delay = 20ms;
  return config;
}

template <typename Predicate>
bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 1s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

struct CopyThrowState {
  std::atomic<bool> throw_on_copy{};
  std::atomic<unsigned> copy_attempts{};
  std::atomic<unsigned> calls{};
};

struct CopyThrowingCallback {
  explicit CopyThrowingCallback(std::shared_ptr<CopyThrowState> state_in)
      : state{std::move(state_in)} {}

  CopyThrowingCallback(const CopyThrowingCallback &other) : state{other.state} {
    ++state->copy_attempts;
    if (state->throw_on_copy) {
      throw std::bad_alloc{};
    }
  }
  CopyThrowingCallback(CopyThrowingCallback &&) noexcept = default;

  void operator()(const netft::Sample &) const { ++state->calls; }

  std::shared_ptr<CopyThrowState> state;
};

struct HookGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered{};
  bool released{};
};

struct ClientLifecycleTestAccess {
  static void fail_next_thread_creation(netft::Client &client) {
    fail_next = true;
    client.impl_->thread_factory_ = &create_thread;
  }

  static bool worker_joinable(const netft::Client &client) {
    return client.impl_->worker_.joinable();
  }

  static bool worker_exited(const netft::Client &client) { return client.impl_->worker_exited_; }

  static bool has_callback(const netft::Client &client) {
    return static_cast<bool>(client.impl_->callback_);
  }

  static bool stopping(const netft::Client &client) { return client.impl_->stopping_; }

  static std::uint64_t generation(const netft::Client &client) {
    std::lock_guard<std::mutex> data_lock(client.impl_->data_mutex_);
    return client.impl_->generation_;
  }

  static void gate_fault_publication(netft::Client &client) {
    reset_gate(fault_gate);
    client.impl_->fault_published_test_hook_ = &on_fault_published;
  }

  static bool wait_for_fault_publication(const std::chrono::milliseconds timeout = 1s) {
    return wait_for_gate(fault_gate, timeout);
  }

  static void release_fault_publication() { release_gate(fault_gate); }

  static void gate_waiter_after_wake(netft::Client &client) {
    reset_gate(waiter_gate);
    client.impl_->wait_wake_test_hook_ = &on_wait_wake;
  }

  static bool wait_for_waiter_wake(const std::chrono::milliseconds timeout = 1s) {
    return wait_for_gate(waiter_gate, timeout);
  }

  static void release_waiter() { release_gate(waiter_gate); }

private:
  static std::thread create_thread(netft::Client::Impl *impl) {
    if (fail_next.exchange(false)) {
      throw std::system_error{std::make_error_code(std::errc::resource_unavailable_try_again),
                              "injected thread creation failure"};
    }
    return std::thread{&netft::Client::Impl::run, impl};
  }

  static void reset_gate(HookGate &gate) {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.entered = false;
    gate.released = false;
  }

  static bool wait_for_gate(HookGate &gate, const std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.cv.wait_for(lock, timeout, [&] { return gate.entered; });
  }

  static void release_gate(HookGate &gate) {
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.released = true;
    }
    gate.cv.notify_all();
  }

  static void enter_gate(HookGate &gate) noexcept {
    try {
      std::unique_lock<std::mutex> lock(gate.mutex);
      gate.entered = true;
      gate.cv.notify_all();
      static_cast<void>(gate.cv.wait_for(lock, 2s, [&] { return gate.released; }));
    } catch (...) {
    }
  }

  static void on_fault_published(netft::Client::Impl *) noexcept { enter_gate(fault_gate); }

  static void on_wait_wake(netft::Client::Impl *, std::uint64_t) noexcept {
    enter_gate(waiter_gate);
  }

  static inline std::atomic<bool> fail_next{};
  static inline HookGate fault_gate;
  static inline HookGate waiter_gate;
};

TEST(ClientLifecycle, RateLimitDropsIntermediateSamplesWithoutQueueing) {
  netft::test::FakeSensor sensor{1000.0};
  auto config = config_for(sensor);
  config.sample_rate_limit_hz = 50.0;
  netft::Client client{config};
  std::atomic<unsigned> delivered{};
  client.start([&](const netft::Sample &) { ++delivered; });

  ASSERT_TRUE(client.wait_for_first_sample(500ms));
  ASSERT_TRUE(wait_until([&] {
    const auto health = client.health();
    return health.delivered_count >= 3 && health.rate_limited_count > 0;
  }));
  client.stop();

  const auto health = client.health();
  EXPECT_GT(health.delivered_count, 0U);
  EXPECT_GT(health.received_count, health.delivered_count);
  EXPECT_GT(health.rate_limited_count, 0U);
  EXPECT_EQ(health.delivered_count, delivered.load());
  EXPECT_EQ(health.received_count, health.delivered_count + health.rate_limited_count);
  EXPECT_DOUBLE_EQ(health.delivery_rate_hz, static_cast<double>(health.delivered_count));
}

TEST(ClientLifecycle, CallbackExceptionsContinueUnderReconnectPolicy) {
  netft::test::FakeSensor sensor{200.0};
  netft::Client client{config_for(sensor)};
  std::atomic<unsigned> calls{};
  client.start([&](const netft::Sample &) {
    ++calls;
    throw std::runtime_error{"consumer failed"};
  });

  ASSERT_TRUE(wait_until([&] { return client.health().callback_error_count >= 3; }));
  EXPECT_FALSE(client.faulted());
  EXPECT_GE(calls.load(), 3U);
  EXPECT_EQ(client.health().delivered_count, 0U);
  client.stop();
}

TEST(ClientLifecycle, CallbackCopyExceptionsContinueUnderReconnectPolicy) {
  netft::test::FakeSensor sensor{200.0};
  sensor.pause();
  netft::Client client{config_for(sensor)};
  auto state = std::make_shared<CopyThrowState>();
  netft::Client::SampleCallback callback{CopyThrowingCallback{state}};
  client.start(std::move(callback));
  ASSERT_EQ(state->copy_attempts.load(), 0U);

  state->throw_on_copy = true;
  sensor.resume();
  ASSERT_TRUE(wait_until([&] { return client.health().callback_error_count >= 3; }));
  EXPECT_EQ(state->calls.load(), 0U);
  EXPECT_FALSE(client.faulted());

  state->throw_on_copy = false;
  ASSERT_TRUE(wait_until([&] { return state->calls.load() > 0; }));
  EXPECT_TRUE(client.wait_for_first_sample(100ms));
  client.stop();
}

TEST(ClientLifecycle, CallbackExceptionLatchesUnderFailStopPolicy) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  netft::Client client{config};
  std::atomic<unsigned> calls{};
  client.start([&](const netft::Sample &) {
    ++calls;
    throw std::runtime_error{"fail-stop callback"};
  });
  sensor.queue_record(1, 0, 100);
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), netft::FaultCode::Callback);
  EXPECT_EQ(calls.load(), 1U);
  EXPECT_EQ(client.health().callback_error_count, 1U);
  EXPECT_EQ(client.health().reconnect_count, 0U);
  client.stop();
  EXPECT_EQ(client.fault_code(), netft::FaultCode::Callback);
}

TEST(ClientLifecycle, CallbackCopyExceptionLatchesUnderFailStopPolicy) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  netft::Client client{config};
  auto state = std::make_shared<CopyThrowState>();
  netft::Client::SampleCallback callback{CopyThrowingCallback{state}};
  client.start(std::move(callback));
  ASSERT_EQ(state->copy_attempts.load(), 0U);
  state->throw_on_copy = true;
  sensor.queue_record(1, 0, 100);
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), netft::FaultCode::Callback);
  EXPECT_EQ(state->copy_attempts.load(), 1U);
  EXPECT_EQ(state->calls.load(), 0U);
  EXPECT_EQ(client.health().callback_error_count, 1U);
  EXPECT_EQ(client.health().reconnect_count, 0U);
  client.stop();
}

TEST(ClientLifecycle, EarlierSeriousStatusWinsOverCallbackFailure) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.deliver_samples_with_error_status = true;
  netft::Client client{config};
  client.start([](const netft::Sample &) { throw std::runtime_error{"serious callback failed"}; });
  sensor.queue_record(1, 0x80020000U, 100);
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), netft::FaultCode::SeriousStatus);
  EXPECT_EQ(client.health().callback_error_count, 1U);
  EXPECT_EQ(client.health().device_error_count, 1U);
  client.stop();
}

TEST(ClientLifecycle, CallbackFailureDoesNotConsumeDeliveryRateSlot) {
  netft::test::FakeSensor sensor{500.0};
  auto config = config_for(sensor);
  config.sample_rate_limit_hz = 1.0;
  netft::Client client{config};
  std::atomic<unsigned> calls{};
  client.start([&](const netft::Sample &) {
    if (++calls == 1) {
      sensor.pause();
      throw std::runtime_error{"first delivery failed"};
    }
  });

  EXPECT_FALSE(client.wait_for_first_sample(50ms));
  sensor.resume();
  ASSERT_TRUE(wait_until([&] { return calls.load() >= 2; }, 300ms));
  EXPECT_EQ(client.health().delivered_count, 1U);
  client.stop();
}

TEST(ClientLifecycle, BiasRequiresStreamingAndRestartsRealtime) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 2s;
  netft::Client client{config};
  EXPECT_THROW(client.bias(), netft::NotConnectedError);
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  EXPECT_EQ(client.health().state, netft::ClientState::Connecting);
  EXPECT_THROW(client.bias(), netft::NotConnectedError);

  sensor.resume();
  ASSERT_TRUE(client.wait_for_first_sample(500ms));
  EXPECT_NO_THROW(client.bias());
  EXPECT_TRUE(sensor.wait_for_command(netft::detail::Command::SetSoftwareBias));
  EXPECT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 2));
  client.stop();
}

TEST(ClientLifecycle, CallbackCanReenterBias) {
  netft::test::FakeSensor sensor{200.0};
  netft::Client client{config_for(sensor)};
  std::atomic<unsigned> calls{};
  client.start([&](const netft::Sample &) {
    ++calls;
    client.bias();
  });

  ASSERT_TRUE(wait_until([&] { return calls.load() >= 2 && sensor.commands().size() >= 5; }));
  client.stop();
}

TEST(ClientLifecycle, StartRejectsActiveClientAndStopIsIdempotent) {
  netft::test::FakeSensor sensor;
  netft::Client client{config_for(sensor)};
  client.start([](const netft::Sample &) {});
  EXPECT_THROW(client.start([](const netft::Sample &) {}), std::logic_error);
  client.stop();
  client.stop();
}

TEST(ClientLifecycle, CallbackStopReturnsAndExternalStopReapsWorker) {
  netft::test::FakeSensor sensor{200.0};
  netft::Client client{config_for(sensor)};
  std::atomic<bool> returned{};
  client.start([&](const netft::Sample &) {
    client.stop();
    returned = true;
  });

  ASSERT_TRUE(wait_until([&] { return returned.load(); }));
  ASSERT_TRUE(wait_until([&] { return ClientLifecycleTestAccess::worker_exited(client); }));
  EXPECT_TRUE(ClientLifecycleTestAccess::worker_joinable(client));
  client.stop();
  EXPECT_FALSE(ClientLifecycleTestAccess::worker_joinable(client));
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);

  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(client.wait_for_first_sample(500ms));
  client.stop();
  EXPECT_FALSE(ClientLifecycleTestAccess::worker_joinable(client));
}

TEST(ClientLifecycle, CallbackStartAndConcurrentStopDoNotDeadlock) {
  netft::test::FakeSensor sensor{200.0};
  netft::Client client{config_for(sensor)};
  std::atomic<bool> entered{};
  std::atomic<bool> stop_started{};
  std::atomic<bool> callback_finished{};
  client.start([&](const netft::Sample &) {
    entered = true;
    while (!stop_started) {
      std::this_thread::yield();
    }
    EXPECT_THROW(client.start([](const netft::Sample &) {}), std::logic_error);
    callback_finished = true;
  });
  ASSERT_TRUE(wait_until([&] { return entered.load(); }));

  std::thread stopper([&] {
    stop_started = true;
    client.stop();
  });
  ASSERT_TRUE(wait_until([&] { return callback_finished.load(); }, 300ms));
  stopper.join();
  client.stop();
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);
}

TEST(ClientLifecycle, CallbackStoppedWorkerCanBeReapedByDirectRestart) {
  netft::test::FakeSensor sensor{200.0};
  netft::Client client{config_for(sensor)};
  std::atomic<bool> stopped{};
  client.start([&](const netft::Sample &) {
    client.stop();
    stopped = true;
  });
  ASSERT_TRUE(wait_until([&] { return stopped.load(); }));
  ASSERT_TRUE(wait_until([&] { return client.health().state == netft::ClientState::Stopped; }));
  ASSERT_TRUE(wait_until([&] { return ClientLifecycleTestAccess::worker_exited(client); }));
  EXPECT_TRUE(ClientLifecycleTestAccess::worker_joinable(client));
  const auto stopped_generation = ClientLifecycleTestAccess::generation(client);

  std::atomic<unsigned> restarted{};
  client.start([&](const netft::Sample &) { ++restarted; });
  EXPECT_EQ(ClientLifecycleTestAccess::generation(client), stopped_generation + 1);
  ASSERT_TRUE(wait_until([&] { return restarted.load() > 0; }));
  client.stop();
  EXPECT_FALSE(ClientLifecycleTestAccess::worker_joinable(client));
}

TEST(ClientLifecycle, FaultedWorkerCanBeDirectlyRestartedBeforeItExits) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  netft::Client client{config};
  ClientLifecycleTestAccess::gate_fault_publication(client);
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  const auto faulted_generation = ClientLifecycleTestAccess::generation(client);
  sensor.send_record_now(1, 0x80020000U, 100);

  ASSERT_TRUE(ClientLifecycleTestAccess::wait_for_fault_publication());
  ASSERT_TRUE(client.faulted());
  ASSERT_FALSE(ClientLifecycleTestAccess::worker_exited(client));
  ASSERT_TRUE(ClientLifecycleTestAccess::worker_joinable(client));

  std::atomic<bool> restart_entered{};
  std::atomic<bool> restart_returned{};
  std::exception_ptr restart_error;
  std::atomic<unsigned> restarted_deliveries{};
  std::thread restarter([&] {
    restart_entered = true;
    try {
      client.start([&](const netft::Sample &) { ++restarted_deliveries; });
    } catch (...) {
      restart_error = std::current_exception();
    }
    restart_returned = true;
  });
  EXPECT_TRUE(wait_until([&] { return restart_entered.load(); }));
  EXPECT_FALSE(wait_until([&] { return restart_returned.load(); }, 20ms));

  ClientLifecycleTestAccess::release_fault_publication();
  restarter.join();
  EXPECT_EQ(restart_error, nullptr);
  EXPECT_TRUE(restart_returned.load());
  EXPECT_EQ(ClientLifecycleTestAccess::generation(client), faulted_generation + 1);
  EXPECT_FALSE(client.faulted());

  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 2));
  sensor.send_record_now(1, 0, 104);
  EXPECT_TRUE(wait_until([&] { return restarted_deliveries.load() == 1; }, 500ms));
  client.stop();
}

TEST(ClientLifecycle, RestartClearsActiveStateAndPreservesLifetimeHealth) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.deliver_samples_with_error_status = true;
  config.receive_timeout = 2s;
  netft::Client client{config};

  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  sensor.send_record_now(17, 0x80020000U, 100);
  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  ASSERT_TRUE(wait_until([&] { return ClientLifecycleTestAccess::worker_exited(client); }));
  client.stop();

  const auto before_restart = client.health();
  ASSERT_TRUE(client.latest_sample());
  ASSERT_TRUE(before_restart.sensor_configuration);
  ASSERT_EQ(before_restart.fault_code, netft::FaultCode::SeriousStatus);
  ASSERT_EQ(before_restart.received_count, 1U);
  ASSERT_EQ(before_restart.delivered_count, 1U);
  ASSERT_EQ(before_restart.device_error_count, 1U);
  ASSERT_EQ(before_restart.sensor_configuration->revision, 1U);

  sensor.set_xml_configuration(kChangedConfiguration);
  client.start([](const netft::Sample &) {});

  const auto starting = client.health();
  EXPECT_FALSE(client.faulted());
  EXPECT_EQ(starting.fault_code, netft::FaultCode::None);
  EXPECT_FALSE(client.latest_sample());
  EXPECT_EQ(starting.state, netft::ClientState::Connecting);
  EXPECT_TRUE(starting.last_error.empty());
  EXPECT_DOUBLE_EQ(starting.receive_rate_hz, 0.0);
  EXPECT_DOUBLE_EQ(starting.delivery_rate_hz, 0.0);
  EXPECT_EQ(starting.received_count, before_restart.received_count);
  EXPECT_EQ(starting.delivered_count, before_restart.delivered_count);
  EXPECT_EQ(starting.device_error_count, before_restart.device_error_count);

  ASSERT_TRUE(sensor.wait_for_http_request(2));
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 2));
  const auto rediscovered = client.health();
  ASSERT_TRUE(rediscovered.sensor_configuration);
  EXPECT_EQ(rediscovered.sensor_configuration->revision, 2U);
  EXPECT_EQ(rediscovered.calibration_change_count, before_restart.calibration_change_count + 1);
  EXPECT_EQ(rediscovered.received_count, before_restart.received_count);
  EXPECT_EQ(rediscovered.delivered_count, before_restart.delivered_count);

  sensor.send_record_now(1, 0, 104);
  ASSERT_TRUE(wait_until(
      [&] { return client.health().delivered_count == before_restart.delivered_count + 1; }));
  const auto after_restart = client.health();
  const auto latest = client.latest_sample();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->configuration_revision, 2U);
  EXPECT_EQ(latest->torque_unit, netft::TorqueUnit::NewtonMillimeter);
  EXPECT_EQ(after_restart.received_count, before_restart.received_count + 1);
  EXPECT_EQ(after_restart.delivered_count, before_restart.delivered_count + 1);
  EXPECT_EQ(after_restart.device_error_count, before_restart.device_error_count);
  client.stop();
}

TEST(ClientLifecycle, ConcurrentStartAndStopLeaveClientRestartable) {
  netft::test::FakeSensor sensor{400.0};
  netft::Client client{config_for(sensor)};
  for (int iteration = 0; iteration < 40; ++iteration) {
    std::thread starter([&] {
      try {
        client.start([](const netft::Sample &) {});
      } catch (const std::logic_error &) {
      }
    });
    std::thread stopper([&] { client.stop(); });
    starter.join();
    stopper.join();
    client.stop();
  }

  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(wait_until([&] { return client.health().received_count > 0; }));
  client.stop();
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);
}

TEST(ClientLifecycle, ThreadCreationFailureRestoresPriorFaultedState) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.deliver_samples_with_error_status = true;
  netft::Client client{config};
  std::atomic<unsigned> original_callback_calls{};
  client.start([&](const netft::Sample &) { ++original_callback_calls; });
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  sensor.queue_record(17, 0x80020000U, 100);
  sensor.resume();
  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  ASSERT_TRUE(wait_until([&] { return ClientLifecycleTestAccess::worker_exited(client); }));

  const auto prior_health = client.health();
  const auto prior_latest = client.latest_sample();
  const auto prior_generation = ClientLifecycleTestAccess::generation(client);
  ASSERT_TRUE(prior_latest);
  ASSERT_EQ(original_callback_calls.load(), 1U);
  ASSERT_EQ(prior_health.state, netft::ClientState::Faulted);
  ASSERT_EQ(prior_health.fault_code, netft::FaultCode::SeriousStatus);
  ASSERT_EQ(prior_health.received_count, 1U);
  ASSERT_EQ(prior_health.delivered_count, 1U);

  ClientLifecycleTestAccess::fail_next_thread_creation(client);

  EXPECT_THROW(client.start([](const netft::Sample &) {}), std::system_error);
  const auto restored_health = client.health();
  const auto restored_latest = client.latest_sample();
  ASSERT_TRUE(restored_latest);
  EXPECT_EQ(restored_health.state, prior_health.state);
  EXPECT_EQ(restored_health.fault_code, prior_health.fault_code);
  EXPECT_EQ(restored_health.last_error, prior_health.last_error);
  EXPECT_EQ(restored_health.received_count, prior_health.received_count);
  EXPECT_EQ(restored_health.delivered_count, prior_health.delivered_count);
  EXPECT_EQ(restored_health.device_error_count, prior_health.device_error_count);
  EXPECT_EQ(restored_health.delivery_rate_hz, prior_health.delivery_rate_hz);
  EXPECT_EQ(restored_latest->rdt_sequence, prior_latest->rdt_sequence);
  EXPECT_EQ(restored_latest->ft_sequence, prior_latest->ft_sequence);
  EXPECT_EQ(client.fault_code(), netft::FaultCode::SeriousStatus);
  EXPECT_FALSE(ClientLifecycleTestAccess::worker_joinable(client));
  EXPECT_TRUE(ClientLifecycleTestAccess::worker_exited(client));
  EXPECT_TRUE(ClientLifecycleTestAccess::has_callback(client));
  EXPECT_FALSE(ClientLifecycleTestAccess::stopping(client));
  EXPECT_EQ(ClientLifecycleTestAccess::generation(client), prior_generation);
  EXPECT_TRUE(client.wait_for_first_sample(10ms));
  client.stop();
}

TEST(ClientLifecycle, WaiterWakesForStopAndFault) {
  {
    netft::test::FakeSensor sensor;
    sensor.pause();
    netft::Client client{config_for(sensor)};
    std::atomic<bool> returned{};
    client.start([](const netft::Sample &) {});
    std::thread waiter([&] {
      EXPECT_FALSE(client.wait_for_first_sample(2s));
      returned = true;
    });
    std::this_thread::sleep_for(10ms);
    client.stop();
    ASSERT_TRUE(wait_until([&] { return returned.load(); }, 200ms));
    waiter.join();
  }
  {
    netft::test::FakeSensor sensor;
    sensor.pause();
    auto config = config_for(sensor);
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    config.receive_timeout = 50ms;
    netft::Client client{config};
    std::atomic<bool> returned{};
    client.start([](const netft::Sample &) {});
    std::thread waiter([&] {
      EXPECT_FALSE(client.wait_for_first_sample(2s));
      returned = true;
    });
    ASSERT_TRUE(wait_until([&] { return client.faulted(); }, 500ms));
    ASSERT_TRUE(wait_until([&] { return returned.load(); }, 200ms));
    waiter.join();
    client.stop();
  }
}

TEST(ClientLifecycle, OldWaiterReturnsFalseAfterNewGenerationDelivers) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 2s;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ClientLifecycleTestAccess::gate_waiter_after_wake(client);
  std::atomic<bool> waiter_entered{};
  std::atomic<bool> waiter_returned{};
  std::atomic<bool> waiter_result{true};
  std::thread waiter([&] {
    waiter_entered = true;
    waiter_result = client.wait_for_first_sample(2s);
    waiter_returned = true;
  });
  ASSERT_TRUE(wait_until([&] { return waiter_entered.load(); }));

  client.stop();
  const bool waiter_woke = ClientLifecycleTestAccess::wait_for_waiter_wake();
  client.start([](const netft::Sample &) {});
  sensor.resume();
  const bool new_generation_delivered =
      wait_until([&] { return client.health().delivered_count > 0; }, 500ms);
  EXPECT_FALSE(waiter_returned.load());

  ClientLifecycleTestAccess::release_waiter();
  waiter.join();
  EXPECT_TRUE(waiter_woke);
  EXPECT_TRUE(new_generation_delivered);
  EXPECT_TRUE(waiter_returned.load());
  EXPECT_FALSE(waiter_result.load());
  client.stop();
}

TEST(ClientLifecycle, WaitForFirstSampleRequiresSuccessfulDelivery) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  netft::Client client{config};
  client.start([](const netft::Sample &) { throw std::runtime_error{"delivery failed"}; });
  sensor.queue_record(1, 0, 100);
  sensor.resume();

  EXPECT_FALSE(client.wait_for_first_sample(500ms));
  EXPECT_EQ(client.fault_code(), netft::FaultCode::Callback);
  client.stop();
}

TEST(ClientLifecycle, DestructionSendsStopStreaming) {
  netft::test::FakeSensor sensor;
  {
    netft::Client client{config_for(sensor)};
    client.start([](const netft::Sample &) {});
    ASSERT_TRUE(wait_until([&] { return client.health().received_count > 0; }));
  }
  EXPECT_TRUE(sensor.wait_for_command(netft::detail::Command::StopStreaming));
}

} // namespace
