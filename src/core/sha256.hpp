#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace uds {

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);

} // namespace uds
