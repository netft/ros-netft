#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "detail/protocol.hpp"
#include "netft/client.hpp"
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
  config.reconnect_initial_delay = 10ms;
  config.reconnect_max_delay = 40ms;
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

std::size_t start_count(const netft::test::FakeSensor &sensor) {
  const auto commands = sensor.commands();
  return static_cast<std::size_t>(
      std::count(commands.begin(), commands.end(), netft::detail::Command::StartRealtime));
}

std::vector<std::chrono::steady_clock::time_point>
start_times(const netft::test::FakeSensor &sensor) {
  std::vector<std::chrono::steady_clock::time_point> times;
  for (const auto &event : sensor.command_events()) {
    if (event.command == netft::detail::Command::StartRealtime) {
      times.push_back(event.at);
    }
  }
  return times;
}

TEST(ClientRecovery, CountsLossDuplicatesAndOutOfOrderBeforeOrderedDelivery) {
  netft::test::FakeSensor sensor{500.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.receive_timeout = 1s;
  std::mutex delivered_mutex;
  std::vector<std::uint32_t> delivered;
  netft::Client client{config};
  client.start([&](const netft::Sample &sample) {
    std::lock_guard<std::mutex> lock(delivered_mutex);
    delivered.push_back(sample.rdt_sequence);
  });
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  sensor.send_record_now(1, 0, 100);
  ASSERT_TRUE(wait_until([&] { return client.health().received_count == 1; }));
  sensor.send_record_now(4, 0, 104);
  ASSERT_TRUE(wait_until([&] { return client.health().received_count == 2; }));
  sensor.send_record_now(4, 0x80010000U, 108);
  ASSERT_TRUE(wait_until([&] { return client.health().received_count == 3; }));
  sensor.send_record_now(3, 0x80010000U, 112);
  ASSERT_TRUE(wait_until([&] { return client.health().received_count == 4; }));
  client.stop();
  const auto health = client.health();
  std::lock_guard<std::mutex> lock(delivered_mutex);
  EXPECT_EQ(delivered, (std::vector<std::uint32_t>{1, 4}));
  EXPECT_EQ(health.received_count, 4U);
  EXPECT_EQ(health.delivered_count, 2U);
  EXPECT_EQ(health.lost_count, 2U);
  EXPECT_EQ(health.duplicate_count, 1U);
  EXPECT_EQ(health.out_of_order_count, 1U);
  EXPECT_EQ(health.warning_count, 2U);
  EXPECT_GT(health.receive_rate_hz, 0.0);
  EXPECT_GT(health.delivery_rate_hz, 0.0);
}

TEST(ClientRecovery, ValidRecordResetsConsecutiveMalformedCount) {
  netft::test::FakeSensor sensor{500.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.receive_timeout = 1s;
  std::atomic<unsigned> delivered{};
  netft::Client client{config};

  for (int count = 0; count < 9; ++count) {
    sensor.queue_payload({1, 2});
  }
  sensor.queue_record(10, 0x80010000U, 100);
  for (int count = 0; count < 9; ++count) {
    sensor.queue_payload({1, 2});
  }
  sensor.queue_record(13, 0, 104);
  client.start([&](const netft::Sample &) { ++delivered; });
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return delivered.load() >= 2; }));
  const auto health = client.health();
  EXPECT_FALSE(client.faulted());
  EXPECT_EQ(health.malformed_count, 18U);
  EXPECT_EQ(health.lost_count, 2U);
  EXPECT_EQ(health.warning_count, 1U);
  client.stop();
}

TEST(ClientRecovery, TenConsecutiveMalformedRecordsLatchMalformedStorm) {
  netft::test::FakeSensor sensor{500.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.receive_timeout = 1s;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  for (int count = 0; count < 10; ++count) {
    sensor.queue_payload({1, 2});
  }
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), netft::FaultCode::MalformedStorm);
  const auto health = client.health();
  EXPECT_EQ(health.malformed_count, 10U);
  EXPECT_EQ(health.state, netft::ClientState::Faulted);
  client.stop();
}

