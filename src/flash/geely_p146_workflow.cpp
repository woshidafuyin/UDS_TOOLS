#include "flash/geely_p146_workflow.hpp"

#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "core/vbf.hpp"
#include "flash/geely_geea2_flow.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace uds {
namespace {

std::filesystem::path resolve_path(
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& selected) {
  if (selected.empty() || selected.is_absolute()) return selected;
  return executable_directory / selected;
}

void report(const FlashWorkflowCallbacks& callbacks, std::string step,
            std::string verdict, std::string detail) {
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::toupper(ch));
                 });
  return value;
}

std::string describe(const GeelyGeea2Image& image) {
  std::size_t bytes{};
  for (const auto& block : image.file.blocks) bytes += block.data.size();
  std::ostringstream output;
  output << image.label << '=' << image.file.sw_part_type << ", "
         << image.file.blocks.size() << " blocks/" << bytes
         << " bytes, signature=" << image.file.signature.size() << " B";
  if (image.file.has_ecu_address) {
    output << ", ecu_address=0x" << std::uppercase << std::hex
           << image.file.ecu_address;
  }
  return output.str();
}

GeelyGeea2Image load_image(const std::filesystem::path& executable_directory,
                           const std::filesystem::path& selected,
                           std::string label, bool secondary_bootloader,
                           std::wstring signature_policy) {
  if (selected.empty()) {
    throw std::runtime_error("GEEA2 " + label + " VBF is not selected");
  }
  auto file = load_vbf(resolve_path(executable_directory, selected));
  std::transform(signature_policy.begin(), signature_policy.end(),
                 signature_policy.begin(), [](wchar_t ch) {
                   return static_cast<wchar_t>(std::towlower(ch));
                 });
  if (signature_policy == L"development") {
    if (file.signature_dev.empty()) {
      throw std::runtime_error("GEEA2 " + label +
                               " VBF has no development signature");
    }
    file.signature = file.signature_dev;
  } else if (signature_policy == L"production") {
    if (file.signature_prod.empty()) {
      throw std::runtime_error("GEEA2 " + label +
                               " VBF has no production signature");
    }
    file.signature = file.signature_prod;
  } else if (signature_policy != L"auto") {
    throw std::runtime_error(
        "GEEA2 vbf_signature_policy must be auto, development or production");
  }
  return {std::move(label), std::move(file), secondary_bootloader};
}

std::vector<GeelyGeea2Image> load_selected_images(const FlashJob& job) {
  std::vector<GeelyGeea2Image> images;
  if (!job.driver_file.empty()) {
    images.push_back(load_image(job.executable_directory, job.driver_file,
                                "SBL", true,
                                job.profile.vbf_signature_policy));
  }

  const auto mode = job.entry_mode.empty() ? std::wstring{L"app"}
                                            : job.entry_mode;
  if (mode == L"app" || mode == L"app_cal") {
    images.push_back(load_image(job.executable_directory, job.app_file,
                                "APP", false,
                                job.profile.vbf_signature_policy));
  }
  if (mode == L"cal" || mode == L"app_cal") {
    images.push_back(load_image(job.executable_directory, job.cal_file,
                                "CAL/DATA", false,
                                job.profile.vbf_signature_policy));
  }
  if (mode != L"app" && mode != L"cal" && mode != L"app_cal") {
    throw std::runtime_error(
        "Geely P146 supports APP, CAL/DATA and APP+CAL modes only");
  }
  for (const auto& image : images) {
    if (!image.secondary_bootloader && upper(image.file.sw_part_type) == "SBL") {
      throw std::runtime_error("GEEA2 " + image.label +
                               " selector contains an SBL VBF");
    }
  }
  validate_geely_geea2_images(images);
  return images;
}

} // namespace

std::wstring_view GeelyP146Workflow::id() const noexcept {
  return L"geely_p146";
}

std::string GeelyP146Workflow::report_title(const FlashProfile&) const {
  return "Geely P146 GEEA2.0 Normal Download Report";
}

