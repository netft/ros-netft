#pragma once

#include <cstdint>
#include <string>

#include "netft/export.hpp"

namespace netft {

enum class StatusSeverity { Ok, Warn, Error };

NETFT_API StatusSeverity classify_status(std::uint32_t status) noexcept;
NETFT_API std::string decode_status(std::uint32_t status);

} // namespace netft