TEST(ClientRecovery, MalformedRecordDoesNotExtendValidRecordDeadline) {
  netft::test::FakeSensor sensor{200.0};
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  config.receive_timeout = 200ms;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(client.wait_for_first_sample(500ms));

  sensor.pause();
  const auto last_valid = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(150ms);
  sensor.send_payload_now({1, 2});
  ASSERT_TRUE(wait_until([&] { return client.faulted(); }, 500ms));
  const auto elapsed = std::chrono::steady_clock::now() - last_valid;

  EXPECT_EQ(client.fault_code(), netft::FaultCode::Timeout);
  EXPECT_EQ(client.health().malformed_count, 1U);
  EXPECT_GE(elapsed, 175ms);
  EXPECT_LT(elapsed, 300ms);
  client.stop();
}

TEST(ClientRecovery, SeriousStatusUsesDeliveryPolicyAndLatchesFailStop) {
  for (const bool deliver_error : {false, true}) {
    netft::test::FakeSensor sensor{100.0};
    sensor.pause();
    auto config = config_for(sensor);
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    config.deliver_samples_with_error_status = deliver_error;
    std::atomic<unsigned> delivered{};
    netft::Client client{config};
    client.start([&](const netft::Sample &) { ++delivered; });
    ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

    sensor.queue_record(1, 0x80020000U, 100);
    sensor.resume();

    ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
    EXPECT_EQ(client.fault_code(), netft::FaultCode::SeriousStatus);
    EXPECT_EQ(delivered.load(), deliver_error ? 1U : 0U);
    EXPECT_EQ(client.health().device_error_count, 1U);
    client.stop();
  }
}

TEST(ClientRecovery, FailStopMapsFtStallAndBackwardFaults) {
  for (const auto test_case : {
           std::pair{100U, netft::FaultCode::FtStall},
           std::pair{90U, netft::FaultCode::FtBackward},
       }) {
    netft::test::FakeSensor sensor{200.0};
    sensor.pause();
    auto config = config_for(sensor);
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    netft::Client client{config};
    client.start([](const netft::Sample &) {});
    ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

    sensor.queue_record(1, 0, 100);
    sensor.queue_record(2, 0, test_case.first);
    sensor.resume();

    ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
    EXPECT_EQ(client.fault_code(), test_case.second);
    EXPECT_EQ(client.health().reconnect_count, 0U);
    client.stop();
  }
}

TEST(ClientRecovery, ReconnectPolicyDropsFtDiscontinuitiesAndConfirmsRestart) {
  netft::test::FakeSensor sensor{500.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 1s;
  std::mutex delivered_mutex;
  std::vector<std::uint32_t> delivered;
  netft::Client client{config};
  client.start([&](const netft::Sample &sample) {
    std::lock_guard<std::mutex> lock(delivered_mutex);
    delivered.push_back(sample.ft_sequence);
    if (sample.ft_sequence == 6) {
      sensor.pause();
    }
  });
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  sensor.queue_record(1, 0, 0x00100000U);
  sensor.queue_record(2, 0, 0x00100000U);
  sensor.queue_record(3, 0, 2);
  sensor.queue_record(4, 0, 6);
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return client.health().ft_restart_count == 1; }));
  client.stop();
  const auto health = client.health();
  std::lock_guard<std::mutex> lock(delivered_mutex);
  EXPECT_EQ(delivered, (std::vector<std::uint32_t>{0x00100000U, 6U}));
  EXPECT_EQ(health.ft_stall_count, 1U);
  EXPECT_EQ(health.ft_backward_count, 1U);
  EXPECT_EQ(health.ft_restart_count, 1U);
  EXPECT_EQ(health.last_ft_progress, "restart");
  EXPECT_EQ(health.reconnect_count, 0U);
}

