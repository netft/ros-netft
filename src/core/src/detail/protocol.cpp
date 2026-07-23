#include "detail/protocol.hpp"

#include <string>

namespace netft::detail {
namespace {

std::uint32_t read_u32(const std::uint8_t *data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

std::int32_t read_i32(const std::uint8_t *data) {
  const std::uint32_t value = read_u32(data);
  if (value <= 0x7fffffffU) {
    return static_cast<std::int32_t>(value);
  }
  return -static_cast<std::int32_t>(~value) - 1;
}

} // namespace

std::array<std::uint8_t, 8> encode_request(const Command command, const std::uint32_t count) {
  const auto value = static_cast<std::uint16_t>(command);
  return {{0x12, 0x34, static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value),
           static_cast<std::uint8_t>(count >> 24U), static_cast<std::uint8_t>(count >> 16U),
           static_cast<std::uint8_t>(count >> 8U), static_cast<std::uint8_t>(count)}};
}

RawRecord decode_record(const std::uint8_t *data, const std::size_t size) {
  if (size != 36) {
    throw ProtocolError{"RDT record must be exactly 36 bytes, received " + std::to_string(size)};
  }
  if (data == nullptr) {
    throw ProtocolError{"RDT record data must not be null"};
  }
  return {read_u32(data),      read_u32(data + 4),  read_u32(data + 8),
          read_i32(data + 12), read_i32(data + 16), read_i32(data + 20),
          read_i32(data + 24), read_i32(data + 28), read_i32(data + 32)};
}

} // namespace netft::detail
