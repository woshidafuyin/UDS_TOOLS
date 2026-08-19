#include "app/version_value_decoder.hpp"

#include "core/hex.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace uds::app {
namespace {

std::wstring decode_hex(std::span<const std::uint8_t> payload) {
  const auto raw = to_hex(payload);
  return std::wstring(raw.begin(), raw.end());
}

std::wstring decode_ascii_trim(std::span<const std::uint8_t> payload) {
  std::string ascii;
  ascii.reserve(payload.size());
  for (const auto byte : payload) {
    if (byte == 0x00 || byte == 0xFF) continue;
    ascii.push_back(static_cast<char>(byte));
  }
  while (!ascii.empty() &&
         std::isspace(static_cast<unsigned char>(ascii.front()))) {
    ascii.erase(ascii.begin());
  }
  while (!ascii.empty() &&
         std::isspace(static_cast<unsigned char>(ascii.back()))) {
    ascii.pop_back();
  }
  if (!std::all_of(ascii.begin(), ascii.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch <= 0x7E;
      })) {
    throw std::runtime_error("version value contains non-ASCII bytes");
  }
  return std::wstring(ascii.begin(), ascii.end());
}

std::wstring byte_as_two_digits(std::uint8_t value) {
  std::wostringstream output;
  output << std::uppercase << std::hex << std::setfill(L'0') << std::setw(2)
         << static_cast<unsigned>(value);
  return output.str();
}

std::wstring component_name(std::wstring_view identifier) {
  if (identifier.find(L"_PBL") != std::wstring_view::npos) return L"PBL";
  if (identifier.find(L"_SBL") != std::wstring_view::npos) return L"SBL";
  if (identifier.find(L"_APP") != std::wstring_view::npos) return L"APP";
  return L"软件";
}

std::wstring decode_xizhong_f180(std::span<const std::uint8_t> payload) {
  constexpr std::size_t kRecordSize = 15;
  constexpr std::size_t kIdentifierSize = 13;

  if (payload.size() != kRecordSize) {
    throw std::runtime_error("xizhong F180 record layout is invalid");
  }
  const auto identifier =
      decode_ascii_trim(payload.first(kIdentifierSize));
  if (identifier.empty()) {
    throw std::runtime_error("xizhong F180 software identifier is empty");
  }

  std::wstring decoded = identifier;
  decoded += L" V";
  decoded += byte_as_two_digits(payload[kIdentifierSize]);
  decoded += L".";
  decoded += byte_as_two_digits(payload[kIdentifierSize + 1]);
  return decoded;
}

std::wstring decode_xizhong_f189(std::span<const std::uint8_t> payload) {
  constexpr std::size_t kRecordSize = 15;
  constexpr std::size_t kIdentifierSize = 13;

  if (payload.empty()) {
    throw std::runtime_error("xizhong F189 payload is empty");
  }
  const auto count = static_cast<std::size_t>(payload.front());
  if (count == 0 || payload.size() != 1 + count * kRecordSize) {
    throw std::runtime_error("xizhong F189 record layout is invalid");
  }

  std::wstring decoded;
  for (std::size_t index = 0; index < count; ++index) {
    const auto record = payload.subspan(1 + index * kRecordSize, kRecordSize);
    const auto identifier =
        decode_ascii_trim(record.first(kIdentifierSize));
    if (identifier.empty()) {
      throw std::runtime_error("xizhong F189 software identifier is empty");
    }

    if (!decoded.empty()) decoded += L" | ";
    decoded += component_name(identifier);
    decoded += L": ";
    decoded += identifier;
    decoded += L" V";
    decoded += byte_as_two_digits(record[kIdentifierSize]);
    decoded += L".";
    decoded += byte_as_two_digits(record[kIdentifierSize + 1]);
  }
  return decoded;
}

std::wstring decode_bcd_ascii_part_number(
    std::span<const std::uint8_t> payload, std::size_t bcd_bytes) {
  if (payload.size() != bcd_bytes + 3) {
    throw std::runtime_error("BCD plus ASCII part-number layout is invalid");
  }

  std::wstring decoded;
  decoded.reserve(bcd_bytes * 2 + 4);
  for (const auto byte : payload.first(bcd_bytes)) {
    const auto high = static_cast<std::uint8_t>(byte >> 4U);
    const auto low = static_cast<std::uint8_t>(byte & 0x0FU);
    if (high > 9 || low > 9) {
      throw std::runtime_error("part number contains invalid BCD");
    }
    decoded.push_back(static_cast<wchar_t>(L'0' + high));
    decoded.push_back(static_cast<wchar_t>(L'0' + low));
  }

  const auto suffix = decode_ascii_trim(payload.last(3));
  if (!suffix.empty()) {
    decoded += L" ";
    decoded += suffix;
  }
  return decoded;
}

std::wstring decode_counted_bcd_ascii_part_number_list(
    std::span<const std::uint8_t> payload, std::size_t record_size) {
  if (payload.empty()) {
    throw std::runtime_error("counted part-number list is empty");
  }
  const auto count = static_cast<std::size_t>(payload.front());
  if (count == 0 || payload.size() != 1 + count * record_size) {
    throw std::runtime_error("counted part-number list layout is invalid");
  }

  std::wstring decoded = L"模块数: " + std::to_wstring(count);
  for (std::size_t index = 0; index < count; ++index) {
    decoded += L" | #" + std::to_wstring(index + 1) + L": ";
    decoded += decode_bcd_ascii_part_number(
        payload.subspan(1 + index * record_size, record_size),
        record_size - 3);
  }
  return decoded;
}

std::wstring decode_counted_ascii_24(std::span<const std::uint8_t> payload) {
  constexpr std::size_t kIdentifierSize = 24;
  if (payload.size() != 1 + kIdentifierSize || payload.front() != 0x01) {
    throw std::runtime_error("counted ASCII-24 record layout is invalid");
  }
  const auto identifier = decode_ascii_trim(payload.subspan(1));
  if (identifier.empty()) {
    throw std::runtime_error("counted ASCII-24 identifier is empty");
  }
  return L"模块数: 1 | Boot: " + identifier;
}

} // namespace

std::wstring decode_version_value(std::span<const std::uint8_t> payload,
                                  std::wstring_view decoder) {
  if (decoder.empty() || decoder == L"ascii_trim") {
    return decode_ascii_trim(payload);
  }
  if (decoder == L"hex") return decode_hex(payload);
  if (decoder == L"xizhong_f180") return decode_xizhong_f180(payload);
  if (decoder == L"xizhong_f189") return decode_xizhong_f189(payload);
  if (decoder == L"counted_ascii_24") return decode_counted_ascii_24(payload);
  if (decoder == L"bcd_ascii_part_7") {
    return decode_bcd_ascii_part_number(payload, 4);
  }
  if (decoder == L"bcd_ascii_part_8") {
    return decode_bcd_ascii_part_number(payload, 5);
  }
  if (decoder == L"counted_bcd_ascii_part_7") {
    return decode_counted_bcd_ascii_part_number_list(payload, 7);
  }
  if (decoder == L"counted_bcd_ascii_part_8") {
    return decode_counted_bcd_ascii_part_number_list(payload, 8);
  }
  throw std::runtime_error("unknown version value decoder");
}

} // namespace uds::app