TEST(ClientRecovery, ReconnectResetsRdtOrderingForNewLowSequenceStream) {
  netft::test::FakeSensor sensor{500.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 80ms;
  std::mutex delivered_mutex;
  std::vector<std::uint32_t> delivered;
  netft::Client client{config};
  client.start([&](const netft::Sample &sample) {
    std::lock_guard<std::mutex> lock(delivered_mutex);
    delivered.push_back(sample.rdt_sequence);
  });
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  sensor.send_record_now(100, 0, 0x00100000U);
  ASSERT_TRUE(wait_until([&] { return client.health().delivered_count == 1; }));
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 2, 500ms));
  sensor.send_record_now(1, 0, 0x00100004U);
  ASSERT_TRUE(wait_until([&] { return client.health().delivered_count == 2; }));

  client.stop();
  const auto health = client.health();
  const auto latest = client.latest_sample();
  std::lock_guard<std::mutex> lock(delivered_mutex);
  EXPECT_EQ(delivered, (std::vector<std::uint32_t>{100U, 1U}));
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->rdt_sequence, 1U);
  EXPECT_EQ(latest->configuration_revision, 1U);
  EXPECT_EQ(health.received_count, 2U);
  EXPECT_EQ(health.delivered_count, 2U);
  EXPECT_EQ(health.lost_count, 0U);
  EXPECT_EQ(health.duplicate_count, 0U);
  EXPECT_EQ(health.out_of_order_count, 0U);
  EXPECT_EQ(health.timeout_count, 1U);
  EXPECT_EQ(health.reconnect_count, 1U);
  ASSERT_TRUE(health.sensor_configuration);
  EXPECT_EQ(health.sensor_configuration->revision, 1U);
  EXPECT_EQ(sensor.http_request_count(), 2U);
}

TEST(ClientRecovery, ReconnectPreservesFtHistoryButClearsUnconfirmedRestartCandidate) {
  netft::test::FakeSensor sensor{500.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 80ms;
  std::mutex delivered_mutex;
  std::vector<std::uint32_t> delivered;
  netft::Client client{config};
  client.start([&](const netft::Sample &sample) {
    std::lock_guard<std::mutex> lock(delivered_mutex);
    delivered.push_back(sample.ft_sequence);
  });
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  sensor.send_record_now(100, 0, 0x00100000U);
  ASSERT_TRUE(wait_until([&] { return client.health().delivered_count == 1; }));
  sensor.send_record_now(101, 0, 2);
  ASSERT_TRUE(wait_until([&] { return client.health().received_count == 2; }));
  {
    const auto health = client.health();
    EXPECT_EQ(health.delivered_count, 1U);
    EXPECT_EQ(health.ft_backward_count, 1U);
    EXPECT_EQ(health.ft_restart_count, 0U);
  }

  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 2, 500ms));
  sensor.send_record_now(1, 0, 6);
  ASSERT_TRUE(wait_until([&] { return client.health().received_count == 3; }));
  {
    const auto health = client.health();
    EXPECT_EQ(health.delivered_count, 1U);
    EXPECT_EQ(health.ft_backward_count, 2U);
    EXPECT_EQ(health.ft_restart_count, 0U);
    EXPECT_EQ(health.out_of_order_count, 0U);
  }

  sensor.send_record_now(2, 0, 10);
  ASSERT_TRUE(wait_until([&] { return client.health().ft_restart_count == 1; }));
  client.stop();

  const auto health = client.health();
  const auto latest = client.latest_sample();
  std::lock_guard<std::mutex> lock(delivered_mutex);
  EXPECT_EQ(delivered, (std::vector<std::uint32_t>{0x00100000U, 10U}));
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->rdt_sequence, 2U);
  EXPECT_EQ(latest->ft_sequence, 10U);
  EXPECT_EQ(latest->configuration_revision, 1U);
  EXPECT_EQ(health.received_count, 4U);
  EXPECT_EQ(health.delivered_count, 2U);
  EXPECT_EQ(health.lost_count, 0U);
  EXPECT_EQ(health.duplicate_count, 0U);
  EXPECT_EQ(health.out_of_order_count, 0U);
  EXPECT_EQ(health.ft_stall_count, 0U);
  EXPECT_EQ(health.ft_backward_count, 2U);
  EXPECT_EQ(health.ft_restart_count, 1U);
  EXPECT_EQ(health.last_ft_progress, "restart");
  EXPECT_EQ(health.timeout_count, 1U);
  EXPECT_EQ(health.reconnect_count, 1U);
  EXPECT_EQ(health.calibration_change_count, 0U);
  ASSERT_TRUE(health.sensor_configuration);
  EXPECT_EQ(health.sensor_configuration->revision, 1U);
  EXPECT_EQ(sensor.http_request_count(), 2U);
}

