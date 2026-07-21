#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "netft_driver/protocol.hpp"

namespace {

using netft_driver::Command;
using netft_driver::ProtocolError;
using netft_driver::decode_record;
using netft_driver::encode_request;

TEST(Protocol, EncodesRequestsInNetworkByteOrder)
{
  EXPECT_EQ(encode_request(Command::StartRealtime, 0),
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

TEST(Protocol, DecodesUnsignedHeadersAndSignedAxes)
{
  const std::array<std::uint8_t, 36> payload{
    0xff, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0x80, 0x02, 0, 0,
    0xff, 0xff, 0xff, 0xff, 0, 0, 0, 2, 0xff, 0xff, 0xff, 0xfd,
    0, 0, 0, 4, 0xff, 0xff, 0xff, 0xfb, 0, 0, 0, 6};
  const auto record = decode_record(payload.data(), payload.size());
  EXPECT_EQ(record.rdt_sequence, 0xffffffffU);
  EXPECT_EQ(record.ft_sequence, 0x80000000U);
  EXPECT_EQ(record.status, 0x80020000U);
  EXPECT_EQ(record.fx, -1); EXPECT_EQ(record.fy, 2); EXPECT_EQ(record.fz, -3);
  EXPECT_EQ(record.tx, 4); EXPECT_EQ(record.ty, -5); EXPECT_EQ(record.tz, 6);
}

TEST(Protocol, DecodesTwoComplementAxisBoundariesWithoutNarrowing)
{
  const std::array<std::uint8_t, 36> payload{
    0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3,
    0x80, 0, 0, 0, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0, 0, 0, 0, 0x80, 0, 0, 1, 0x7f, 0xff, 0xff, 0xfe};
  const auto record = decode_record(payload.data(), payload.size());
  EXPECT_EQ(record.fx, std::numeric_limits<std::int32_t>::min());
  EXPECT_EQ(record.fy, std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(record.fz, -1);
  EXPECT_EQ(record.tx, 0);
  EXPECT_EQ(record.ty, std::numeric_limits<std::int32_t>::min() + 1);
  EXPECT_EQ(record.tz, std::numeric_limits<std::int32_t>::max() - 1);
}

TEST(Protocol, DoesNotUseImplementationDefinedUnsignedToSignedConversion)
{
  std::ifstream source{NETFT_PROTOCOL_SOURCE_PATH};
  const std::string contents{std::istreambuf_iterator<char>{source}, {}};
  EXPECT_EQ(contents.find("static_cast<std::int32_t>(read_u32"), std::string::npos);
}

class InvalidDatagramSize : public ::testing::TestWithParam<std::size_t> {};
TEST_P(InvalidDatagramSize, RejectsEveryNonRdtLength)
{
  const std::vector<std::uint8_t> payload(GetParam());
  try {
    static_cast<void>(decode_record(payload.data(), payload.size()));
    FAIL() << "malformed payload was accepted";
  } catch (const ProtocolError & error) {
    EXPECT_NE(std::string{error.what()}.find("exactly 36 bytes"), std::string::npos);
  }
}
INSTANTIATE_TEST_SUITE_P(Protocol, InvalidDatagramSize, ::testing::Values(0, 7, 35, 37, 72));

}  // namespace
