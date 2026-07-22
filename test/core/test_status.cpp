#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "netft/status.hpp"

namespace {

TEST(Status, ClassifiesHealthConditionAndError) {
  EXPECT_EQ(netft::classify_status(0), netft::StatusSeverity::Ok);
  EXPECT_EQ(netft::classify_status(0x80010000U), netft::StatusSeverity::Warn);
  EXPECT_EQ(netft::classify_status(0x80020000U), netft::StatusSeverity::Error);
  EXPECT_EQ(netft::classify_status(0x00010000U), netft::StatusSeverity::Error);
}

TEST(Status, ProducesDistinctNonemptyDescriptionsForDefinedBits) {
  constexpr std::array<std::uint32_t, 30> masks{
      0x80000000U, 0x40000000U, 0x20000000U, 0x10000000U, 0x08000000U, 0x04000000U,
      0x02000000U, 0x01000000U, 0x00800000U, 0x00400000U, 0x00200000U, 0x00100000U,
      0x00080000U, 0x00040000U, 0x00020000U, 0x00010000U, 0x00004000U, 0x00002000U,
      0x00001000U, 0x00000800U, 0x00000400U, 0x00000200U, 0x00000100U, 0x00000080U,
      0x00000040U, 0x00000020U, 0x00000010U, 0x00000008U, 0x00000004U, 0x00000002U};
  std::unordered_set<std::string> descriptions;
  descriptions.insert(netft::decode_status(0));
  for (const auto mask : masks) {
    const auto description = netft::decode_status(mask);
    EXPECT_FALSE(description.empty());
    EXPECT_TRUE(descriptions.insert(description).second);
  }
}

} // namespace
