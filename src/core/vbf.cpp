#include "core/vbf.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>

namespace uds {
namespace {

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::uint16_t read_be16(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
      static_cast<std::uint16_t>(bytes[offset + 1U]));
}

std::string required_match(const std::string& header,
                           const std::regex& expression,
                           const char* field) {
  std::smatch match;
  if (!std::regex_search(header, match, expression) || match.size() < 2U) {
    throw std::runtime_error(std::string("VBF header is missing ") + field);
  }
  return match[1].str();
}

std::string optional_match(const std::string& header,
                           const std::regex& expression) {
  std::smatch match;
  if (!std::regex_search(header, match, expression) || match.size() < 2U) {
    return {};
  }
  return match[1].str();
}

std::uint32_t parse_u32(const std::string& text, const char* field) {
  const auto value = std::stoull(text, nullptr, 16);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::string("VBF ") + field + " is out of range");
  }
  return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> parse_hex_bytes(const std::string& text,
                                          const char* field) {
  if (text.empty() || (text.size() % 2U) != 0U) {
    throw std::runtime_error(std::string("VBF ") + field +
                             " must contain complete bytes");
  }
  std::vector<std::uint8_t> result;
  result.reserve(text.size() / 2U);
  for (std::size_t offset = 0; offset < text.size(); offset += 2U) {
    result.push_back(static_cast<std::uint8_t>(
        std::stoul(text.substr(offset, 2U), nullptr, 16)));
  }
  return result;
}

} // namespace

std::uint16_t vbf_crc16_ccitt_false(
    const std::vector<std::uint8_t>& data) noexcept {
  std::uint16_t crc{0xFFFFU};
  for (const auto byte : data) {
    crc ^= static_cast<std::uint16_t>(byte) << 8U;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0U
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<std::uint16_t>(crc << 1U);
    }
  }
  return crc;
}

