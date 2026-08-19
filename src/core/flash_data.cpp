#include "core/flash_data.hpp"
#include "core/hex.hpp"
#include "core/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace uds {
namespace {

struct SRecordDataRecord {
  std::uint32_t address{};
  std::vector<std::uint8_t> data;
  unsigned line_number{};
};

std::vector<SRecordDataRecord> parse_srecord_data_records(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open S-record file");

  std::vector<SRecordDataRecord> records;
  std::string line;
  unsigned line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (line.size() < 4 || line[0] != 'S') {
      throw std::runtime_error("invalid S-record line");
    }
    const char type = line[1];
    if (type != '1' && type != '2' && type != '3') continue;
    const std::size_t address_bytes =
        type == '1' ? 2U : (type == '2' ? 3U : 4U);
    auto bytes = from_hex(line.substr(2));
    if (bytes.empty() || bytes[0] != bytes.size() - 1U) {
      throw std::runtime_error("S-record count mismatch");
    }
    unsigned checksum = 0;
    for (const auto byte : bytes) checksum += byte;
    if ((checksum & 0xFFU) != 0xFFU) {
      throw std::runtime_error("S-record checksum mismatch");
    }
    if (bytes.size() < address_bytes + 2U) {
      throw std::runtime_error("short S-record");
    }

    std::uint32_t address = 0;
    for (std::size_t index = 0; index < address_bytes; ++index) {
      address = (address << 8U) | bytes[1 + index];
    }
    const auto data_begin =
        bytes.begin() + static_cast<std::ptrdiff_t>(1 + address_bytes);
    const auto data_end = bytes.end() - 1;
    const auto data_length =
        static_cast<std::uint64_t>(data_end - data_begin);
    if (static_cast<std::uint64_t>(address) + data_length >
        0x100000000ULL) {
      throw std::runtime_error("S-record data address overflows");
    }
    if (data_begin == data_end) continue;
    records.push_back(
        {address, {data_begin, data_end}, line_number});
  }
  if (records.empty()) {
    throw std::runtime_error("S-record has no data records");
  }
  std::sort(records.begin(), records.end(),
            [](const auto& left, const auto& right) {
              if (left.address != right.address) {
                return left.address < right.address;
              }
              return left.line_number < right.line_number;
            });
  return records;
}

std::vector<std::uint8_t> load_srecord_window_impl(const std::filesystem::path& path,
                                                    std::uint32_t start, std::size_t length,
                                                    bool ignore_records_outside_window) {
  if (length == 0) throw std::runtime_error("configured S-record window is empty");
  const auto window_begin = static_cast<std::uint64_t>(start);
  const auto window_end = window_begin + length;
  if (window_end > 0x100000000ULL) throw std::runtime_error("configured S-record window overflows");
  std::vector<std::uint8_t> image(length, 0xFF);
  std::size_t segments = 0;
  for (const auto& segment : load_srecord_image(path).segments) {
    const auto record_begin = static_cast<std::uint64_t>(segment.address);
    const auto record_end = record_begin + segment.data.size();
    if (ignore_records_outside_window &&
        (record_end <= window_begin || record_begin >= window_end)) {
      continue;
    }
    if (record_begin < window_begin || record_end > window_end) {
      throw std::runtime_error("S-record data outside configured flash window");
    }
    std::copy(segment.data.begin(), segment.data.end(),
              image.begin() + static_cast<std::ptrdiff_t>(
                                  segment.address - start));
    ++segments;
  }
  if (segments == 0) throw std::runtime_error("S-record has no data records in configured flash window");
  return image;
}

std::uint32_t read_be32(std::span<const std::uint8_t> data, std::size_t offset,
                        const char* context) {
  if (offset > data.size() || data.size() - offset < 4U) {
    throw std::runtime_error(std::string("CBF is truncated while reading ") + context);
  }
  return (static_cast<std::uint32_t>(data[offset]) << 24U) |
         (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(data[offset + 3U]);
}

std::uint16_t read_be16(std::span<const std::uint8_t> data, std::size_t offset,
                        const char* context) {
  if (offset > data.size() || data.size() - offset < 2U) {
    throw std::runtime_error(std::string("CBF is truncated while reading ") + context);
  }
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(data[offset]) << 8U) | data[offset + 1U]);
}

std::uint16_t crc16_ccitt_false(std::span<const std::uint8_t> data) {
  std::uint16_t crc = 0xFFFFU;
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

std::uint32_t crc32_ieee(std::span<const std::uint8_t> data) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const auto byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::string cbf_header_value(const std::string& header, const char* field) {
  const std::regex pattern(std::string(R"((?:^|\n)\s*)") + field +
                           R"(\s*=\s*([^;]+)\s*;)");
  std::smatch match;
  if (!std::regex_search(header, match, pattern)) {
    throw std::runtime_error(std::string("CBF header field is missing: ") + field);
  }
  return match[1].str();
}

std::string cbf_unquote(std::string value, const char* field) {
  const auto first = value.find_first_not_of(" \t\r\n");
  const auto last = value.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    throw std::runtime_error(std::string("CBF header field is empty: ") + field);
  }
  value = value.substr(first, last - first + 1U);
  if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
    throw std::runtime_error(std::string("CBF header field must be quoted: ") + field);
  }
  return value.substr(1U, value.size() - 2U);
}

