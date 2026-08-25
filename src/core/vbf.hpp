#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace uds {

struct VbfBlock {
  std::uint32_t address{};
  std::vector<std::uint8_t> data;
  std::uint16_t crc16{};
};

struct VbfEraseRange {
  std::uint32_t address{};
  std::uint32_t length{};
};

struct VbfFile {
  std::filesystem::path source;
  std::string sw_part_number;
  std::string sw_version;
  std::string sw_part_type;
  std::uint8_t data_format_identifier{};
  std::uint32_t file_checksum{};
  std::uint32_t call_address{};
  bool has_call_address{};
  std::uint32_t ecu_address{};
  bool has_ecu_address{};
  std::vector<VbfEraseRange> erase_ranges;
  std::vector<std::uint8_t> signature_dev;
  std::vector<std::uint8_t> signature_prod;
  // Backward-compatible preferred signature. Development is preferred when
  // present; otherwise this aliases the production signature. Project policy
  // still decides which signature is acceptable for an actual download.
  std::vector<std::uint8_t> signature;
  std::vector<VbfBlock> blocks;
  bool block_crc16_verified{true};
};

std::uint16_t vbf_crc16_ccitt_false(
    const std::vector<std::uint8_t>& data) noexcept;
std::uint32_t vbf_crc32(const std::vector<std::uint8_t>& data) noexcept;
VbfFile load_vbf(const std::filesystem::path& path);

} // namespace uds
