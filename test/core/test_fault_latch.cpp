#include <gtest/gtest.h>

#include <mutex>
#include <string>

#include "detail/fault_latch.hpp"

namespace {

TEST(FaultLatch, LaterPublicationCannotReplaceFirstCodeOrMessage) {
  std::mutex data_mutex;
  netft::HealthSnapshot health;
  netft::detail::FaultLatch latch;
  const std::string first_error{__func__};
  const std::string later_error = first_error + std::to_string(__LINE__);

  EXPECT_TRUE(latch.publish(netft::FaultCode::Timeout, first_error, data_mutex, health));
  EXPECT_FALSE(latch.publish(netft::FaultCode::Socket, later_error, data_mutex, health));

  EXPECT_TRUE(latch.faulted());
  EXPECT_EQ(latch.code(), netft::FaultCode::Timeout);
  std::lock_guard<std::mutex> data_lock(data_mutex);
  EXPECT_EQ(health.state, netft::ClientState::Faulted);
  EXPECT_EQ(health.fault_code, netft::FaultCode::Timeout);
  EXPECT_EQ(health.last_error, first_error);
}

} // namespace
