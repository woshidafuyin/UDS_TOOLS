#include "core/profile.hpp"

#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace uds {
namespace {

using ProfileValues = std::unordered_map<std::wstring, std::wstring>;

std::wstring trim(std::wstring value) {
  const auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
  return value;
}

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return value;
}

std::wstring decode_utf8(std::string bytes) {
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF) {
    bytes.erase(0, 3);
  }
  if (bytes.empty()) return {};
  const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                         static_cast<int>(bytes.size()), nullptr, 0);
  if (count <= 0) throw std::runtime_error("profile must be UTF-8 encoded");
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                      static_cast<int>(bytes.size()), result.data(), count);
  return result;
}

std::string encode_utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) throw std::runtime_error("profile contains invalid Unicode text");
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
  return result;
}

ProfileValues read_profile_values(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open profile");
  const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  std::wistringstream lines(decode_utf8(bytes));
  ProfileValues values;
  bool in_profile_section = false;
  for (std::wstring line; std::getline(lines, line);) {
    line = trim(std::move(line));
    if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
    if (line.front() == L'[' && line.back() == L']') {
      in_profile_section = lowercase(trim(line.substr(1, line.size() - 2))) == L"profile";
      continue;
    }
    if (!in_profile_section) continue;
    const auto separator = line.find(L'=');
    if (separator == std::wstring::npos) continue;
    auto key = lowercase(trim(line.substr(0, separator)));
    if (!key.empty()) values[std::move(key)] = trim(line.substr(separator + 1));
  }
  return values;
}

std::wstring read_text(const ProfileValues& values, std::wstring_view key,
                       const std::wstring& fallback = {}) {
  const auto item = values.find(lowercase(std::wstring(key)));
  return item == values.end() ? fallback : item->second;
}

unsigned read_uint(const ProfileValues& values, std::wstring_view key,
                   unsigned fallback) {
  const auto value = read_text(values, key);
  if (value.empty()) return fallback;
  return static_cast<unsigned>(std::stoul(value, nullptr, 0));
}

bool read_bool(const ProfileValues& values, std::wstring_view key,
               bool fallback) {
  const auto value = lowercase(read_text(values, key));
  if (value.empty()) return fallback;
  if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") return true;
  if (value == L"0" || value == L"false" || value == L"no" || value == L"off") return false;
  throw std::runtime_error("invalid boolean profile value");
}

void append_value(std::wostringstream& output, std::wstring_view key, const std::wstring& value) {
  output << key << L'=' << value << L'\n';
}

std::wstring target_key(std::size_t index, std::wstring_view field) {
  return L"target_" + std::to_wstring(index) + L"_" + std::wstring(field);
}

} // namespace