std::uint32_t vbf_crc32(const std::vector<std::uint8_t>& data) noexcept {
  std::uint32_t crc{0xFFFFFFFFU};
  for (const auto byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

VbfFile load_vbf(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open VBF file: " + path.string());
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
  if (bytes.size() < 16U) {
    throw std::runtime_error("VBF file is too small: " + path.string());
  }

  const auto open = std::find(bytes.begin(), bytes.end(),
                              static_cast<std::uint8_t>('{'));
  if (open == bytes.end()) {
    throw std::runtime_error("VBF header has no opening brace");
  }
  int depth{};
  std::size_t binary_offset{};
  for (auto cursor = static_cast<std::size_t>(open - bytes.begin());
       cursor < bytes.size(); ++cursor) {
    if (bytes[cursor] == static_cast<std::uint8_t>('{')) ++depth;
    if (bytes[cursor] == static_cast<std::uint8_t>('}')) {
      --depth;
      if (depth == 0) {
        binary_offset = cursor + 1U;
        break;
      }
    }
  }
  if (binary_offset == 0U || binary_offset >= bytes.size()) {
    throw std::runtime_error("VBF header has no balanced closing brace");
  }

  const std::string header(bytes.begin(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(binary_offset));
  VbfFile result;
  result.source = path;
  result.sw_part_number = required_match(
      header, std::regex(R"vbf(sw_part_number\s*=\s*"([^"]+)")vbf"),
      "sw_part_number");
  result.sw_version = required_match(
      header, std::regex(R"vbf(sw_version\s*=\s*"([^"]+)")vbf"),
      "sw_version");
  result.sw_part_type = required_match(
      header, std::regex(R"(sw_part_type\s*=\s*([A-Za-z0-9_]+))"),
      "sw_part_type");
  result.data_format_identifier = static_cast<std::uint8_t>(parse_u32(
      required_match(
          header,
          std::regex(R"(data_format_identifier\s*=\s*0x([0-9A-Fa-f]+))"),
          "data_format_identifier"),
      "data_format_identifier"));
  result.file_checksum = parse_u32(
      required_match(header,
                     std::regex(R"(file_checksum\s*=\s*0x([0-9A-Fa-f]+))"),
                     "file_checksum"),
      "file_checksum");
  const auto signature_dev = optional_match(
      header, std::regex(R"(sw_signature_dev\s*=\s*0x([0-9A-Fa-f]+))"));
  const auto signature_prod = optional_match(
      header, std::regex(R"(sw_signature_prod\s*=\s*0x([0-9A-Fa-f]+))"));
  if (!signature_dev.empty()) {
    result.signature_dev = parse_hex_bytes(signature_dev, "sw_signature_dev");
  }
  if (!signature_prod.empty()) {
    result.signature_prod =
        parse_hex_bytes(signature_prod, "sw_signature_prod");
  }
  result.signature = !result.signature_dev.empty() ? result.signature_dev
                                                    : result.signature_prod;

  std::smatch ecu_match;
  if (std::regex_search(
          header, ecu_match,
          std::regex(R"(\becu_address\s*=\s*0x([0-9A-Fa-f]+))"))) {
    result.ecu_address = parse_u32(ecu_match[1].str(), "ecu_address");
    result.has_ecu_address = true;
  }

  std::smatch call_match;
  if (std::regex_search(
          header, call_match,
          std::regex(R"(\bcall\s*=\s*0x([0-9A-Fa-f]+))"))) {
    result.call_address = parse_u32(call_match[1].str(), "call");
    result.has_call_address = true;
  }

  const auto erase_position = header.find("erase");
  if (erase_position != std::string::npos) {
    const auto erase_end = header.find(';', erase_position);
    const auto erase_text = header.substr(
        erase_position,
        erase_end == std::string::npos ? std::string::npos
                                       : erase_end - erase_position);
    const std::regex pair_expression(
        R"(\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*\})");
    for (std::sregex_iterator item(erase_text.begin(), erase_text.end(),
                                   pair_expression),
         end;
         item != end; ++item) {
      result.erase_ranges.push_back(
          {parse_u32((*item)[1].str(), "erase address"),
           parse_u32((*item)[2].str(), "erase length")});
    }
  }

  std::vector<std::uint8_t> binary(
      bytes.begin() + static_cast<std::ptrdiff_t>(binary_offset), bytes.end());
  if (vbf_crc32(binary) != result.file_checksum) {
    throw std::runtime_error("VBF file_checksum CRC32 mismatch: " +
                             path.string());
  }
  std::size_t offset{};
  while (offset < binary.size()) {
    if (binary.size() - offset < 10U) {
      throw std::runtime_error("VBF binary block header is truncated");
    }
    const auto address = read_be32(binary, offset);
    const auto length = read_be32(binary, offset + 4U);
    offset += 8U;
    if (length > binary.size() - offset - 2U) {
      throw std::runtime_error("VBF binary block data is truncated");
    }
    std::vector<std::uint8_t> data(
        binary.begin() + static_cast<std::ptrdiff_t>(offset),
        binary.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    const auto stored_crc = read_be16(binary, offset);
    offset += 2U;
    if (vbf_crc16_ccitt_false(data) != stored_crc) {
      if (result.data_format_identifier == 0U) {
        throw std::runtime_error("VBF block CRC16 mismatch at address " +
                                 std::to_string(address));
      }
      // For processed VBF data (for example DFI 0x10), the supplier block
      // checksum belongs to the post-processing domain.  The generic parser
      // has no OEM decrypt/decompress implementation, so it cannot reproduce
      // that CRC from the stored transfer bytes.  The file-level CRC32 still
      // authenticates the complete binary area, including all stored block
      // checksums; retain those checksums for the ECU transfer contract.
      result.block_crc16_verified = false;
    }
    result.blocks.push_back({address, std::move(data), stored_crc});
  }
  if (result.blocks.empty()) {
    throw std::runtime_error("VBF contains no binary blocks");
  }
  return result;
}

} // namespace uds