void GeelyP146Workflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (job.profile.tx_id == 0U || job.profile.rx_id == 0U) {
    throw std::runtime_error(
        "Geely P146 diagnostic Tx/Rx IDs are not frozen by the public GEEA2.0 "
        "framework; configure the target ECU IDs before CAN access");
  }
  if (job.profile.security_level == 0U ||
      (job.profile.security_level & 1U) == 0U ||
      job.profile.security_level >= 0x7FU) {
    throw std::runtime_error(
        "Geely P146 SecurityAccess seed subfunction must be an odd byte");
  }
  if (job.profile.power_control || job.profile.supports_ft_entry ||
      !job.profile.supports_cal_download) {
    throw std::runtime_error(
        "Geely P146 GEEA2 profile must use external power, no FT entry and "
        "enable selectable APP/CAL downloads");
  }

  std::vector<GeelyGeea2Image> images;
  try {
    images = load_selected_images(job);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("Geely P146 VBF preflight failed before CAN access: ") +
        error.what());
  }

  std::ostringstream layout;
  for (std::size_t index = 0; index < images.size(); ++index) {
    if (index != 0U) layout << "; ";
    layout << describe(images[index]);
  }
  if (callbacks.log) {
    callbacks.log("Geely P146 VBF preflight PASS: " + layout.str());
    callbacks.log(
        "Evidence boundary: the public P146 package provides the generic "
        "GEEA2.0 SWDL state machine; diagnostic IDs, SeedKey DLL/variant and "
        "bench power/version policy remain target configuration and require "
        "P146 ECU acceptance.");
  }
  report(callbacks, "VBF preflight", "PASS", layout.str());
  report(callbacks, "Source boundary", "WARN",
         "Normal Download sequence is reproduced from P146 GEEA2.0 "
         "SWDLonCAN V7.6; target endpoints/security and ECU PASS are pending");

  const auto broker = job.executable_directory / L"keygen_broker.exe";
  const auto security_dll =
      resolve_path(job.executable_directory, job.security_dll);
  if (!std::filesystem::is_regular_file(broker)) {
    throw std::runtime_error(
        "Geely P146 x86 keygen_broker.exe is missing before CAN access");
  }
  if (security_dll.empty() ||
      !std::filesystem::is_regular_file(security_dll)) {
    throw std::runtime_error(
        "Geely P146 target SeedKey DLL is not configured before CAN access");
  }
  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }

  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});
  IsoTpConfig config;
  config.tx_id = job.profile.tx_id;
  config.rx_id = job.profile.rx_id;
  config.padding = job.profile.padding;
  config.st_min = job.profile.isotp_st_min;
  config.tx_extended = job.profile.extended_id;
  config.rx_extended = job.profile.extended_id;
  config.tx_fd = job.profile.uds_fd;
  config.tx_brs = job.profile.uds_brs;
  config.tx_data_length = job.profile.uds_fd ? 64U : 8U;
  IsoTpSession transport(*bus, config);
  UdsClient physical(
      transport,
      [&](const std::string& line) {
        if (callbacks.log) callbacks.log(line);
      },
      stop);

  GeelyGeea2Protocol protocol;
  protocol.security_seed_subfunction =
      static_cast<std::uint8_t>(job.profile.security_level);
  GeelyGeea2Flow flow(
      physical,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 TransferData")) {
          callbacks.progress(percent, line);
        }
      },
      [&](std::span<const std::uint8_t> seed, unsigned level) {
        return generate_key_x86(broker, security_dll, seed, level,
                                job.profile.security_variant);
      },
      protocol);

  try {
    flow.run(images, stop);
  } catch (...) {
    const auto warning =
        flow.core_programming_completed()
            ? "GEEA2 programming, compatibility check and ECU reset completed; "
              "confirm the P146 application is online before retrying."
            : "GEEA2 exited before compatibility check and ECU reset completed; "
              "the target ECU state is unknown.";
    if (callbacks.log) callbacks.log("WARN: " + std::string(warning));
    report(callbacks, "Failure state", "WARN", warning);
    throw;
  }
  report(callbacks, "Normal Download", "PASS",
         "GEEA2.0 configured VBF sequence completed; this run result applies "
         "only to the connected target and selected artifacts");
}

} // namespace uds
