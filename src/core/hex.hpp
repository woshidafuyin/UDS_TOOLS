#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uds {
std::string to_hex(std::span<const std::uint8_t> bytes, bool spaces = true);
std::vector<std::uint8_t> from_hex(const std::string& text);
} // namespace uds
