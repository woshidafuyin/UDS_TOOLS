#include "core/version_check_plan.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace uds {
namespace {

using Values = std::unordered_map<std::wstring, std::wstring>;
using Sections = std::unordered_map<std::wstring, Values>;

std::wstring trim(std::wstring value) {
  const auto space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), space));
  value.erase(
      std::find_if_not(value.rbegin(), value.rend(), space).base(),
      value.end());
  return value;
}

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) {
                   return static_cast<wchar_t>(std::towlower(ch));
                 });
  return value;
}

std::wstring decode_utf8(std::string bytes) {
  if (bytes.size() >= 3 &&
      static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF) {
    bytes.erase(0, 3);
  }
  if (bytes.empty()) return {};
  const auto count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
      static_cast<int>(bytes.size()), nullptr, 0);
  if (count <= 0) {
    throw std::runtime_error("version-check profile must be UTF-8 encoded");
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                      static_cast<int>(bytes.size()), result.data(), count);
  return result;
}

Sections read_sections(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open version-check profile");
  const std::string bytes{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
  std::wistringstream lines(decode_utf8(bytes));
  Sections sections;
  std::wstring section;
  for (std::wstring line; std::getline(lines, line);) {
    line = trim(std::move(line));
    if (line.empty() || line.front() == L';' || line.front() == L'#') {
      continue;
    }
    if (line.front() == L'[' && line.back() == L']') {
      section = lowercase(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    if (section.empty()) continue;
    const auto separator = line.find(L'=');
    if (separator == std::wstring::npos) continue;
    auto key = lowercase(trim(line.substr(0, separator)));
    if (!key.empty()) {
      sections[section][std::move(key)] =
          trim(line.substr(separator + 1));
    }
  }
  return sections;
}

std::wstring value(const Values& values, std::wstring_view key,
                   std::wstring fallback = {}) {
  const auto found = values.find(lowercase(std::wstring(key)));
  return found == values.end() ? std::move(fallback) : found->second;
}

bool boolean(const Values& values, std::wstring_view key, bool fallback) {
  const auto raw = lowercase(value(values, key));
  if (raw.empty()) return fallback;
  if (raw == L"1" || raw == L"true" || raw == L"yes" || raw == L"on") {
    return true;
  }
  if (raw == L"0" || raw == L"false" || raw == L"no" || raw == L"off") {
    return false;
  }
  throw std::runtime_error("invalid version-check boolean");
}

unsigned number(const Values& values, std::wstring_view key,
                unsigned fallback) {
  const auto raw = value(values, key);
  return raw.empty() ? fallback
                     : static_cast<unsigned>(std::stoul(raw, nullptr, 0));
}

std::vector<std::uint8_t> hex_bytes(const std::wstring& raw) {
  std::wistringstream tokens(raw);
  std::vector<std::uint8_t> bytes;
  for (std::wstring token; tokens >> token;) {
    const auto parsed = std::stoul(token, nullptr, 16);
    if (parsed > 0xFFU) {
      throw std::runtime_error("version-check request byte exceeds 0xFF");
    }
    bytes.push_back(static_cast<std::uint8_t>(parsed));
  }
  return bytes;
}

std::wstring item_key(std::size_t index, std::wstring_view field) {
  return L"item_" + std::to_wstring(index) + L"_" + std::wstring(field);
}

std::vector<std::uint8_t> default_prefix(
    const std::vector<std::uint8_t>& request) {
  if (request.empty()) return {};
  if (request[0] == 0x22 && request.size() >= 3) {
    return {0x62, request[1], request[2]};
  }
  if (request[0] == 0x23) return {0x63};
  return {static_cast<std::uint8_t>(request[0] + 0x40U)};
}

} // namespace

VersionCheckPlan load_version_check_plan(
    const std::filesystem::path& profile_path, std::wstring_view target_id) {
  const auto sections = read_sections(profile_path);
  const auto base_it = sections.find(L"version_check");
  if (base_it == sections.end()) return {};
  const auto& base = base_it->second;

  const auto target_section =
      L"version_check.target." + lowercase(std::wstring(target_id));
  const auto target_it = sections.find(target_section);
  const Values empty;
  const auto& overrides =
      target_it == sections.end() ? empty : target_it->second;

  VersionCheckPlan plan;
  plan.session = static_cast<std::uint8_t>(
      number(base, L"session", plan.session));
  plan.precondition = lowercase(value(base, L"precondition"));
  const auto count = number(base, L"item_count", 0);
  plan.items.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    VersionCheckItem item;
    item.name = value(base, item_key(index, L"name"));
    item.request = hex_bytes(value(base, item_key(index, L"request")));
    item.response_prefix =
        hex_bytes(value(base, item_key(index, L"response_prefix")));
    if (item.response_prefix.empty()) {
      item.response_prefix = default_prefix(item.request);
    }
    item.expected = value(overrides, item_key(index, L"expected"),
                          value(base, item_key(index, L"expected")));
    item.decoder = lowercase(value(base, item_key(index, L"decoder"),
                                   item.decoder));
    item.required =
        boolean(base, item_key(index, L"required"), item.required);
    if (item.name.empty() || item.request.empty() ||
        item.response_prefix.empty()) {
      throw std::runtime_error(
          "version-check item requires name, request and response prefix");
    }
    plan.items.push_back(std::move(item));
  }
  return plan;
}

} // namespace uds
