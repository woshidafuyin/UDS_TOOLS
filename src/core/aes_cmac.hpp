#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace uds {
using Aes128Block = std::array<std::uint8_t, 16>;

// RFC 4493. The message is authenticated as bytes; no integer byte swapping.
Aes128Block aes128_cmac(std::span<const std::uint8_t, 16> key,
                       std::span<const std::uint8_t> message);

// CHKEY1 + Windows DPAPI blob, bound to the current Windows user.
// No OEM secret is compiled into an executable or copied to a release package.
Aes128Block load_protected_aes128_key(const std::filesystem::path& path);
std::filesystem::path default_oem_key_path();
} // namespace uds