FlashProfile load_profile_ini(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("profile not found");
  const auto values = read_profile_values(path);
  FlashProfile p;
  p.id = read_text(values, L"id", path.stem().wstring());
  p.flow = read_text(values, L"flow", p.flow);
  p.name = read_text(values, L"name", p.name);
  p.vendor_name = read_text(values, L"vendor_name", p.vendor_name);
  p.project_name = read_text(values, L"project_name", p.project_name);
  p.device_name = read_text(values, L"device_name", p.device_name);
  p.description = read_text(values, L"description", p.description);
  p.placeholder = read_bool(values, L"placeholder", p.placeholder);
  p.can_fd = read_bool(values, L"can_fd", p.can_fd);
  p.power_control = read_bool(values, L"power_control", p.power_control);
  p.extended_id = read_bool(values, L"extended_id", p.extended_id);
  p.uds_fd = read_bool(values, L"uds_fd", p.uds_fd);
  p.uds_brs = read_bool(values, L"uds_brs", p.uds_brs);
  p.supports_ft_entry =
      read_bool(values, L"supports_ft_entry", p.supports_ft_entry);
  p.supports_cal_download =
      read_bool(values, L"supports_cal_download",
                p.supports_cal_download);
  p.supports_app_tmp_package = read_bool(
      values, L"supports_app_tmp_package", p.supports_app_tmp_package);
  p.lock_diagnostic_ids =
      read_bool(values, L"lock_diagnostic_ids", p.lock_diagnostic_ids);
  p.default_entry_mode =
      lowercase(read_text(values, L"default_entry_mode", p.default_entry_mode));
  p.app_entry_label =
      read_text(values, L"app_entry_label", p.app_entry_label);
  p.ft_entry_label =
      read_text(values, L"ft_entry_label", p.ft_entry_label);
  p.tx_id = read_uint(values, L"tx_id", p.tx_id);
  p.rx_id = read_uint(values, L"rx_id", p.rx_id);
  p.functional_id = read_uint(values, L"functional_id", p.functional_id);
  p.programming_tx_id =
      read_uint(values, L"programming_tx_id", p.programming_tx_id);
  p.programming_rx_id =
      read_uint(values, L"programming_rx_id", p.programming_rx_id);
  p.ft_tx_id = read_uint(values, L"ft_tx_id", p.ft_tx_id);
  p.ft_rx_id = read_uint(values, L"ft_rx_id", p.ft_rx_id);
  p.ft_extended_id =
      read_bool(values, L"ft_extended_id", p.ft_extended_id);
  p.ft_uds_fd = read_bool(values, L"ft_uds_fd", p.ft_uds_fd);
  p.ft_uds_brs = read_bool(values, L"ft_uds_brs", p.ft_uds_brs);
  p.ft_padding =
      static_cast<std::uint8_t>(read_uint(values, L"ft_padding", p.ft_padding));
  p.channel = read_uint(values, L"channel", p.channel);
  p.nominal_bitrate = read_uint(values, L"nominal_bitrate", p.nominal_bitrate);
  p.data_bitrate = read_uint(values, L"data_bitrate", p.data_bitrate);
  p.padding = static_cast<std::uint8_t>(read_uint(values, L"padding", p.padding));
  p.isotp_st_min = static_cast<std::uint8_t>(read_uint(values, L"isotp_st_min", p.isotp_st_min));
  p.security_level = read_uint(values, L"security_level", p.security_level);
  p.security_variant = read_text(values, L"security_variant", p.security_variant);
  p.vbf_signature_policy = lowercase(
      read_text(values, L"vbf_signature_policy", p.vbf_signature_policy));
  p.driver0_start = read_uint(values, L"driver0_start", p.driver0_start);
  p.driver0_length = read_uint(values, L"driver0_length", p.driver0_length);
  p.driver_start = read_uint(values, L"driver_start", p.driver_start);
  p.driver_length = read_uint(values, L"driver_length", p.driver_length);
  p.app_start = read_uint(values, L"app_start", p.app_start);
  p.app_length = read_uint(values, L"app_length", p.app_length);
  p.cal_start = read_uint(values, L"cal_start", p.cal_start);
  p.cal_length = read_uint(values, L"cal_length", p.cal_length);
  p.expected_driver_crc16 = static_cast<std::uint16_t>(
      read_uint(values, L"expected_driver_crc16",
                p.expected_driver_crc16));
  p.driver_file = read_text(values, L"driver_file");
  p.app_file = read_text(values, L"app_file");
  p.cal_file = read_text(values, L"cal_file");
  p.driver_verify_file = read_text(values, L"driver_verify_file");
  p.app_verify_file = read_text(values, L"app_verify_file");
  p.app_verify_label =
      read_text(values, L"app_verify_label", p.app_verify_label);
  p.cal_verify_file = read_text(values, L"cal_verify_file");
  p.security_dll = read_text(values, L"security_dll");
  const auto target_count = read_uint(values, L"target_count", 0);
  p.targets.reserve(target_count);
  for (std::size_t index = 0; index < target_count; ++index) {
    FlashTargetProfile target;
    target.id = read_text(values, target_key(index, L"id"));
    target.display_name =
        read_text(values, target_key(index, L"display_name"), target.id);
    target.tx_id = read_uint(values, target_key(index, L"tx_id"), 0);
    target.rx_id = read_uint(values, target_key(index, L"rx_id"), 0);
    target.pending_validation =
        read_bool(values, target_key(index, L"pending_validation"), false);
    target.expected_app_crc16 = static_cast<std::uint16_t>(
        read_uint(values, target_key(index, L"expected_app_crc16"), 0));
    target.driver_file = read_text(values, target_key(index, L"driver_file"));
    target.app_file = read_text(values, target_key(index, L"app_file"));
    target.cal_file = read_text(values, target_key(index, L"cal_file"));
    target.driver_verify_file =
        read_text(values, target_key(index, L"driver_verify_file"));
    target.app_verify_file =
        read_text(values, target_key(index, L"app_verify_file"));
    target.cal_verify_file =
        read_text(values, target_key(index, L"cal_verify_file"));
    target.security_dll =
        read_text(values, target_key(index, L"security_dll"));
    target.ft_tx_id =
        read_uint(values, target_key(index, L"ft_tx_id"), 0);
    target.ft_rx_id =
        read_uint(values, target_key(index, L"ft_rx_id"), 0);
    if (target.id.empty() || target.display_name.empty() ||
        target.tx_id == 0 || target.rx_id == 0) {
      throw std::runtime_error("invalid target profile entry");
    }
    p.targets.push_back(std::move(target));
  }
  return p;
}

