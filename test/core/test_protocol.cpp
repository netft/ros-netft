#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "detail/protocol.hpp"

namespace {

using netft::detail::Command;
using netft::detail::decode_record;
using netft::detail::encode_request;
using netft::detail::ProtocolError;

TEST(Protocol, EncodesRequestsInNetworkByteOrder) {
  EXPECT_EQ(encode_request(Command::StartRealtime),
            (std::array<std::uint8_t, 8>{0x12, 0x34, 0x00, 0x02, 0, 0, 0, 0}));
  EXPECT_EQ(encode_request(Command::StopStreaming, 7),
            (std::array<std::uint8_t, 8>{0x12, 0x34, 0x00, 0x00, 0, 0, 0, 7}));
  EXPECT_EQ(encode_request(Command::StartBuffered, 7),
            (std::array<std::uint8_t, 8>{0x12, 0x34, 0x00, 0x03, 0, 0, 0, 7}));
  EXPECT_EQ(encode_request(Command::ResetConditionLatch, 7),
            (std::array<std::uint8_t, 8>{0x12, 0x34, 0x00, 0x41, 0, 0, 0, 7}));
  EXPECT_EQ(encode_request(Command::SetSoftwareBias, 7),
            (std::array<std::uint8_t, 8>{0x12, 0x34, 0x00, 0x42, 0, 0, 0, 7}));
}

TEST(Protocol, DecodesUnsignedHeadersAndSignedAxes) {
  const std::array<std::uint8_t, 36> payload{0xff, 0xff, 0xff, 0xff, 0x80, 0,    0,    0, 0x80,
                                             0x02, 0,    0,    0xff, 0xff, 0xff, 0xff, 0, 0,
                                             0,    2,    0xff, 0xff, 0xff, 0xfd, 0,    0, 0,
                                             4,    0xff, 0xff, 0xff, 0xfb, 0,    0,    0, 6};
  const auto record = decode_record(payload.data(), payload.size());
  EXPECT_EQ(record.rdt_sequence, 0xffffffffU);
  EXPECT_EQ(record.ft_sequence, 0x80000000U);
  EXPECT_EQ(record.status, 0x80020000U);
  EXPECT_EQ(record.fx, -1);
  EXPECT_EQ(record.fy, 2);
  EXPECT_EQ(record.fz, -3);
  EXPECT_EQ(record.tx, 4);
  EXPECT_EQ(record.ty, -5);
  EXPECT_EQ(record.tz, 6);
}

TEST(Protocol, DecodesTwoComplementAxisBoundaries) {
  const std::array<std::uint8_t, 36> payload{
      0,    0,    0,    1,    0,    0,    0, 2, 0, 0, 0,    3, 0x80, 0, 0,    0,    0x7f, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0x80, 0, 0,    1, 0x7f, 0xff, 0xff, 0xfe};
  const auto record = decode_record(payload.data(), payload.size());
  EXPECT_EQ(record.fx, std::numeric_limits<std::int32_t>::min());
  EXPECT_EQ(record.fy, std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(record.fz, -1);
  EXPECT_EQ(record.tx, 0);
  EXPECT_EQ(record.ty, std::numeric_limits<std::int32_t>::min() + 1);
  EXPECT_EQ(record.tz, std::numeric_limits<std::int32_t>::max() - 1);
}

TEST(Protocol, RejectsNullAndMalformedRecords) {
  const std::array<std::uint8_t, 35> payload{};
  EXPECT_THROW(decode_record(nullptr, 36), ProtocolError);
  EXPECT_THROW(decode_record(payload.data(), 35), ProtocolError);
}

} // namespace
