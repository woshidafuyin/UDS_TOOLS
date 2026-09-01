#pragma once

#include "core/profile.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace uds::app {

inline constexpr unsigned kMinFlashRepeatCount = 1;
inline constexpr unsigned kMaxFlashRepeatCount = 10000;

// Immutable-by-convention snapshot assembled by a UI before a flash operation
// starts. Controllers receive this type by value so later UI edits cannot
// change an operation that is already running.
struct FlashRequest {
  FlashProfile profile;
  std::wstring target_id;
  std::wstring entry_mode{L"app"};
  bool update_public_key{};
  unsigned repeat_count{kMinFlashRepeatCount};
  std::filesystem::path executable_directory;

  // Audit metadata captured at the operation boundary.  These fields do not
  // control a workflow; they make the execution log and HTML report explain
  // exactly which hardware backend and pre-flash qualification evidence were
  // associated with this immutable request.
  std::string hardware_backend{"unspecified"};
  std::string target_description{"unspecified"};
  std::string qualification_status{"NOT_RUN"};
  std::string qualification_detail{"No matching pre-flash probe was recorded"};
  std::string qualification_completed_at;

  unsigned channel{};
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  std::uint32_t functional_id{};
  unsigned nominal_bitrate{};
  unsigned data_bitrate{};
  std::uint8_t padding{};

  std::filesystem::path driver_file;
  std::filesystem::path app_file;
  std::filesystem::path cal_file;
  std::filesystem::path driver_verify_file;
  std::filesystem::path app_verify_file;
  std::filesystem::path cal_verify_file;
  std::filesystem::path security_dll;
  std::filesystem::path trace_file;
};

} // namespace uds::app
