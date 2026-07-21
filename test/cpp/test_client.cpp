#include "netft_driver/client.hpp"
#include "support/fake_sensor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>

namespace netft_driver {

struct NetFTClientTestAccess {
  static void fail_next_thread_creation(NetFTClient & client)
  {
    fail_next = true;
    client.thread_factory_ = &create_thread;
  }

  static bool worker_joinable(const NetFTClient & client) { return client.worker_.joinable(); }
  static bool worker_exited(const NetFTClient & client) { return client.worker_exited_; }
  static bool has_callback(const NetFTClient & client) { return static_cast<bool>(client.callback_); }
  static bool stopping(const NetFTClient & client) { return client.stopping_; }
  static void latch_fault(NetFTClient & client, FaultCode code, const char * message)
  {
    client.latch_fault(code, message);
  }
  static std::unique_lock<std::mutex> lock_health(NetFTClient & client)
  {
    return std::unique_lock<std::mutex>{client.health_mutex_};
  }
  static std::unique_lock<std::mutex> lock_socket(NetFTClient & client)
  {
    return std::unique_lock<std::mutex>{client.socket_mutex_};
  }
  static void publish_fault_code_only(NetFTClient & client, FaultCode code)
  {
    client.fault_code_.store(code, std::memory_order_release);
  }
  static std::uint64_t generation(const NetFTClient & client)
  {
    std::lock_guard<std::mutex> lock(client.health_mutex_);
    return client.generation_;
  }

private:
  static std::thread create_thread(NetFTClient * client)
  {
    if (fail_next.exchange(false)) {
      throw std::system_error{
        std::make_error_code(std::errc::resource_unavailable_try_again),
        "injected thread creation failure"};
    }
    return std::thread{&NetFTClient::run, client};
  }

  static inline std::atomic<bool> fail_next{};
};

namespace {
using namespace std::chrono_literals;

ClientConfig config_for(const test::FakeSensor & sensor)
{
  ClientConfig config; config.sensor_host = sensor.host(); config.sensor_port = sensor.port();
  config.receive_timeout = 30ms; config.reconnect_initial_delay = 5ms; config.reconnect_max_delay = 20ms;
  return config;
}
template<class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 1000ms)
{ const auto end = std::chrono::steady_clock::now() + timeout; while (std::chrono::steady_clock::now() < end) { if (predicate()) return true; std::this_thread::sleep_for(2ms); } return predicate(); }

struct CopyThrowState {
  std::atomic<bool> throw_on_copy{};
  std::atomic<unsigned> copy_attempts{};
  std::atomic<unsigned> calls{};
};

struct CopyThrowingCallback {
  explicit CopyThrowingCallback(std::shared_ptr<CopyThrowState> state_in)
  : state{std::move(state_in)} {}

  CopyThrowingCallback(const CopyThrowingCallback & other) : state{other.state}
  {
    ++state->copy_attempts;
    if (state->throw_on_copy) throw std::bad_alloc{};
  }
  CopyThrowingCallback(CopyThrowingCallback &&) noexcept = default;

  void operator()(const WrenchSample &) const { ++state->calls; }