std::uint32_t cbf_hex_u32(const std::string& value, const char* field) {
  try {
    std::size_t used{};
    const auto parsed = std::stoul(value, &used, 0);
    if (used != value.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("invalid");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (const std::exception&) {
    throw std::runtime_error(std::string("CBF header field is not a 32-bit hexadecimal value: ") + field);
  }
}

std::vector<std::uint8_t> cbf_hex_bytes(std::string value, const char* field,
                                        std::size_t expected_size) {
  const auto first = value.find_first_not_of(" \t\r\n");
  const auto last = value.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) throw std::runtime_error(std::string("CBF header field is empty: ") + field);
  value = value.substr(first, last - first + 1U);
  if (value.rfind("0x", 0) != 0U && value.rfind("0X", 0) != 0U) {
    throw std::runtime_error(std::string("CBF header field must use 0x hexadecimal form: ") + field);
  }
  value.erase(0, 2U);
  if (value.size() != expected_size * 2U ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
      })) {
    throw std::runtime_error(std::string("CBF header field has an invalid length or hexadecimal data: ") + field);
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(expected_size);
  for (std::size_t offset = 0; offset < value.size(); offset += 2U) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(value.substr(offset, 2U), nullptr, 16)));
  }
  return bytes;
}

CbfSegment read_cbf_segment(std::span<const std::uint8_t> file,
                            std::size_t& offset,
                            std::vector<std::uint8_t>& checksum_data) {
  const auto address = read_be32(file, offset, "block address");
  const auto length = read_be32(file, offset + 4U, "block length");
  if (length == 0U || length > file.size() || offset + 10U > file.size() - length) {
    throw std::runtime_error("CBF block length exceeds file size");
  }
  checksum_data.insert(checksum_data.end(), file.begin() + static_cast<std::ptrdiff_t>(offset),
                       file.begin() + static_cast<std::ptrdiff_t>(offset + 8U + length + 2U));
  CbfSegment segment{address,
                     {file.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
                      file.begin() + static_cast<std::ptrdiff_t>(offset + 8U + length)}};
  const auto expected_crc = read_be16(file, offset + 8U + length, "block CRC16");
  if (crc16_ccitt_false(segment.data) != expected_crc) {
    throw std::runtime_error("CBF block CRC16 mismatch");
  }
  offset += 10U + length;
  return segment;
}

} // namespace

SRecordImage load_srecord_image(const std::filesystem::path& path) {
  const auto records = parse_srecord_data_records(path);
  SRecordImage image;
  for (const auto& record : records) {
    if (image.segments.empty()) {
      image.segments.push_back({record.address, record.data});
      continue;
    }

    auto& segment = image.segments.back();
    const auto segment_begin = static_cast<std::uint64_t>(segment.address);
    const auto segment_end = segment_begin + segment.data.size();
    const auto record_begin = static_cast<std::uint64_t>(record.address);
    if (record_begin > segment_end) {
      image.segments.push_back({record.address, record.data});
      continue;
    }

    const auto offset =
        static_cast<std::size_t>(record_begin - segment_begin);
    const auto overlap =
        std::min(record.data.size(), segment.data.size() - offset);
    if (!std::equal(record.data.begin(),
                    record.data.begin() +
                        static_cast<std::ptrdiff_t>(overlap),
                    segment.data.begin() +
                        static_cast<std::ptrdiff_t>(offset))) {
      throw std::runtime_error(
          "overlapping S-record data contains conflicting bytes");
    }
    if (overlap < record.data.size()) {
      segment.data.insert(
          segment.data.end(),
          record.data.begin() + static_cast<std::ptrdiff_t>(overlap),
          record.data.end());
    }
  }
  for (const auto& segment : image.segments) {
    if (image.payload_size >
        std::numeric_limits<std::size_t>::max() - segment.data.size()) {
      throw std::runtime_error("S-record payload size overflows");
    }
    image.payload_size += segment.data.size();
  }
  return image;
}

SRecordSegment load_single_srecord_segment(
    const std::filesystem::path& path) {
  auto image = load_srecord_image(path);
  if (image.segments.size() != 1U) {
    throw std::runtime_error(
        "S-record requires one contiguous data segment, found " +
        std::to_string(image.segments.size()));
  }
  return std::move(image.segments.front());
}

std::vector<std::uint8_t> load_srecord_window(const std::filesystem::path& path,
                                               std::uint32_t start, std::size_t length) {
  return load_srecord_window_impl(path, start, length, false);
}

std::vector<std::uint8_t> load_srecord_window_filtered(const std::filesystem::path& path,
                                                        std::uint32_t start, std::size_t length) {
  return load_srecord_window_impl(path, start, length, true);
}

