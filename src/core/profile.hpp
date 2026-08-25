#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace uds {

struct FlashTargetProfile {
  std::wstring id;
  std::wstring display_name;
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  bool pending_validation{};
  std::uint16_t expected_app_crc16{};
  std::filesystem::path driver_file;
  std::filesystem::path app_file;
  std::filesystem::path cal_file;
  std::filesystem::path driver_verify_file;
  std::filesystem::path app_verify_file;
  std::filesystem::path cal_verify_file;
  std::filesystem::path security_dll;
  // Optional recovery endpoint override for this target. This keeps projects
  // with multiple ECUs from coupling their FT routing to UI display state.
  std::uint32_t ft_tx_id{};
  std::uint32_t ft_rx_id{};
};

struct FlashProfile {
  std::wstring id;
  std::wstring flow;
  std::wstring name;
  std::wstring vendor_name;
  std::wstring project_name;
  std::wstring device_name;
  std::wstring description;
  bool placeholder{};
  bool can_fd{true};
  bool power_control{true};
  bool extended_id{};
  bool uds_fd{};
  bool uds_brs{};
  bool supports_ft_entry{};
  bool supports_cal_download{};
  // The APP selector may accept a self-contained package whose parser yields
  // both the APP image and its verification payload. Keep this capability in
  // the Profile so the generic UI does not hard-code project identifiers.
  bool supports_app_tmp_package{};
  bool lock_diagnostic_ids{};
  std::wstring default_entry_mode{L"app"};
  std::wstring app_entry_label{L"APP"};
  std::wstring ft_entry_label{L"FT"};
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  std::uint32_t functional_id{};
  std::uint32_t ft_tx_id{};
  std::uint32_t ft_rx_id{};
  bool ft_extended_id{};
  bool ft_uds_fd{};
  bool ft_uds_brs{};
  std::uint8_t ft_padding{};
  unsigned channel{1};
  unsigned nominal_bitrate{500000};
  unsigned data_bitrate{2000000};
  std::uint8_t padding{};
  std::uint8_t isotp_st_min{10};
  unsigned security_level{0x11};
  std::wstring security_variant{L"chuneng"};
  // VBF containers may carry development and production signatures. Projects
  // select "development", "production" or "auto" without coupling that
  // policy to the generic parser.
  std::wstring vbf_signature_policy{L"auto"};
  std::uint32_t driver0_start{};
  std::uint32_t driver0_length{};
  std::uint32_t driver_start{};
  std::uint32_t driver_length{};
  std::uint32_t app_start{};
  std::uint32_t app_length{};
  std::uint32_t cal_start{};
  std::uint32_t cal_length{};
  std::uint16_t expected_driver_crc16{};
  std::filesystem::path driver_file;
  std::filesystem::path app_file;
  std::filesystem::path cal_file;
  std::filesystem::path driver_verify_file;
  std::filesystem::path app_verify_file;
  std::wstring app_verify_label{L"APPData"};
  std::filesystem::path cal_verify_file;
  std::filesystem::path security_dll;
  std::vector<FlashTargetProfile> targets;
};

struct FlashProfileRecord {
  std::filesystem::path source;
  FlashProfile profile;
};

struct FlashProfileLoadError {
  std::filesystem::path source;
  std::string message;
};

struct FlashProfileCatalog {
  std::vector<FlashProfileRecord> profiles;
  std::vector<FlashProfileLoadError> errors;
};

FlashProfile load_profile_ini(const std::filesystem::path& path);
void save_profile_ini(const FlashProfile& profile, const std::filesystem::path& path);
FlashProfileCatalog discover_flash_profiles(const std::filesystem::path& directory);

} // namespace uds