TEST(ClientRecovery, SeriousStatusWinsWhenFtAlsoDiscontinuous) {
  netft::test::FakeSensor sensor{200.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.recovery_policy = netft::RecoveryPolicy::FailStop;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime));

  sensor.queue_record(1, 0, 100);
  sensor.queue_record(2, 0x80020000U, 100);
  sensor.resume();

  ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
  EXPECT_EQ(client.fault_code(), netft::FaultCode::SeriousStatus);
  EXPECT_EQ(client.health().ft_stall_count, 1U);
  EXPECT_EQ(client.health().reconnect_count, 0U);
  client.stop();
}

TEST(ClientRecovery, FailStopMapsTimeoutSocketAndConfigurationFailures) {
  {
    netft::test::FakeSensor sensor{200.0};
    sensor.pause();
    auto config = config_for(sensor);
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    config.receive_timeout = 50ms;
    netft::Client client{config};
    client.start([](const netft::Sample &) {});
    ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
    EXPECT_EQ(client.fault_code(), netft::FaultCode::Timeout);
    EXPECT_EQ(client.health().timeout_count, 1U);
    client.stop();
  }
  {
    netft::Config config;
    config.sensor_host = "not-a-loopback-host.invalid";
    config.calibration_override =
        netft::Calibration{1.0, 1.0, netft::ForceUnit::Newton, netft::TorqueUnit::NewtonMeter};
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    netft::Client client{config};
    client.start([](const netft::Sample &) {});
    ASSERT_TRUE(wait_until([&] { return client.faulted(); }, 2s));
    EXPECT_EQ(client.fault_code(), netft::FaultCode::Socket);
    client.stop();
  }
  {
    netft::test::FakeSensor sensor;
    sensor.set_xml_configuration("<netft>");
    auto config = config_for(sensor);
    config.recovery_policy = netft::RecoveryPolicy::FailStop;
    netft::Client client{config};
    client.start([](const netft::Sample &) {});
    ASSERT_TRUE(wait_until([&] { return client.faulted(); }));
    EXPECT_EQ(client.fault_code(), netft::FaultCode::SensorConfiguration);
    EXPECT_EQ(start_count(sensor), 0U);
    client.stop();
  }
}

TEST(ClientRecovery, ReconnectBackoffDoublesCapsAndResetsAfterValidRecord) {
  netft::test::FakeSensor sensor{500.0};
  auto config = config_for(sensor);
  config.receive_timeout = 40ms;
  config.reconnect_initial_delay = 20ms;
  config.reconnect_max_delay = 80ms;
  std::atomic<unsigned> delivered{};
  netft::Client client{config};
  client.start([&](const netft::Sample &) { ++delivered; });
  ASSERT_TRUE(wait_until([&] { return delivered.load() > 0; }));

  sensor.pause();
  ASSERT_TRUE(wait_until([&] { return start_times(sensor).size() >= 5; }, 1s));
  const auto starts = start_times(sensor);
  ASSERT_GE(starts.size(), 5U);
  const auto interval = [&](const std::size_t index) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(starts[index] - starts[index - 1]);
  };
  EXPECT_GE(interval(1), 50ms);
  EXPECT_LT(interval(1), 90ms);
  EXPECT_GE(interval(2), 70ms);
  EXPECT_LT(interval(2), 110ms);
  EXPECT_GE(interval(3), 105ms);
  EXPECT_LT(interval(3), 150ms);
  EXPECT_GE(interval(4), 105ms);
  EXPECT_LT(interval(4), 150ms);

  const auto recovery_baseline = delivered.load();
  sensor.resume();
  ASSERT_TRUE(wait_until([&] { return delivered.load() > recovery_baseline; }, 500ms));
  sensor.pause();
  const auto paused_at = std::chrono::steady_clock::now();
  const auto prior_starts = start_times(sensor).size();
  ASSERT_TRUE(wait_until([&] { return start_times(sensor).size() > prior_starts; }, 500ms));
  const auto reset_start = start_times(sensor).back();
  EXPECT_GE(reset_start - paused_at, 45ms);
  EXPECT_LT(reset_start - paused_at, 95ms);
  EXPECT_FALSE(client.faulted());
  client.stop();
}

