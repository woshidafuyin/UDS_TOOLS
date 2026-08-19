#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace uds {

struct VersionCheckItem {
  std::wstring name;
  std::vector<std::uint8_t> request;
  std::vector<std::uint8_t> response_prefix;
  std::wstring expected;
  std::wstring decoder{L"ascii_trim"};
  bool required{true};
};

struct VersionCheckPlan {
  std::uint8_t session{0x01};
  std::wstring precondition;
  std::vector<VersionCheckItem> items;
};

// Loads the optional [version_check] section from the same INI used by the
// flash profile. Items without an expected value are read-only; projects that
// explicitly need comparison may configure an expected value independently.
[[nodiscard]] VersionCheckPlan load_version_check_plan(
    const std::filesystem::path& profile_path, std::wstring_view target_id);

} // namespace uds