void save_profile_ini(const FlashProfile& p, const std::filesystem::path& path) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::wostringstream contents;
  contents << L"[profile]\n";
  append_value(contents, L"id", p.id);
  append_value(contents, L"flow", p.flow);
  append_value(contents, L"name", p.name);
  append_value(contents, L"vendor_name", p.vendor_name);
  append_value(contents, L"project_name", p.project_name);
  append_value(contents, L"device_name", p.device_name);
  append_value(contents, L"description", p.description);
  append_value(contents, L"placeholder", p.placeholder ? L"true" : L"false");
  append_value(contents, L"can_fd", p.can_fd ? L"true" : L"false");
  append_value(contents, L"power_control", p.power_control ? L"true" : L"false");
  append_value(contents, L"extended_id", p.extended_id ? L"true" : L"false");
  append_value(contents, L"uds_fd", p.uds_fd ? L"true" : L"false");
  append_value(contents, L"uds_brs", p.uds_brs ? L"true" : L"false");
  append_value(contents, L"supports_ft_entry", p.supports_ft_entry ? L"true" : L"false");
  append_value(contents, L"supports_cal_download",
               p.supports_cal_download ? L"true" : L"false");
  append_value(contents, L"supports_app_tmp_package",
               p.supports_app_tmp_package ? L"true" : L"false");
  append_value(contents, L"lock_diagnostic_ids",
               p.lock_diagnostic_ids ? L"true" : L"false");
  append_value(contents, L"default_entry_mode", p.default_entry_mode);
  append_value(contents, L"app_entry_label", p.app_entry_label);
  append_value(contents, L"ft_entry_label", p.ft_entry_label);
  append_value(contents, L"tx_id", std::to_wstring(p.tx_id));
  append_value(contents, L"rx_id", std::to_wstring(p.rx_id));
  append_value(contents, L"functional_id", std::to_wstring(p.functional_id));
  append_value(contents, L"programming_tx_id",
               std::to_wstring(p.programming_tx_id));
  append_value(contents, L"programming_rx_id",
               std::to_wstring(p.programming_rx_id));
  append_value(contents, L"ft_tx_id", std::to_wstring(p.ft_tx_id));
  append_value(contents, L"ft_rx_id", std::to_wstring(p.ft_rx_id));
  append_value(contents, L"ft_extended_id", p.ft_extended_id ? L"true" : L"false");
  append_value(contents, L"ft_uds_fd", p.ft_uds_fd ? L"true" : L"false");
  append_value(contents, L"ft_uds_brs", p.ft_uds_brs ? L"true" : L"false");
  append_value(contents, L"ft_padding", std::to_wstring(p.ft_padding));
  append_value(contents, L"channel", std::to_wstring(p.channel));
  append_value(contents, L"nominal_bitrate", std::to_wstring(p.nominal_bitrate));
  append_value(contents, L"data_bitrate", std::to_wstring(p.data_bitrate));
  append_value(contents, L"padding", std::to_wstring(p.padding));
  append_value(contents, L"isotp_st_min", std::to_wstring(p.isotp_st_min));
  append_value(contents, L"security_level", std::to_wstring(p.security_level));
  append_value(contents, L"security_variant", p.security_variant);
  append_value(contents, L"vbf_signature_policy", p.vbf_signature_policy);
  append_value(contents, L"driver0_start", std::to_wstring(p.driver0_start));
  append_value(contents, L"driver0_length", std::to_wstring(p.driver0_length));
  append_value(contents, L"driver_start", std::to_wstring(p.driver_start));
  append_value(contents, L"driver_length", std::to_wstring(p.driver_length));
  append_value(contents, L"app_start", std::to_wstring(p.app_start));
  append_value(contents, L"app_length", std::to_wstring(p.app_length));
  append_value(contents, L"cal_start", std::to_wstring(p.cal_start));
  append_value(contents, L"cal_length", std::to_wstring(p.cal_length));
  append_value(contents, L"expected_driver_crc16",
               std::to_wstring(p.expected_driver_crc16));
  append_value(contents, L"driver_file", p.driver_file.wstring());
  append_value(contents, L"app_file", p.app_file.wstring());
  append_value(contents, L"cal_file", p.cal_file.wstring());
  append_value(contents, L"driver_verify_file", p.driver_verify_file.wstring());
  append_value(contents, L"app_verify_file", p.app_verify_file.wstring());
  append_value(contents, L"app_verify_label", p.app_verify_label);
  append_value(contents, L"cal_verify_file", p.cal_verify_file.wstring());
  append_value(contents, L"security_dll", p.security_dll.wstring());
  append_value(contents, L"target_count", std::to_wstring(p.targets.size()));
  for (std::size_t index = 0; index < p.targets.size(); ++index) {
    const auto& target = p.targets[index];
    append_value(contents, target_key(index, L"id"), target.id);
    append_value(contents, target_key(index, L"display_name"),
                 target.display_name);
    append_value(contents, target_key(index, L"tx_id"),
                 std::to_wstring(target.tx_id));
    append_value(contents, target_key(index, L"rx_id"),
                 std::to_wstring(target.rx_id));
    append_value(contents, target_key(index, L"pending_validation"),
                 target.pending_validation ? L"true" : L"false");
    append_value(contents, target_key(index, L"expected_app_crc16"),
                 std::to_wstring(target.expected_app_crc16));
    append_value(contents, target_key(index, L"driver_file"),
                 target.driver_file.wstring());
    append_value(contents, target_key(index, L"app_file"),
                 target.app_file.wstring());
    append_value(contents, target_key(index, L"cal_file"),
                 target.cal_file.wstring());
    append_value(contents, target_key(index, L"driver_verify_file"),
                 target.driver_verify_file.wstring());
    append_value(contents, target_key(index, L"app_verify_file"),
                 target.app_verify_file.wstring());
    append_value(contents, target_key(index, L"cal_verify_file"),
                 target.cal_verify_file.wstring());
    append_value(contents, target_key(index, L"security_dll"),
                 target.security_dll.wstring());
    append_value(contents, target_key(index, L"ft_tx_id"),
                 std::to_wstring(target.ft_tx_id));
    append_value(contents, target_key(index, L"ft_rx_id"),
                 std::to_wstring(target.ft_rx_id));
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write profile");
  const auto bytes = encode_utf8(contents.str());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("cannot write profile");
}

FlashProfileCatalog discover_flash_profiles(const std::filesystem::path& directory) {
  FlashProfileCatalog catalog;
  if (!std::filesystem::is_directory(directory)) return catalog;

  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    auto extension = entry.path().extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension != L".ini") continue;
    try {
      auto profile = load_profile_ini(entry.path());
      if (profile.id.empty()) profile.id = entry.path().stem().wstring();
      if (profile.name.empty()) profile.name = profile.id;
      if (profile.flow.empty() && !profile.placeholder) {
        throw std::runtime_error("profile flow is empty");
      }
      catalog.profiles.push_back({entry.path(), std::move(profile)});
    } catch (const std::exception& error) {
      catalog.errors.push_back({entry.path(), error.what()});
    }
  }
  std::sort(catalog.profiles.begin(), catalog.profiles.end(),
            [](const FlashProfileRecord& left, const FlashProfileRecord& right) {
              if (left.profile.name != right.profile.name) return left.profile.name < right.profile.name;
              return left.profile.id < right.profile.id;
            });
  return catalog;
}

} // namespace uds
