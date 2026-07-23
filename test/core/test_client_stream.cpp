#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

#include "detail/protocol.hpp"
#include "netft/client.hpp"
#include "support/fake_sensor.hpp"

namespace {

using namespace std::chrono_literals;

netft::Config config_for(const netft::test::FakeSensor &sensor) {
  netft::Config config;
  config.sensor_host = sensor.host();
  config.rdt_port = sensor.rdt_port();
  config.http_port = sensor.http_port();
  config.receive_timeout = 200ms;
  config.configuration_connect_timeout = 200ms;
  config.configuration_timeout = 500ms;
  return config;
}

template <typename Predicate>
bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 1s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

class TwoPartyBarrier {
public:
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto generation = generation_;
    if (++arrivals_ == 2) {
      arrivals_ = 0;
      ++generation_;
      cv_.notify_one();
      return;
    }
    cv_.wait(lock, [this, generation] { return generation_ != generation; });
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  unsigned arrivals_{};
  unsigned generation_{};
};

TEST(ClientStream, ConstructionPerformsNoNetworkIo) {
  netft::test::FakeSensor sensor;

  netft::Client client{config_for(sensor)};

  EXPECT_EQ(sensor.http_request_count(), 0U);
  EXPECT_TRUE(sensor.commands().empty());
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);
  EXPECT_FALSE(client.faulted());
  EXPECT_EQ(client.fault_code(), netft::FaultCode::None);
  EXPECT_FALSE(client.latest_sample());
  EXPECT_FALSE(client.wait_for_first_sample(1ms));
  EXPECT_THROW(client.bias(), netft::NotConnectedError);
}

TEST(ClientStream, DiscoveryCompletesBeforeRealtimeStarts) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  sensor.set_http_response_delay(100ms);
  netft::Client client{config_for(sensor)};

  client.start([](const netft::Sample &) {});

  ASSERT_TRUE(sensor.wait_for_http_request());
  EXPECT_FALSE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 1, 20ms));
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 1, 500ms));
  EXPECT_EQ(sensor.http_request_count(), 1U);
  client.stop();
}

TEST(ClientStream, ConvertsNativeUnitsAndPublishesLatestSample) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  sensor.queue_record(1, 0, 100,
                      {1'000'000, -2'000'000, 3'000'000, 1'000'000, -2'000'000, 3'000'000});
  netft::Client client{config_for(sensor)};
  std::mutex callback_mutex;
  std::optional<netft::Sample> callback_sample;
  client.start([&](const netft::Sample &sample) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (!callback_sample) {
      callback_sample = sample;
      sensor.pause();
    }
  });
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  sensor.resume();

  ASSERT_TRUE(client.wait_for_first_sample(500ms));
  const auto sample = client.latest_sample();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->rdt_sequence, 1U);
  EXPECT_EQ(sample->ft_sequence, 100U);
  EXPECT_EQ(sample->force, (std::array<double, 3>{1.0, -2.0, 3.0}));
  EXPECT_EQ(sample->torque, (std::array<double, 3>{1.0, -2.0, 3.0}));
  EXPECT_EQ(sample->force_unit, netft::ForceUnit::Newton);
  EXPECT_EQ(sample->torque_unit, netft::TorqueUnit::NewtonMeter);
  EXPECT_EQ(sample->configuration_revision, 1U);
  {
    std::lock_guard<std::mutex> lock(callback_mutex);
    ASSERT_TRUE(callback_sample);
    EXPECT_EQ(callback_sample->rdt_sequence, sample->rdt_sequence);
    EXPECT_EQ(callback_sample->force, sample->force);
    EXPECT_EQ(callback_sample->torque, sample->torque);
  }

  const auto health = client.health();
  ASSERT_TRUE(health.sensor_configuration);
  EXPECT_EQ(health.sensor_configuration->source, netft::CalibrationSource::Sensor);
  EXPECT_EQ(health.sensor_configuration->revision, 1U);
  EXPECT_EQ(health.received_count, 1U);
  EXPECT_EQ(health.delivered_count, 1U);
  client.stop();
}

TEST(ClientStream, CompleteCalibrationOverrideSkipsHttpDiscovery) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.calibration_override = netft::Calibration{100.0, 10.0, netft::ForceUnit::PoundForce,
                                                   netft::TorqueUnit::PoundForceFoot};
  netft::Client client{config};

  client.start([](const netft::Sample &) {});

  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  EXPECT_EQ(sensor.http_request_count(), 0U);
  const auto health = client.health();
  ASSERT_TRUE(health.sensor_configuration);
  EXPECT_EQ(health.sensor_configuration->source, netft::CalibrationSource::Override);
  EXPECT_EQ(health.sensor_configuration->calibration.force_unit, netft::ForceUnit::PoundForce);
  client.stop();
}

TEST(ClientStream, StopIsIdempotentAndSendsOneEffectiveCommand) {
  netft::test::FakeSensor sensor;
  netft::Client client{config_for(sensor)};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  client.stop();
  client.stop();

  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StopStreaming));
  const auto commands = sensor.commands();
  EXPECT_EQ(std::count(commands.begin(), commands.end(), netft::detail::Command::StopStreaming), 1);
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);
}

TEST(ClientStream, StopWakesBlockedReceiveBeforeLongTimeout) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 10s;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  std::this_thread::sleep_for(20ms);
  const auto before_stop = std::chrono::steady_clock::now();
  client.stop();
  const auto elapsed = std::chrono::steady_clock::now() - before_stop;

  EXPECT_LT(elapsed, 500ms);
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);
}

TEST(ClientStream, ConcurrentBiasAndRecordTrackingRemainConsistent) {
  netft::test::FakeSensor sensor;
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 2s;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));
  sensor.send_record_now(0, 0, 996);
  ASSERT_TRUE(client.wait_for_first_sample(500ms));

  constexpr std::uint32_t kIterations = 200;
  TwoPartyBarrier barrier;
  std::exception_ptr bias_error;
  std::thread bias_thread([&] {
    try {
      for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        barrier.arrive_and_wait();
        client.bias();
        barrier.arrive_and_wait();
      }
    } catch (...) {
      bias_error = std::current_exception();
    }
  });

  bool all_records_received = true;
  for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
    barrier.arrive_and_wait();
    sensor.send_record_now(iteration + 1, 0, 1000 + iteration * 4);
    barrier.arrive_and_wait();
    all_records_received = wait_until([&client, iteration] {
                             return client.health().received_count >= iteration + 2;
                           }) &&
                           all_records_received;
  }
  bias_thread.join();

  ASSERT_EQ(bias_error, nullptr);
  EXPECT_TRUE(all_records_received);
  const auto health = client.health();
  EXPECT_EQ(health.received_count, kIterations + 1);
  EXPECT_EQ(health.lost_count, 0U);
  EXPECT_EQ(health.duplicate_count, 0U);
  EXPECT_EQ(health.out_of_order_count, 0U);
  client.stop();
}

} // namespace
