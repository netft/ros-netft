#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
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

TEST(Status, DecodesEveryDefinedActiveBitDistinctly)
{
  constexpr std::array<std::uint32_t, 30> masks{
    0x80000000U, 0x40000000U, 0x20000000U, 0x10000000U, 0x08000000U,
    0x04000000U, 0x02000000U, 0x01000000U, 0x00800000U, 0x00400000U,
    0x00200000U, 0x00100000U, 0x00080000U, 0x00040000U, 0x00020000U,
    0x00010000U, 0x00004000U, 0x00002000U, 0x00001000U, 0x00000800U,
    0x00000400U, 0x00000200U, 0x00000100U, 0x00000080U, 0x00000040U,
    0x00000020U, 0x00000010U, 0x00000008U, 0x00000004U, 0x00000002U};
  std::unordered_set<std::string> descriptions;
  for (const auto mask : masks) {
    const auto description = decode_status(mask);
    EXPECT_FALSE(description.empty());
    descriptions.insert(description);
  }
  EXPECT_EQ(descriptions.size(), masks.size());
}
TEST(Status, ReportsNonemptyDescriptionWhenNoBitsSet) { EXPECT_FALSE(decode_status(0).empty()); }

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
