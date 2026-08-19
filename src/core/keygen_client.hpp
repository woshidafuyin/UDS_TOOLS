#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace uds {
std::vector<std::uint8_t> generate_key_x86(const std::filesystem::path& broker,
                                           const std::filesystem::path& dll,
                                           std::span<const std::uint8_t> seed,
                                           unsigned security_level = 0x11,
                                           const std::wstring& variant = L"chuneng");
} // namespace uds
