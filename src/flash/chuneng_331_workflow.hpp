#pragma once

#include "flash/flash_workflow.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

inline constexpr std::uint32_t kChuneng331CbfDriverAddress = 0x10280000U;

constexpr bool is_supported_chuneng_driver_cbf_type(
    std::string_view software_type) noexcept {
  return software_type == "EXE" || software_type == "SBL";
}

constexpr bool is_supported_chuneng_app_cbf_type(
    std::string_view software_type) noexcept {
  return software_type == "DATA" || software_type == "APP";
}

enum class Chuneng331InputMode {
  cbf_pair,
  srecord_pair,
};

struct Chuneng331AbtMetadata {
  std::uint32_t source_address{};
  std::uint32_t image_length{};
};

// S-record mode uses bridge-compatible sidecars: Driver_Ver.asc pairs with
// Driver_ABT.asc, and APP_Ver.asc pairs with APP_ABT.asc.
[[nodiscard]] std::filesystem::path chuneng_331_abt_sidecar_path(
    const std::filesystem::path& verification_path);
[[nodiscard]] Chuneng331AbtMetadata validate_chuneng_331_abt(
    std::span<const std::uint8_t> abt,
    std::span<const std::uint8_t> image);

// A ChuNeng package is an atomic Driver/APP pair. Mixing a CBF role with an
// S-record role would also mix signature provenance, so reject it during
// preflight before a CAN provider is created.
Chuneng331InputMode resolve_chuneng_331_input_mode(
    const std::filesystem::path& driver,
    const std::filesystem::path& app);

class Chuneng331Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;
};

// The current ARC331 profile is the only selectable ChuNeng workflow. It
// accepts either a Driver+APP CBF pair or a Driver+APP S-record/ASC pair, then
// delegates both roles to the dedicated 331 state machine; it never enters the
// LP radar certificate flow.
class ChunengArc331Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;
};

} // namespace uds
