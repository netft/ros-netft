#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "netft_driver/status.hpp"

namespace {
using namespace netft_driver;

TEST(Status, ClassifiesHealthConditionAndError)
{
  EXPECT_EQ(classify_status(0), DiagnosticSeverity::Ok);
  EXPECT_EQ(classify_status(0x80010000U), DiagnosticSeverity::Warn);
  EXPECT_EQ(classify_status(0x80020000U), DiagnosticSeverity::Error);
  EXPECT_EQ(classify_status(0x00010000U), DiagnosticSeverity::Error);
}

class StatusBit : public ::testing::TestWithParam<std::pair<std::uint32_t, const char *>> {};
TEST_P(StatusBit, DecodesEveryDefinedActiveBit)
{
  const auto [mask, name] = GetParam();
  EXPECT_NE(decode_status(mask).find(name), std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(Status, StatusBit, ::testing::Values(
  std::make_pair(0x80000000U, "error summary"), std::make_pair(0x40000000U, "CPU or RAM error"),
  std::make_pair(0x20000000U, "digital board error"), std::make_pair(0x10000000U, "analog board error"),
  std::make_pair(0x08000000U, "serial link communication error"), std::make_pair(0x04000000U, "program memory verification error"),
  std::make_pair(0x02000000U, "halted due to configuration errors"), std::make_pair(0x01000000U, "settings validation error"),
  std::make_pair(0x00800000U, "configuration incompatible with calibration"), std::make_pair(0x00400000U, "network communication failure"),
  std::make_pair(0x00200000U, "CAN communication error"), std::make_pair(0x00100000U, "RDT communication error"),
  std::make_pair(0x00080000U, "EtherNet/IP protocol failure"), std::make_pair(0x00040000U, "DeviceNet protocol failure"),
  std::make_pair(0x00020000U, "transducer saturation or A/D error"), std::make_pair(0x00010000U, "monitor condition latched"),
  std::make_pair(0x00004000U, "watchdog timeout error"), std::make_pair(0x00002000U, "stack check error"),
  std::make_pair(0x00001000U, "serial EEPROM I2C failure"), std::make_pair(0x00000800U, "serial flash SPI failure"),
  std::make_pair(0x00000400U, "analog board watchdog timeout"), std::make_pair(0x00000200U, "excessive strain gage excitation current"),
  std::make_pair(0x00000100U, "insufficient strain gage excitation current"), std::make_pair(0x00000080U, "artificial analog ground out of range"),
  std::make_pair(0x00000040U, "analog board power supply too high"), std::make_pair(0x00000020U, "analog board power supply too low"),
  std::make_pair(0x00000010U, "serial link data unavailable"), std::make_pair(0x00000008U, "reference voltage or power monitoring error"),
  std::make_pair(0x00000004U, "internal temperature error"), std::make_pair(0x00000002U, "HTTP protocol failure")));
TEST(Status, ReportsHealthyWhenNoBitsSet) { EXPECT_EQ(decode_status(0), "healthy"); }

TEST(RdtSequence, HandlesFirstContiguousRolloverGapsAndOrdering)
{
  RdtSequenceTracker tracker;
  EXPECT_EQ(tracker.observe(0xfffffffeU).kind, SequenceKind::First);
  EXPECT_EQ(tracker.observe(0xffffffffU).kind, SequenceKind::Contiguous);
  EXPECT_EQ(tracker.observe(0).kind, SequenceKind::Contiguous);
  EXPECT_EQ(tracker.observe(4).gap, 3U);
  EXPECT_EQ(tracker.observe(4).kind, SequenceKind::Duplicate);
  EXPECT_EQ(tracker.observe(3).kind, SequenceKind::OutOfOrder);
  EXPECT_EQ(tracker.observe(5).kind, SequenceKind::Contiguous);
  tracker.reset(); EXPECT_EQ(tracker.observe(1).kind, SequenceKind::First);
}

TEST(FtSequence, TracksForwardStallBackwardAndConfirmedRestart)
{
  FtSequenceTracker tracker;
  EXPECT_EQ(tracker.observe(0xfffffff0U).kind, FtSequenceKind::First);
  EXPECT_EQ(tracker.observe(0xfffffffeU).kind, FtSequenceKind::Forward);
  EXPECT_EQ(tracker.observe(7).kind, FtSequenceKind::Forward);
  EXPECT_EQ(tracker.observe(7).kind, FtSequenceKind::Stall);
  EXPECT_EQ(tracker.observe(6).kind, FtSequenceKind::Backward);
  tracker.observe(0x00100000U); tracker.begin_session();
  EXPECT_EQ(tracker.observe(2).kind, FtSequenceKind::Backward);
  EXPECT_EQ(tracker.observe(6).kind, FtSequenceKind::Restart);
  EXPECT_EQ(tracker.observe(10).kind, FtSequenceKind::Forward);
}

TEST(FtSequence, ClearsCandidatesAndRejectsHighWindowNearProgress)
{
  FtSequenceTracker tracker; const auto baseline = 0x00100000U;
  tracker.observe(baseline); EXPECT_EQ(tracker.observe(2).kind, FtSequenceKind::Backward);
  tracker.begin_session(); EXPECT_EQ(tracker.observe(6).kind, FtSequenceKind::Backward);
  EXPECT_EQ(tracker.observe(baseline + 4).kind, FtSequenceKind::Forward);
  EXPECT_EQ(tracker.observe(10).kind, FtSequenceKind::Backward);
  EXPECT_EQ(tracker.observe(14).kind, FtSequenceKind::Restart);
  FtSequenceTracker high; high.observe(0x70000000U);
  EXPECT_EQ(high.observe(0x6fffff00U).kind, FtSequenceKind::Backward);
  EXPECT_EQ(high.observe(0x6fffff04U).kind, FtSequenceKind::Backward);
  EXPECT_EQ(high.last(), 0x70000000U);
}

}  // namespace