std::vector<std::uint8_t> load_hex_bytes(const std::filesystem::path& path,
                                         std::size_t take, std::size_t minimum) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open hexadecimal byte file");
  std::ostringstream content;
  content << input.rdbuf();
  const auto text = content.str();
  const std::regex token(R"((^|[^0-9A-Fa-f])([0-9A-Fa-f]{2})(?=$|[^0-9A-Fa-f]))");
  std::vector<std::uint8_t> bytes;
  auto begin = std::sregex_iterator(text.begin(), text.end(), token);
  const auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it)
    bytes.push_back(static_cast<std::uint8_t>(std::stoul((*it)[2].str(), nullptr, 16)));
  if (bytes.size() < minimum) throw std::runtime_error("hexadecimal byte file is too short");
  bytes.resize(take);
  return bytes;
}

std::vector<std::uint8_t> load_asc_hex(const std::filesystem::path& path,
                                      std::size_t take, std::size_t minimum) {
  return load_hex_bytes(path, take, minimum);
}

CbfImage load_chuneng_cbf(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open CBF file");
  const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(input), {}};
  const std::string text(file.begin(), file.end());
  if (text.rfind("cbf_version=1.0;", 0) != 0U) {
    throw std::runtime_error("unsupported CBF version");
  }
  const auto header_end = text.find("\n}");
  if (header_end == std::string::npos) throw std::runtime_error("CBF header end is missing");
  const auto data_start = header_end + 2U;
  const auto header = text.substr(0, data_start);

  CbfImage image;
  image.software_id = cbf_unquote(cbf_header_value(header, "sw_id"), "sw_id");
  image.software_version = cbf_unquote(cbf_header_value(header, "sw_version"), "sw_version");
  image.software_type = cbf_unquote(cbf_header_value(header, "sw_type"), "sw_type");
  if (cbf_hex_u32(cbf_header_value(header, "data_format_id"), "data_format_id") != 0U) {
    throw std::runtime_error("CBF data_format_id is not raw 0x00");
  }
  image.ecu_address = cbf_hex_u32(cbf_header_value(header, "ecu_address"), "ecu_address");
  const auto abt_address = cbf_hex_u32(cbf_header_value(header, "abt_start"), "abt_start");
  const auto abt_length = cbf_hex_u32(cbf_header_value(header, "abt_length"), "abt_length");
  const auto abt_hash = cbf_hex_bytes(cbf_header_value(header, "abt_hash"), "abt_hash", 32U);
  image.device_signature = cbf_hex_bytes(cbf_header_value(header, "dev_signature"), "dev_signature", 256U);
  const auto expected_checksum = cbf_hex_u32(cbf_header_value(header, "cbf_checksum"), "cbf_checksum");

  std::size_t offset = data_start;
  while (offset < file.size() && (file[offset] == '\r' || file[offset] == '\n')) ++offset;
  std::vector<std::uint8_t> checksum_data;
  image.main = read_cbf_segment(file, offset, checksum_data);
  image.abt = read_cbf_segment(file, offset, checksum_data);
  if (offset != file.size()) throw std::runtime_error("CBF has trailing data after two blocks");
  if (crc32_ieee(checksum_data) != expected_checksum) throw std::runtime_error("CBF checksum mismatch");
  if (image.abt.address != abt_address || image.abt.data.size() != abt_length ||
      image.abt.data.size() != 0x2CU) {
    throw std::runtime_error("CBF ABT block does not match header");
  }
  std::vector<std::uint8_t> abt_hash_data;
  abt_hash_data.reserve(8U + image.abt.data.size());
  for (const auto shift : {24U, 16U, 8U, 0U}) abt_hash_data.push_back(static_cast<std::uint8_t>(image.abt.address >> shift));
  for (const auto shift : {24U, 16U, 8U, 0U}) abt_hash_data.push_back(static_cast<std::uint8_t>(image.abt.data.size() >> shift));
  abt_hash_data.insert(abt_hash_data.end(), image.abt.data.begin(), image.abt.data.end());
  const auto actual_abt_hash = sha256(abt_hash_data);
  if (!std::equal(actual_abt_hash.begin(), actual_abt_hash.end(), abt_hash.begin())) {
    throw std::runtime_error("CBF ABT SHA-256 mismatch");
  }
  const auto main_hash = sha256(image.main.data);
  if (read_be16(image.abt.data, 0, "ABT hash type") != 0U ||
      read_be16(image.abt.data, 2, "ABT entry count") != 1U ||
      read_be32(image.abt.data, 4, "ABT entry address") != image.main.address ||
      read_be32(image.abt.data, 8, "ABT entry length") != image.main.data.size() ||
      !std::equal(main_hash.begin(), main_hash.end(), image.abt.data.begin() + 12)) {
    throw std::runtime_error("CBF ABT main-data contract mismatch");
  }
  return image;
}

} // namespace uds
