#include <gtest/gtest.h>

#include <cstdint>

#include "detail/sequence.hpp"

namespace {

using netft::detail::FtSequenceKind;
using netft::detail::FtSequenceTracker;
using netft::detail::RdtSequenceTracker;
using netft::detail::SequenceKind;

TEST(RdtSequence, HandlesFirstContiguousRolloverGapsAndOrdering) {
  RdtSequenceTracker tracker;
  EXPECT_EQ(tracker.observe(0xfffffffeU).kind, SequenceKind::First);
  EXPECT_EQ(tracker.observe(0xffffffffU).kind, SequenceKind::Contiguous);
  EXPECT_EQ(tracker.observe(0).kind, SequenceKind::Contiguous);
  EXPECT_EQ(tracker.observe(4).gap, 3U);
  EXPECT_EQ(tracker.observe(4).kind, SequenceKind::Duplicate);
  EXPECT_EQ(tracker.observe(3).kind, SequenceKind::OutOfOrder);
  EXPECT_EQ(tracker.observe(5).kind, SequenceKind::Contiguous);
  tracker.reset();
  EXPECT_EQ(tracker.observe(1).kind, SequenceKind::First);
}

TEST(FtSequence, TracksForwardStallBackwardAndConfirmedRestart) {
  FtSequenceTracker tracker;
  EXPECT_EQ(tracker.observe(0xfffffff0U).kind, FtSequenceKind::First);
  EXPECT_EQ(tracker.observe(0xfffffffeU).kind, FtSequenceKind::Forward);
  EXPECT_EQ(tracker.observe(7).kind, FtSequenceKind::Forward);
  EXPECT_EQ(tracker.observe(7).kind, FtSequenceKind::Stall);
  EXPECT_EQ(tracker.observe(6).kind, FtSequenceKind::Backward);
  tracker.observe(0x00100000U);
  tracker.begin_session();
  EXPECT_EQ(tracker.observe(2).kind, FtSequenceKind::Backward);
  EXPECT_EQ(tracker.observe(6).kind, FtSequenceKind::Restart);
  EXPECT_EQ(tracker.observe(10).kind, FtSequenceKind::Forward);
}

TEST(FtSequence, ClearsCandidatesAndRejectsHighWindowNearProgress) {
  FtSequenceTracker tracker;
  constexpr auto baseline = 0x00100000U;
  tracker.observe(baseline);
  EXPECT_EQ(tracker.observe(2).kind, FtSequenceKind::Backward);
  tracker.begin_session();
  EXPECT_EQ(tracker.observe(6).kind, FtSequenceKind::Backward);
  EXPECT_EQ(tracker.observe(baseline + 4).kind, FtSequenceKind::Forward);
  EXPECT_EQ(tracker.observe(10).kind, FtSequenceKind::Backward);
  EXPECT_EQ(tracker.observe(14).kind, FtSequenceKind::Restart);

  FtSequenceTracker high;
  high.observe(0x70000000U);
  EXPECT_EQ(high.observe(0x6fffff00U).kind, FtSequenceKind::Backward);
  EXPECT_EQ(high.observe(0x6fffff04U).kind, FtSequenceKind::Backward);
  EXPECT_EQ(high.last(), 0x70000000U);
}

} // namespace