TEST(ClientRecovery, StopInterruptsReconnectBackoff) {
  netft::test::FakeSensor sensor{100.0};
  sensor.pause();
  auto config = config_for(sensor);
  config.receive_timeout = 40ms;
  config.reconnect_initial_delay = 2s;
  config.reconnect_max_delay = 2s;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(
      wait_until([&] { return client.health().state == netft::ClientState::Backoff; }, 500ms));

  const auto stop_started = std::chrono::steady_clock::now();
  client.stop();
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;

  EXPECT_LT(stop_elapsed, 500ms);
  EXPECT_EQ(client.health().state, netft::ClientState::Stopped);
}

TEST(ClientRecovery, ReconnectRediscoversChangedCalibrationAndIncrementsRevision) {
  netft::test::FakeSensor sensor{200.0};
  auto config = config_for(sensor);
  config.receive_timeout = 60ms;
  netft::Client client{config};
  client.start([](const netft::Sample &) {});
  ASSERT_TRUE(client.wait_for_first_sample(500ms));
  ASSERT_EQ(client.latest_sample()->configuration_revision, 1U);

  sensor.set_xml_configuration(kChangedConfiguration);
  sensor.pause();
  ASSERT_TRUE(sensor.wait_for_http_request(2, 500ms));
  ASSERT_TRUE(sensor.wait_for_command(netft::detail::Command::StartRealtime, 2, 500ms));
  sensor.resume();
  ASSERT_TRUE(wait_until([&] {
    const auto sample = client.latest_sample();
    return sample && sample->configuration_revision == 2;
  }));

  const auto sample = client.latest_sample();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->torque_unit, netft::TorqueUnit::NewtonMillimeter);
  EXPECT_EQ(sample->configuration_revision, 2U);
  const auto health = client.health();
  ASSERT_TRUE(health.sensor_configuration);
  EXPECT_EQ(health.sensor_configuration->revision, 2U);
  EXPECT_EQ(health.calibration_change_count, 1U);
  client.stop();
}

TEST(ClientRecovery, InvalidReplacementConfigurationCannotStartNewRdtSession) {
  for (const std::string replacement : {
           "<netft>",
           R"xml(<netft><prodname>Fake</prodname><cfgcpf>1000000</cfgcpf>
<cfgcpt>1000</cfgcpt><scfgfu>N</scfgfu><scfgtu>mystery</scfgtu></netft>)xml",
       }) {
    netft::test::FakeSensor sensor{200.0};
    auto config = config_for(sensor);
    config.receive_timeout = 60ms;
    config.reconnect_initial_delay = 10ms;
    config.reconnect_max_delay = 10ms;
    netft::Client client{config};
    client.start([](const netft::Sample &) {});
    ASSERT_TRUE(client.wait_for_first_sample(500ms));
    ASSERT_EQ(start_count(sensor), 1U);

    sensor.set_xml_configuration(replacement);
    sensor.pause();
    ASSERT_TRUE(sensor.wait_for_http_request(3, 500ms));
    EXPECT_EQ(start_count(sensor), 1U);
    EXPECT_EQ(client.latest_sample()->configuration_revision, 1U);
    EXPECT_EQ(client.health().calibration_change_count, 0U);
    EXPECT_FALSE(client.faulted());
    client.stop();
  }
}

} // namespace