  std::shared_ptr<CopyThrowState> state;
};

TEST(NetFTClient, StreamsScaledSamplesAndStopsIdempotently)
{
  test::FakeSensor sensor{200}; ClientConfig config = config_for(sensor); config.counts_per_force = 100; config.counts_per_torque = 10;
  NetFTClient client{config}; std::vector<WrenchSample> samples; std::mutex samples_mutex; client.start([&](const WrenchSample & sample) { std::lock_guard<std::mutex> lock(samples_mutex); samples.push_back(sample); });
  ASSERT_TRUE(eventually([&] { std::lock_guard<std::mutex> lock(samples_mutex); return samples.size() >= 2; })); client.stop(); client.stop();
  std::lock_guard<std::mutex> samples_lock(samples_mutex); EXPECT_EQ(samples.front().force, (std::array<double, 3>{1, -2, 3})); EXPECT_EQ(samples.front().torque, (std::array<double, 3>{1, -2, 3}));
  EXPECT_TRUE(sensor.wait_for_command(Command::StopStreaming)); EXPECT_EQ(client.health_snapshot().state, ClientState::Stopped);
}
TEST(NetFTClient, CountsWarningRecordsBeforeDuplicateAndOutOfOrderPublishFiltering)
{
  for (const auto & sequences : {std::array<std::uint32_t, 3>{1, 1, 2},
                                std::array<std::uint32_t, 3>{10, 9, 11}}) {
    test::FakeSensor sensor{200}; sensor.pause(); NetFTClient client{config_for(sensor)};
    std::atomic<unsigned> callbacks{};
    client.start([&](const WrenchSample &) { ++callbacks; });
    ASSERT_TRUE(sensor.wait_for_command(Command::StartRealtime));
    sensor.queue_record(sequences[0]);
    sensor.queue_record(sequences[1], 0x80010000U);
    sensor.queue_record(sequences[2]);
    sensor.resume();
    ASSERT_TRUE(eventually([&] { return client.health_snapshot().received_count >= 3; }));
    const auto health = client.health_snapshot();
    EXPECT_EQ(health.warning_count, 1U);
    EXPECT_EQ(health.duplicate_count, sequences[0] == 1 ? 1U : 0U);
    EXPECT_EQ(health.out_of_order_count, sequences[0] == 10 ? 1U : 0U);
    EXPECT_EQ(callbacks.load(), 2U);
    client.stop();
  }
}
TEST(NetFTClient, ReconnectsAfterTimeoutAndResetsBackoffAfterValidRecord)
{
  test::FakeSensor sensor{200}; NetFTClient client{config_for(sensor)}; std::atomic<unsigned> samples{}; client.start([&](const WrenchSample &) { ++samples; });
  ASSERT_TRUE(eventually([&] { return samples >= 2; })); sensor.pause(); ASSERT_TRUE(eventually([&] { return client.health_snapshot().reconnect_count >= 1; })); sensor.resume();
  ASSERT_TRUE(eventually([&] { return samples >= 3; })); client.stop(); EXPECT_GE(sensor.commands().size(), 3U);
}
TEST(NetFTClient, MalformedPacketsDoNotExtendValidDeadlineAndFailStopLatches)
{
  test::FakeSensor sensor{100}; auto config = config_for(sensor); config.receive_timeout = 200ms; config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; client.start([](const WrenchSample &) {});
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().received_count > 0; })); sensor.pause(); for (int i = 0; i < 10; ++i) sensor.queue_payload({1, 2}); sensor.resume();
  ASSERT_TRUE(eventually([&] { return client.faulted(); })); EXPECT_EQ(client.fault_code(), FaultCode::MalformedStorm); EXPECT_EQ(client.health_snapshot().state, ClientState::Faulted); client.stop();
}
TEST(NetFTClient, StopAfterNineMalformedPacketsDoesNotDecodeShutdownWakeup)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 2s; config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; client.start([](const WrenchSample &) {});
  ASSERT_TRUE(sensor.wait_for_command(Command::StartRealtime));
  for (int i = 0; i < 9; ++i) sensor.send_payload_now({1, 2});
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().malformed_count == 9; }));

  client.stop(); const auto health = client.health_snapshot();
  EXPECT_EQ(health.malformed_count, 9U);
  EXPECT_FALSE(client.faulted());
  EXPECT_EQ(health.state, ClientState::Stopped);
}
TEST(NetFTClient, StopAfterDeadlineBeforeTimeoutCommitSuppressesTransportFault)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 100ms; config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; client.start([](const WrenchSample &) {});
  ASSERT_TRUE(sensor.wait_for_command(Command::StartRealtime));
  auto health_lock = NetFTClientTestAccess::lock_health(client);
  std::this_thread::sleep_for(150ms);

  std::thread stopper([&] { client.stop(); });
  EXPECT_TRUE(eventually([&] { return NetFTClientTestAccess::stopping(client); }));
  health_lock.unlock(); stopper.join();

  const auto health = client.health_snapshot();
  EXPECT_EQ(health.timeout_count, 0U);
  EXPECT_FALSE(client.faulted());
  EXPECT_EQ(health.state, ClientState::Stopped);
}
TEST(NetFTClient, StopAfterCommittedTimeoutDoesNotEraseTransportFault)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 100ms; config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; client.start([](const WrenchSample &) {});
  ASSERT_TRUE(sensor.wait_for_command(Command::StartRealtime));
  auto socket_lock = NetFTClientTestAccess::lock_socket(client);
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().timeout_count == 1; }, 500ms));

  std::thread stopper([&] { client.stop(); });
  EXPECT_TRUE(eventually([&] { return NetFTClientTestAccess::stopping(client); }));
  socket_lock.unlock(); stopper.join();

  const auto health = client.health_snapshot();
  EXPECT_EQ(health.timeout_count, 1U);
  EXPECT_EQ(client.fault_code(), FaultCode::Timeout);
  EXPECT_EQ(health.state, ClientState::Faulted);
}
TEST(NetFTClient, FailStopPreservesFirstFaultAndNeverReconnects)
{
  test::FakeSensor sensor{100}; auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; client.start([](const WrenchSample &) {});
  sensor.queue_record(1, 0x80020000); ASSERT_TRUE(eventually([&] { return client.faulted(); })); const auto commands = sensor.commands(); std::this_thread::sleep_for(50ms);
  const auto later_commands = sensor.commands(); EXPECT_EQ(client.fault_code(), FaultCode::SeriousStatus); EXPECT_EQ(std::count(later_commands.begin(), later_commands.end(), Command::StartRealtime), std::count(commands.begin(), commands.end(), Command::StartRealtime)); client.stop();
}
TEST(NetFTClient, FailStopMapsTimeoutSocketAndFtSequenceFaults)
{
  { test::FakeSensor sensor{100}; auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; client.start([](const WrenchSample &) {}); ASSERT_TRUE(eventually([&] { return client.health_snapshot().received_count > 0; })); sensor.pause(); ASSERT_TRUE(eventually([&] { return client.faulted(); })); EXPECT_EQ(client.fault_code(), FaultCode::Timeout); client.stop(); }
  { ClientConfig config; config.sensor_host = "not-a-loopback-host.invalid"; config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; client.start([](const WrenchSample &) {}); ASSERT_TRUE(eventually([&] { return client.faulted(); })); EXPECT_EQ(client.fault_code(), FaultCode::Socket); client.stop(); }
  { test::FakeSensor sensor{100}; auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; client.start([](const WrenchSample &) {}); sensor.queue_record(1, 0, 100); sensor.queue_record(2, 0, 100); ASSERT_TRUE(eventually([&] { return client.faulted(); })); EXPECT_EQ(client.fault_code(), FaultCode::FtStall); client.stop(); }
  { test::FakeSensor sensor{100}; auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; client.start([](const WrenchSample &) {}); sensor.queue_record(1, 0, 100); sensor.queue_record(2, 0, 90); ASSERT_TRUE(eventually([&] { return client.faulted(); })); EXPECT_EQ(client.fault_code(), FaultCode::FtBackward); client.stop(); }
}
TEST(NetFTClient, ReconnectPolicyKeepsFtRestartConfirmationInOneSession)
{
  test::FakeSensor sensor{200}; sensor.pause(); NetFTClient client{config_for(sensor)}; std::vector<std::uint32_t> delivered; std::mutex delivered_mutex;
  client.start([&](const WrenchSample & sample) { std::lock_guard<std::mutex> lock(delivered_mutex); delivered.push_back(sample.ft_sequence); });
  sensor.queue_record(1, 0, 0x00100000); sensor.queue_record(2, 0, 0x00100000); sensor.queue_record(3, 0, 2); sensor.queue_record(4, 0, 6); sensor.resume();
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().ft_restart_count >= 1; }));
  ASSERT_TRUE(eventually([&] { std::lock_guard<std::mutex> lock(delivered_mutex); return delivered.size() >= 2; }));
  client.stop(); const auto health = client.health_snapshot();
  std::lock_guard<std::mutex> delivered_lock(delivered_mutex);
  EXPECT_EQ(delivered, (std::vector<std::uint32_t>{0x00100000, 6}));
  EXPECT_EQ(health.reconnect_count, 0U); EXPECT_EQ(health.ft_stall_count, 1U); EXPECT_EQ(health.ft_backward_count, 1U); EXPECT_EQ(health.ft_restart_count, 1U); EXPECT_EQ(health.last_ft_progress, "restart");
}
TEST(NetFTClient, SeriousStatusSurvivesReconnectFtDiscontinuityDrop)
{
  for (const auto discontinuity : {0x00100000U, 2U}) {
    test::FakeSensor sensor{20}; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 2s;
    NetFTClient client{config}; std::atomic<unsigned> callbacks{};
    client.start([&](const WrenchSample &) { ++callbacks; });
    sensor.queue_record(1, 0, 0x00100000U);
    sensor.queue_record(2, 0x80020000U, discontinuity);
    sensor.resume();

    ASSERT_TRUE(eventually([&] { return client.health_snapshot().device_error_count == 1; }));
    sensor.pause();
    ASSERT_TRUE(eventually([&] { return client.health_snapshot().reconnect_count == 1; }, 300ms));
    const auto health = client.health_snapshot();
    EXPECT_EQ(callbacks.load(), 1U);
    EXPECT_EQ(health.device_error_count, 1U);
    EXPECT_EQ(health.ft_stall_count, discontinuity == 0x00100000U ? 1U : 0U);
    EXPECT_EQ(health.ft_backward_count, discontinuity == 2U ? 1U : 0U);
    EXPECT_EQ(health.last_error, "serious device status");
    EXPECT_FALSE(client.faulted());
    client.stop();
  }
}
TEST(NetFTClient, ValidRecordResetsMalformedStormAndNonfatalObservationsStayStreaming)
{
  test::FakeSensor sensor{500}; auto config = config_for(sensor); config.receive_timeout = 500ms; config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; std::atomic<unsigned> samples{};
  for (int i = 0; i < 9; ++i) sensor.queue_payload({1, 2}); sensor.queue_record(10, 0x80010000, 100); for (int i = 0; i < 9; ++i) sensor.queue_payload({1, 2}); sensor.queue_record(13, 0, 104);
  client.start([&](const WrenchSample &) { ++samples; }); ASSERT_TRUE(eventually([&] { return samples >= 2; })); EXPECT_FALSE(client.faulted()); const auto health = client.health_snapshot(); EXPECT_EQ(health.malformed_count, 18U); EXPECT_EQ(health.lost_count, 2U); client.stop();
}
TEST(NetFTClient, PublishRateDropsIntermediateSamplesWithoutQueueing)
{
  test::FakeSensor sensor{1000}; auto config = config_for(sensor); config.publish_rate = 50; NetFTClient client{config}; std::atomic<unsigned> samples{}; client.start([&](const WrenchSample &) { ++samples; }); std::this_thread::sleep_for(150ms); client.stop(); const auto health = client.health_snapshot(); EXPECT_GT(health.received_count, health.published_count); EXPECT_GT(health.rate_dropped_count, 0U); EXPECT_LT(samples, health.received_count);
}
TEST(NetFTClient, CallbackExceptionsAreAccountedWithoutStoppingReconnectClient)
{
  test::FakeSensor sensor{200}; NetFTClient client{config_for(sensor)}; std::atomic<unsigned> calls{}; client.start([&](const WrenchSample &) { ++calls; throw std::runtime_error{"consumer failed"}; });
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().callback_error_count >= 3; })); EXPECT_FALSE(client.faulted()); EXPECT_GE(calls.load(), 3U); client.stop();
}
TEST(NetFTClient, CallbackCopyExceptionsAreAccountedWithoutStoppingWorker)
{
  test::FakeSensor sensor{200}; sensor.pause();
  NetFTClient client{config_for(sensor)};
  auto state = std::make_shared<CopyThrowState>();
  NetFTClient::SampleCallback callback{CopyThrowingCallback{state}};
  client.start(std::move(callback));
  ASSERT_EQ(state->copy_attempts.load(), 0U);

  state->throw_on_copy = true; sensor.resume();
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().callback_error_count >= 3; }));
  EXPECT_EQ(state->calls.load(), 0U);
  EXPECT_FALSE(client.faulted());

  state->throw_on_copy = false;
  ASSERT_TRUE(eventually([&] { return state->calls.load() > 0; }));
  EXPECT_TRUE(client.wait_for_first_sample(100ms));
  client.stop();
}
TEST(NetFTClient, FailStopCallbackInvocationLatchesCallbackFaultWithoutReconnect)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; std::atomic<unsigned> calls{};
  client.start([&](const WrenchSample &) { ++calls; throw std::runtime_error{"fail-stop callback"}; });
  sensor.queue_record(1); sensor.resume();

  ASSERT_TRUE(eventually([&] { return client.faulted(); })); const auto starts = sensor.commands();
  EXPECT_EQ(client.fault_code(), FaultCode::Callback);
  EXPECT_EQ(calls.load(), 1U);
  EXPECT_EQ(client.health_snapshot().callback_error_count, 1U);
  EXPECT_EQ(client.health_snapshot().reconnect_count, 0U);
  ASSERT_TRUE(eventually([&] { return NetFTClientTestAccess::worker_exited(client); }));
  const auto first_error = client.health_snapshot().last_error;
  NetFTClientTestAccess::latch_fault(client, FaultCode::SeriousStatus, "later fault");
  EXPECT_EQ(client.fault_code(), FaultCode::Callback);
  EXPECT_EQ(client.health_snapshot().last_error, first_error);
  std::this_thread::sleep_for(20ms);
  const auto later_commands = sensor.commands();
  EXPECT_EQ(std::count(later_commands.begin(), later_commands.end(), Command::StartRealtime), std::count(starts.begin(), starts.end(), Command::StartRealtime));
  client.stop(); EXPECT_EQ(client.fault_code(), FaultCode::Callback);
}
TEST(NetFTClient, FailStopCallbackCopyLatchesCallbackFaultWithoutReconnect)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; auto state = std::make_shared<CopyThrowState>();
  NetFTClient::SampleCallback callback{CopyThrowingCallback{state}};
  client.start(std::move(callback)); ASSERT_EQ(state->copy_attempts.load(), 0U);
  state->throw_on_copy = true; sensor.queue_record(1); sensor.resume();

  ASSERT_TRUE(eventually([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), FaultCode::Callback);
  EXPECT_EQ(state->copy_attempts.load(), 1U);
  EXPECT_EQ(state->calls.load(), 0U);
  EXPECT_EQ(client.health_snapshot().callback_error_count, 1U);
  EXPECT_EQ(client.health_snapshot().reconnect_count, 0U);
  ASSERT_TRUE(eventually([&] { return NetFTClientTestAccess::worker_exited(client); }));
  client.stop(); EXPECT_EQ(client.fault_code(), FaultCode::Callback);
}
TEST(NetFTClient, FailStopSeriousStatusSurvivesCallbackInvocationFailure)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.publish_on_error = true; config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; std::atomic<unsigned> calls{};
  client.start([&](const WrenchSample &) { if (++calls == 2) throw std::runtime_error{"serious callback failed"}; });
  sensor.queue_record(1, 0, 100);
  sensor.queue_record(2, 0x80020000U, 104);
  sensor.resume();

  ASSERT_TRUE(eventually([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), FaultCode::SeriousStatus);
  EXPECT_EQ(calls.load(), 2U);
  EXPECT_EQ(client.health_snapshot().device_error_count, 1U);
  EXPECT_EQ(client.health_snapshot().callback_error_count, 1U);
  const auto first_error = client.health_snapshot().last_error;
  NetFTClientTestAccess::latch_fault(client, FaultCode::Callback, "later callback fault");
  EXPECT_EQ(client.fault_code(), FaultCode::SeriousStatus);
  EXPECT_EQ(client.health_snapshot().last_error, first_error);
  client.stop();
}
TEST(NetFTClient, FailStopSeriousStatusSurvivesCallbackCopyFailure)
{
  test::FakeSensor sensor{20}; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 500ms; config.publish_on_error = true; config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; auto state = std::make_shared<CopyThrowState>();
  NetFTClient::SampleCallback callback{CopyThrowingCallback{state}};
  client.start(std::move(callback));
  sensor.queue_record(1, 0, 100);
  sensor.queue_record(2, 0x80020000U, 104);
  sensor.resume();
  ASSERT_TRUE(eventually([&] { return state->calls.load() == 1; }));
  state->throw_on_copy = true;

  ASSERT_TRUE(eventually([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), FaultCode::SeriousStatus);
  EXPECT_EQ(state->calls.load(), 1U);
  EXPECT_EQ(client.health_snapshot().device_error_count, 1U);
  EXPECT_EQ(client.health_snapshot().callback_error_count, 1U);
  client.stop();
}
TEST(NetFTClient, CallbackFailureDoesNotPublishOrConsumeRateSlot)
{
  test::FakeSensor sensor{500}; auto config = config_for(sensor); config.publish_rate = 1.0;
  NetFTClient client{config}; std::atomic<unsigned> calls{};
  client.start([&](const WrenchSample &) { if (++calls == 1) { sensor.pause(); throw std::runtime_error{"first"}; } });
  EXPECT_FALSE(client.wait_for_first_sample(50ms));
  sensor.resume();
  ASSERT_TRUE(eventually([&] { return calls >= 2; }, 300ms));
  EXPECT_EQ(client.health_snapshot().published_count, 1U); client.stop();
}
TEST(NetFTClient, ConnectingRejectsBiasUntilFirstValidRecord)
{
  test::FakeSensor sensor; sensor.pause(); NetFTClient client{config_for(sensor)};
  client.start([](const WrenchSample &) {}); EXPECT_THROW(client.bias(), NotConnectedError);
  sensor.resume(); ASSERT_TRUE(client.wait_for_first_sample(500ms)); EXPECT_NO_THROW(client.bias()); client.stop();
}
TEST(NetFTClient, SeriousCallbackStopStillLatchesFailStopReason)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.publish_on_error = true; config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; client.start([&](const WrenchSample &) { client.stop(); }); sensor.queue_record(1, 0x80020000U); sensor.resume();
  ASSERT_TRUE(eventually([&] { return client.faulted(); })); EXPECT_EQ(client.fault_code(), FaultCode::SeriousStatus); EXPECT_EQ(client.health_snapshot().state, ClientState::Faulted); client.stop();
}
TEST(NetFTClient, WaitForFirstSampleBeforeStartIsFalse)
{
  test::FakeSensor sensor; NetFTClient client{config_for(sensor)}; EXPECT_FALSE(client.wait_for_first_sample(1ms));
}
TEST(NetFTClient, TwoKilohertzStreamRemainsBoundedAndStoppable)
{
  test::FakeSensor sensor{2000}; auto config = config_for(sensor); config.receive_timeout = 100ms;
  NetFTClient client{config}; std::atomic<unsigned> callbacks{}; client.start([&](const WrenchSample &) { ++callbacks; });
  std::this_thread::sleep_for(500ms); const auto started = std::chrono::steady_clock::now(); client.stop();
  const auto stopped_in = std::chrono::steady_clock::now() - started; const auto health = client.health_snapshot();
  EXPECT_GE(health.received_count, 500U); EXPECT_EQ(health.published_count, callbacks.load()); EXPECT_LE(stopped_in, 250ms); EXPECT_EQ(health.state, ClientState::Stopped); EXPECT_LE(health.receive_rate, 2500.0);
}
TEST(NetFTClient, SparseMalformedDoesNotExtendValidRecordDeadline)
{
  test::FakeSensor sensor{100}; auto config = config_for(sensor); config.receive_timeout = 120ms; config.reconnect_initial_delay = 5ms; config.reconnect_max_delay = 20ms;
  NetFTClient client{config}; client.start([](const WrenchSample &) {}); ASSERT_TRUE(client.wait_for_first_sample(500ms));
  const auto valid = std::chrono::steady_clock::now(); sensor.pause(); std::this_thread::sleep_for(80ms); sensor.send_payload_now({1, 2});
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().timeout_count >= 1; }, 400ms)); const auto elapsed = std::chrono::steady_clock::now() - valid; const auto health = client.health_snapshot();
  EXPECT_GE(elapsed, 100ms); EXPECT_LT(elapsed, 200ms); EXPECT_EQ(health.malformed_count, 1U); EXPECT_EQ(health.reconnect_count, 1U); EXPECT_NE(client.fault_code(), FaultCode::MalformedStorm); client.stop();
}
TEST(NetFTClient, BackoffResetsAfterValidRecoveryAndRemainsCapped)
{
  test::FakeSensor sensor{200}; auto config = config_for(sensor); config.receive_timeout = 40ms; config.reconnect_initial_delay = 20ms; config.reconnect_max_delay = 80ms;
  NetFTClient client{config}; std::atomic<unsigned> samples{}; client.start([&](const WrenchSample &) { ++samples; }); ASSERT_TRUE(eventually([&] { return samples > 0; }));
  const auto starts = [&] { std::vector<std::chrono::steady_clock::time_point> result; for (const auto & event : sensor.command_events()) if (event.command == Command::StartRealtime) result.push_back(event.at); return result; };
  sensor.pause(); ASSERT_TRUE(eventually([&] { return starts().size() >= 4; }, 500ms));
  const auto first_cycle = starts(); ASSERT_GE(first_cycle.size(), 4U);
  ASSERT_TRUE(eventually([&] { return starts().size() >= 5; }, 800ms)); const auto capped_cycle = starts();
  const auto delay_after_timeout = [](auto later, auto earlier) { return std::chrono::duration_cast<std::chrono::milliseconds>(later - earlier - 40ms); };
  EXPECT_NEAR(delay_after_timeout(capped_cycle[1], capped_cycle[0]).count(), 20, 8);
  EXPECT_NEAR(delay_after_timeout(capped_cycle[2], capped_cycle[1]).count(), 40, 8);
  EXPECT_NEAR(delay_after_timeout(capped_cycle[3], capped_cycle[2]).count(), 80, 8);
  EXPECT_NEAR(delay_after_timeout(capped_cycle[4], capped_cycle[3]).count(), 80, 8);
  const auto recovery_baseline = samples.load(); sensor.resume(); ASSERT_TRUE(eventually([&] { return samples >= recovery_baseline + 1; }, 500ms));
  sensor.pause(); const auto paused = std::chrono::steady_clock::now(); const auto prior_starts = starts().size();
  ASSERT_TRUE(eventually([&] { return starts().size() > prior_starts; }, 300ms)); const auto reset_start = starts().back();
  EXPECT_NEAR(std::chrono::duration_cast<std::chrono::milliseconds>(reset_start - paused - 40ms).count(), 20, 8); client.stop();
}
TEST(NetFTClient, CallbackCanReenterBiasWithoutDeadlockingReceiver)
{
  test::FakeSensor sensor{200}; NetFTClient client{config_for(sensor)}; std::atomic<unsigned> calls{};
  client.start([&](const WrenchSample &) { ++calls; client.bias(); });
  ASSERT_TRUE(eventually([&] { return calls >= 2 && sensor.commands().size() >= 5; }));
  client.stop();
  EXPECT_GE(sensor.commands().size(), 5U);
}
TEST(NetFTClient, PublishOnErrorControlsSeriousStatusDelivery)
{
  for (const bool publish_on_error : {false, true}) {
    test::FakeSensor sensor{100}; auto config = config_for(sensor); config.publish_on_error = publish_on_error;
    std::atomic<unsigned> delivered{}; NetFTClient client{config};
    sensor.pause(); client.start([&](const WrenchSample & sample) { if (sample.status == 0x80020000U) ++delivered; });
    sensor.queue_record(1, 0x80020000U, 100); sensor.resume();
    ASSERT_TRUE(eventually([&] { return client.health_snapshot().device_error_count != 0; }));
    ASSERT_TRUE(eventually([&] { return client.health_snapshot().reconnect_count != 0; }));
    EXPECT_EQ(delivered.load(), publish_on_error ? 1U : 0U); client.stop();
  }
}
TEST(NetFTClient, FaultReadNeverPublishesNoneAfterFaulted)
{
  test::FakeSensor sensor{100}; auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop;
  NetFTClient client{config}; std::atomic<bool> done{}, bad{};
  std::thread observer([&] { while (!done) if (client.faulted() && client.fault_code() == FaultCode::None) bad = true; });
  client.start([](const WrenchSample &) {}); sensor.queue_record(1, 0x80020000U);
  ASSERT_TRUE(eventually([&] { return client.faulted(); })); done = true; observer.join();
  EXPECT_FALSE(bad); EXPECT_EQ(client.fault_code(), FaultCode::SeriousStatus); client.stop();
}
TEST(NetFTClient, HealthSnapshotNeverPairsFaultWithNonFaultedStateDuringReactivation)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 2s;
  NetFTClient client{config}; std::atomic<bool> done{}, bad{};
  std::thread observer([&] {
    while (!done) {
      const auto health = client.health_snapshot();
      if (health.fault_code != FaultCode::None && health.state != ClientState::Faulted) bad = true;
    }
  });

  NetFTClientTestAccess::publish_fault_code_only(client, FaultCode::Timeout);
  const auto transition_snapshot = client.health_snapshot();
  if (transition_snapshot.fault_code != FaultCode::None && transition_snapshot.state != ClientState::Faulted) bad = true;
  client.start([](const WrenchSample &) {}); client.stop();
  for (int i = 0; i < 100; ++i) {
    NetFTClientTestAccess::latch_fault(client, FaultCode::Timeout, "observer transition");
    client.start([](const WrenchSample &) {}); client.stop();
  }
  done = true; observer.join();
  EXPECT_FALSE(bad);
}
TEST(NetFTClient, ConcurrentStartAndStopLeavesClientRestartable)
{
  test::FakeSensor sensor{400}; NetFTClient client{config_for(sensor)};
  for (int i = 0; i < 40; ++i) {
    std::thread starter([&] { try { client.start([](const WrenchSample &) {}); } catch (const std::runtime_error &) {} });
    std::thread stopper([&] { client.stop(); });
    starter.join(); stopper.join(); client.stop();
  }
  client.start([](const WrenchSample &) {});
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().received_count > 0; })); client.stop();
  EXPECT_EQ(client.health_snapshot().state, ClientState::Stopped);
}
TEST(NetFTClient, ThreadCreationFailureRollsBackAndAllowsRestart)
{
  test::FakeSensor sensor; sensor.pause(); NetFTClient client{config_for(sensor)};
  NetFTClientTestAccess::fail_next_thread_creation(client);

  EXPECT_THROW(client.start([](const WrenchSample &) {}), std::system_error);
  EXPECT_EQ(client.health_snapshot().state, ClientState::Stopped);
  EXPECT_FALSE(NetFTClientTestAccess::worker_joinable(client));
  EXPECT_TRUE(NetFTClientTestAccess::worker_exited(client));
  EXPECT_FALSE(NetFTClientTestAccess::has_callback(client));
  EXPECT_FALSE(NetFTClientTestAccess::stopping(client));
  EXPECT_EQ(NetFTClientTestAccess::generation(client), 0U);
  EXPECT_FALSE(client.wait_for_first_sample(10ms));

  sensor.resume(); client.start([](const WrenchSample &) {});
  EXPECT_TRUE(client.wait_for_first_sample(500ms));
  client.stop();
}
TEST(NetFTClient, StopDoesNotWaitForNameResolutionInWorker)
{
  ClientConfig config; config.sensor_host = "not-a-loopback-host.invalid";
  NetFTClient client{config};
  const auto started = std::chrono::steady_clock::now();
  client.start([](const WrenchSample &) {}); client.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
}
TEST(NetFTClient, CallbackStartAndConcurrentStopCompleteWithoutDeadlock)
{
  test::FakeSensor sensor{200}; NetFTClient client{config_for(sensor)};
  std::atomic<bool> entered{}, stop_started{}, callback_finished{};
  client.start([&](const WrenchSample &) {
    entered = true;
    while (!stop_started) std::this_thread::yield();
    EXPECT_THROW(client.start([](const WrenchSample &) {}), std::runtime_error);
    callback_finished = true;
  });
  ASSERT_TRUE(eventually([&] { return entered.load(); }));
  std::thread stopper([&] { stop_started = true; client.stop(); });
  ASSERT_TRUE(eventually([&] { return callback_finished.load(); }, 300ms));
  stopper.join(); client.stop();
  EXPECT_EQ(client.health_snapshot().state, ClientState::Stopped);
}
TEST(NetFTClient, CallbackStopDoesNotSelfJoinAndCanBeCleanedUp)
{
  test::FakeSensor sensor{200}; NetFTClient client{config_for(sensor)}; std::atomic<bool> returned{};
  client.start([&](const WrenchSample &) { client.stop(); returned = true; });
  ASSERT_TRUE(eventually([&] { return returned.load(); }));
  client.stop();
  EXPECT_EQ(client.health_snapshot().state, ClientState::Stopped);
}
TEST(NetFTClient, CallbackStopWorkerCanBeReapedByDirectRestart)
{
  test::FakeSensor sensor{200}; NetFTClient client{config_for(sensor)}; std::atomic<bool> stopped{};
  client.start([&](const WrenchSample &) { client.stop(); stopped = true; });
  ASSERT_TRUE(eventually([&] { return stopped.load(); }));
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().state == ClientState::Stopped; }));
  std::atomic<unsigned> restarted{}; client.start([&](const WrenchSample &) { ++restarted; });
  ASSERT_TRUE(eventually([&] { return restarted.load() > 0; })); client.stop();
}
TEST(NetFTClient, WaitForFirstSampleWakesImmediatelyForStopAndFault)
{
  { test::FakeSensor sensor; sensor.pause(); NetFTClient client{config_for(sensor)}; std::atomic<bool> returned{};
    client.start([](const WrenchSample &) {}); std::thread waiter([&] { client.wait_for_first_sample(2s); returned = true; });
    std::this_thread::sleep_for(10ms); client.stop(); ASSERT_TRUE(eventually([&] { return returned.load(); }, 200ms)); waiter.join(); }
  { test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop; NetFTClient client{config}; std::atomic<bool> returned{};
    client.start([](const WrenchSample &) {}); std::thread waiter([&] { client.wait_for_first_sample(2s); returned = true; });
    ASSERT_TRUE(eventually([&] { return client.faulted(); }, 300ms)); ASSERT_TRUE(eventually([&] { return returned.load(); }, 200ms)); waiter.join(); client.stop(); }
}
TEST(NetFTClient, OldFirstSampleWaiterReturnsFalseAcrossImmediateRestart)
{
  test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.receive_timeout = 2s;
  NetFTClient client{config}; client.start([](const WrenchSample &) {});
  std::atomic<bool> waiter_entered{}, waiter_returned{}, waiter_result{true};
  std::thread waiter([&] {
    waiter_entered = true;
    waiter_result = client.wait_for_first_sample(2s);
    waiter_returned = true;
  });
  ASSERT_TRUE(eventually([&] { return waiter_entered.load(); }));
  std::this_thread::sleep_for(10ms);

  const auto stopped_at = std::chrono::steady_clock::now();
  client.stop();
  client.start([](const WrenchSample &) {});
  ASSERT_TRUE(eventually([&] { return waiter_returned.load(); }, 200ms));
  EXPECT_FALSE(waiter_result.load());
  EXPECT_LT(std::chrono::steady_clock::now() - stopped_at, 500ms);
  waiter.join(); client.stop();
}
TEST(NetFTClient, WaitForFirstSampleReturnsTrueOnlyForPublishedSample)
{
  { test::FakeSensor sensor; sensor.pause(); auto config = config_for(sensor); config.recovery_policy = RecoveryPolicy::FailStop;
    NetFTClient client{config}; client.start([](const WrenchSample &) {}); sensor.queue_record(1, 0x80020000U); sensor.resume();
    EXPECT_FALSE(client.wait_for_first_sample(500ms)); client.stop(); }
  { test::FakeSensor sensor; NetFTClient client{config_for(sensor)}; client.start([](const WrenchSample &) {});
    EXPECT_TRUE(client.wait_for_first_sample(500ms)); client.stop(); }
}
TEST(NetFTClient, ConcurrentBiasAndSessionInitializationKeepTrackersSerialized)
{
  test::FakeSensor sensor{1000}; NetFTClient client{config_for(sensor)}; std::atomic<bool> done{};
  client.start([](const WrenchSample &) {});
  ASSERT_TRUE(eventually([&] { return client.health_snapshot().state == ClientState::Streaming; }));
  std::thread biaser([&] { for (int i = 0; i < 100; ++i) { try { client.bias(); } catch (const NotConnectedError &) {} } done = true; });
  ASSERT_TRUE(eventually([&] { return done.load(); }, 1000ms)); biaser.join(); client.stop();
}
TEST(NetFTClient, RejectsInvalidConfigWithoutConstructingWorkerState)
{
  ClientConfig config; config.sensor_port = 65536;
  EXPECT_THROW(NetFTClient{config}, std::invalid_argument);
}
TEST(NetFTClient, BiasRequiresStreamingAndRestartsRealtime)
{
  test::FakeSensor sensor; NetFTClient client{config_for(sensor)}; EXPECT_THROW(client.bias(), NotConnectedError); client.start([](const WrenchSample &) {}); ASSERT_TRUE(eventually([&] { return client.health_snapshot().state == ClientState::Streaming; })); client.bias(); EXPECT_TRUE(sensor.wait_for_command(Command::SetSoftwareBias)); EXPECT_TRUE(sensor.wait_for_command(Command::StartRealtime, 2)); client.stop();
}
TEST(NetFTClient, DestructionLeavesNoLiveWorker)
{
  test::FakeSensor sensor; { NetFTClient client{config_for(sensor)}; client.start([](const WrenchSample &) {}); ASSERT_TRUE(eventually([&] { return client.health_snapshot().received_count > 0; })); } EXPECT_TRUE(sensor.wait_for_command(Command::StopStreaming));
}
}  // namespace
}  // namespace netft_driver
