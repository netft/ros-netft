#include "netft/status.hpp"

#include <array>
#include <utility>

namespace netft {
namespace {

constexpr std::array<std::pair<std::uint32_t, const char *>, 30> kStatusBits{{
    {0x80000000U, "error summary"},
    {0x40000000U, "CPU or RAM error"},
    {0x20000000U, "digital board error"},
    {0x10000000U, "analog board error"},
    {0x08000000U, "serial link communication error"},
    {0x04000000U, "program memory verification error"},
    {0x02000000U, "halted due to configuration errors"},
    {0x01000000U, "settings validation error"},
    {0x00800000U, "configuration incompatible with calibration"},
    {0x00400000U, "network communication failure"},
    {0x00200000U, "CAN communication error"},
    {0x00100000U, "RDT communication error"},
    {0x00080000U, "EtherNet/IP protocol failure"},
    {0x00040000U, "DeviceNet protocol failure"},
    {0x00020000U, "transducer saturation or A/D error"},
    {0x00010000U, "monitor condition latched"},
    {0x00004000U, "watchdog timeout error"},
    {0x00002000U, "stack check error"},
    {0x00001000U, "serial EEPROM I2C failure"},
    {0x00000800U, "serial flash SPI failure"},
    {0x00000400U, "analog board watchdog timeout"},
    {0x00000200U, "excessive strain gage excitation current"},
    {0x00000100U, "insufficient strain gage excitation current"},
    {0x00000080U, "artificial analog ground out of range"},
    {0x00000040U, "analog board power supply too high"},
    {0x00000020U, "analog board power supply too low"},
    {0x00000010U, "serial link data unavailable"},
    {0x00000008U, "reference voltage or power monitoring error"},
    {0x00000004U, "internal temperature error"},
    {0x00000002U, "HTTP protocol failure"},
}};

} // namespace

StatusSeverity classify_status(const std::uint32_t status) noexcept {
  if (status == 0) {
    return StatusSeverity::Ok;
  }
  return status == 0x80010000U ? StatusSeverity::Warn : StatusSeverity::Error;
}

std::string decode_status(const std::uint32_t status) {
  if (status == 0) {
    return "healthy";
  }

  std::string result;
  for (const auto &[mask, name] : kStatusBits) {
    if ((status & mask) != 0) {
      if (!result.empty()) {
        result += ", ";
      }
      result += name;
    }
  }
  return result;
}

} // namespace netft
