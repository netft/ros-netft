#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace netft::detail {

enum class Command : std::uint16_t {
  StopStreaming = 0x0000,
  StartRealtime = 0x0002,
  StartBuffered = 0x0003,
  ResetConditionLatch = 0x0041,
  SetSoftwareBias = 0x0042,
};

struct RawRecord {
  std::uint32_t rdt_sequence{}, ft_sequence{}, status{};
  std::int32_t fx{}, fy{}, fz{}, tx{}, ty{}, tz{};
};

class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

std::array<std::uint8_t, 8> encode_request(Command command, std::uint32_t count = 0);
RawRecord decode_record(const std::uint8_t *data, std::size_t size);

} // namespace netft::detail
