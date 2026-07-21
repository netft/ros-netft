#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>

#include "netft_driver/types.hpp"

namespace {

TEST(ClientConfig, KeepsPublishedDefaults)
{
  const netft_driver::ClientConfig config{};
  EXPECT_EQ(config.sensor_host, "192.168.31.100");
  EXPECT_EQ(config.sensor_port, 49152);
  EXPECT_DOUBLE_EQ(config.counts_per_force, 1000000.0);
  EXPECT_DOUBLE_EQ(config.counts_per_torque, 1000000.0);
  EXPECT_DOUBLE_EQ(config.publish_rate, 0.0);
  EXPECT_DOUBLE_EQ(config.receive_timeout.count(), 0.1);
  EXPECT_DOUBLE_EQ(config.reconnect_initial_delay.count(), 0.25);
  EXPECT_DOUBLE_EQ(config.reconnect_max_delay.count(), 5.0);
  EXPECT_FALSE(config.publish_on_error);
  EXPECT_EQ(config.recovery_policy, netft_driver::RecoveryPolicy::Reconnect);
}

TEST(ClientConfig, RejectsNonFiniteScale)
{
  netft_driver::ClientConfig config;
  config.counts_per_force = std::numeric_limits<double>::infinity();
  EXPECT_THROW(netft_driver::validate(config), std::invalid_argument);
}

struct InvalidConfigCase {
  const char * name;
  void (*mutate)(netft_driver::ClientConfig &);
  const char * field;
};

class InvalidClientConfig : public ::testing::TestWithParam<InvalidConfigCase> {};

TEST_P(InvalidClientConfig, NamesTheInvalidField)
{
  netft_driver::ClientConfig config;
  const auto & test_case = GetParam();
  test_case.mutate(config);

  try {
    netft_driver::validate(config);
    FAIL() << test_case.name << " was accepted";
  } catch (const std::invalid_argument & error) {
    EXPECT_NE(std::string{error.what()}.find(test_case.field), std::string::npos);
  }
}

INSTANTIATE_TEST_SUITE_P(
  Validation,
  InvalidClientConfig,
  ::testing::Values(
    InvalidConfigCase{"empty_host", [](auto & config) { config.sensor_host.clear(); }, "sensor_host"},
    InvalidConfigCase{"blank_host", [](auto & config) { config.sensor_host = "  "; }, "sensor_host"},
    InvalidConfigCase{"zero_port", [](auto & config) { config.sensor_port = 0; }, "sensor_port"},
    InvalidConfigCase{"zero_force_scale", [](auto & config) { config.counts_per_force = 0.0; }, "counts_per_force"},
    InvalidConfigCase{"nonfinite_force_scale", [](auto & config) { config.counts_per_force = std::numeric_limits<double>::quiet_NaN(); }, "counts_per_force"},
    InvalidConfigCase{"zero_torque_scale", [](auto & config) { config.counts_per_torque = 0.0; }, "counts_per_torque"},
    InvalidConfigCase{"nonfinite_torque_scale", [](auto & config) { config.counts_per_torque = std::numeric_limits<double>::quiet_NaN(); }, "counts_per_torque"},
    InvalidConfigCase{"negative_publish_rate", [](auto & config) { config.publish_rate = -0.1; }, "publish_rate"},
    InvalidConfigCase{"nonfinite_publish_rate", [](auto & config) { config.publish_rate = std::numeric_limits<double>::infinity(); }, "publish_rate"},
    InvalidConfigCase{"zero_receive_timeout", [](auto & config) { config.receive_timeout = std::chrono::duration<double>{0.0}; }, "receive_timeout"},
    InvalidConfigCase{"negative_receive_timeout", [](auto & config) { config.receive_timeout = std::chrono::duration<double>{-0.1}; }, "receive_timeout"},
    InvalidConfigCase{"nonfinite_receive_timeout", [](auto & config) { config.receive_timeout = std::chrono::duration<double>{std::numeric_limits<double>::infinity()}; }, "receive_timeout"},
    InvalidConfigCase{"zero_initial_delay", [](auto & config) { config.reconnect_initial_delay = std::chrono::duration<double>{0.0}; }, "reconnect_initial_delay"},
    InvalidConfigCase{"negative_initial_delay", [](auto & config) { config.reconnect_initial_delay = std::chrono::duration<double>{-0.25}; }, "reconnect_initial_delay"},
    InvalidConfigCase{"nonfinite_initial_delay", [](auto & config) { config.reconnect_initial_delay = std::chrono::duration<double>{std::numeric_limits<double>::infinity()}; }, "reconnect_initial_delay"},
    InvalidConfigCase{"zero_max_delay", [](auto & config) { config.reconnect_max_delay = std::chrono::duration<double>{0.0}; }, "reconnect_max_delay"},
    InvalidConfigCase{"negative_max_delay", [](auto & config) { config.reconnect_max_delay = std::chrono::duration<double>{-5.0}; }, "reconnect_max_delay"},
    InvalidConfigCase{"nonfinite_max_delay", [](auto & config) { config.reconnect_max_delay = std::chrono::duration<double>{std::numeric_limits<double>::infinity()}; }, "reconnect_max_delay"},
    InvalidConfigCase{"maximum_below_initial", [](auto & config) { config.reconnect_initial_delay = std::chrono::duration<double>{2.0}; config.reconnect_max_delay = std::chrono::duration<double>{1.0}; }, "reconnect_max_delay"}));

}  // namespace
