#include "flash/lp_a12ev_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/lingpao_radar_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace uds {
namespace {
using namespace std::chrono_literals;

constexpr std::uint32_t kDriverAddress{0x00000000};
constexpr std::uint32_t kDriverLength{0x00004000};
constexpr std::uint32_t kAppAddress{0x000C0000};
constexpr std::uint32_t kAppLength{0x00180000};
constexpr std::size_t kBlockLength{0x802};
constexpr std::size_t kCertificateLength{1322};

std::filesystem::path resolve_path(
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& selected) {
  if (selected.empty() || selected.is_absolute()) return selected;
  return executable_directory / selected;
}

void report(const FlashWorkflowCallbacks& callbacks, std::string step,
            std::string verdict, std::string detail) {
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict),
                     std::move(detail));
  }
}

bool same_bytes(std::span<const std::uint8_t> left,
                std::span<const std::uint8_t> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

LingpaoRadarSpec lp_a12ev_radar_spec(const FlashProfile& profile) {
  LingpaoRadarSpec spec{
      "LP-A12EV", profile.tx_id, profile.rx_id, profile.ft_tx_id,
      profile.ft_rx_id, profile.functional_id, kAppAddress, kAppLength,
      kDriverAddress, kDriverLength, kBlockLength, kCertificateLength};
  spec.raw_boot_transition_tx_id = 0x771;
  return spec;
}

std::wstring_view LpA12evWorkflow::id() const noexcept {
  return L"lp_a12ev";
}

std::string LpA12evWorkflow::report_title(const FlashProfile&) const {
  return "Leapmotor LP-A12EV Download Report";
}

void LpA12evWorkflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (job.profile.placeholder) {
    throw std::runtime_error(
        "LP-A12EV profile is intentionally blocked until the matching APP S19 "
        "and ASC certificate are packaged and verified");
  }
  if (!job.profile.can_fd || job.profile.extended_id || job.profile.uds_fd ||
      job.profile.uds_brs ||
      job.profile.functional_id != 0x7DF || job.profile.ft_tx_id != 0x701 ||
      job.profile.ft_rx_id != 0x761 || job.profile.ft_extended_id ||
      job.profile.ft_uds_fd || job.profile.ft_uds_brs ||
      job.profile.padding != 0x55 || job.profile.ft_padding != 0x55 ||
      job.profile.nominal_bitrate != 500000 || job.profile.isotp_st_min != 0) {
    throw std::runtime_error(
        "LP-A12EV requires a CAN-FD-capable 500k/2M channel with Classic UDS, "
        "PLS 701/761, functional 7DF, padding 55 "
        "and STmin 0");
  }
  if (job.profile.power_control) {
    throw std::runtime_error(
        "LP-A12EV uses external bench power; CANoe DOUT power control must be disabled");
  }
  if (!job.profile.supports_ft_entry || job.profile.supports_cal_download) {
    throw std::runtime_error(
        "LP-A12EV profile must enable PLS entry and disable CAL download");
  }

  const auto entry_mode =
      resolve_lingpao_radar_entry_mode(job.entry_mode, "LP-A12EV");
  const auto driver_path =
      resolve_path(job.executable_directory, job.driver_file);
  const auto app_path = resolve_path(job.executable_directory, job.app_file);
  const auto certificate_path =
      resolve_path(job.executable_directory, job.app_verify_file);
  const auto security_dll =
      resolve_path(job.executable_directory, job.security_dll);
  const auto broker = job.executable_directory / L"keygen_broker.exe";

  LingpaoRadarImages images;
  try {
    images.driver = load_single_srecord_segment(driver_path);
    images.app = load_single_srecord_segment(app_path);
    images.certificate =
        load_asc_hex(certificate_path, kCertificateLength, kCertificateLength);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("LP-A12EV file preflight failed before CAN access: ") +
        error.what());
  }
  if (images.driver.address != kDriverAddress ||
      images.driver.data.size() != kDriverLength ||
      images.app.address != kAppAddress || images.app.data.size() != kAppLength) {
    throw std::runtime_error(
        "LP-A12EV S19 auto-analysis does not match Driver "
        "00000000/4000 and APP 000C0000/180000");
  }
  report(callbacks, "File preflight", "PASS",
         "Driver=0x00000000/0x00004000; APP=0x000C0000/0x00180000; "
         "certificate=1322 bytes");

  const auto keygen =
      [broker, security_dll, variant = job.profile.security_variant](
          std::span<const std::uint8_t> seed, unsigned level) {
        return generate_key_x86(broker, security_dll, seed, level, variant);
      };
  constexpr std::array<std::uint8_t, 4> kSeed1{0xFF, 0xFD, 0x13, 0xDE};
  constexpr std::array<std::uint8_t, 4> kKey1{0xC0, 0x82, 0x85, 0x73};
  constexpr std::array<std::uint8_t, 4> kSeed2{0xFF, 0xFD, 0x03, 0xD0};
  constexpr std::array<std::uint8_t, 4> kKey2{0x14, 0x07, 0x37, 0x0F};
  try {
    if (!same_bytes(keygen(kSeed1, 0x11), kKey1) ||
        !same_bytes(keygen(kSeed2, 0x11), kKey2)) {
      throw std::runtime_error("captured SeedKey vectors do not match");
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("LP-A12EV SeedKey DLL/x86 broker preflight failed before "
                    "CAN access: ") +
        error.what());
  }
  report(callbacks, "SeedKey preflight", "PASS",
         "FFFD13DE->C0828573 and FFFD03D0->1407370F");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});

  IsoTpConfig app_config{job.profile.tx_id, job.profile.rx_id, 0x55, 0, 0,
                         1000ms, 1000ms, false, false, false, false};
  IsoTpSession physical_transport(*bus, app_config);
  auto app_functional_config = app_config;
  app_functional_config.tx_id = job.profile.functional_id;
  IsoTpSession app_functional_transport(*bus, app_functional_config);
  auto pls_functional_config = app_functional_config;
  pls_functional_config.rx_id = job.profile.ft_rx_id;
  IsoTpSession pls_functional_transport(*bus, pls_functional_config);
  auto pls_config = app_config;
  pls_config.tx_id = job.profile.ft_tx_id;
  pls_config.rx_id = job.profile.ft_rx_id;
  IsoTpSession pls_transport(*bus, pls_config);

  const auto uds_log = [&](const std::string& line) {
    if (callbacks.log) callbacks.log(line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient app_functional(app_functional_transport, uds_log, stop);
  UdsClient pls_functional(pls_functional_transport, uds_log, stop);
  LingpaoRadarFlow flow(
      physical, app_functional, pls_functional, physical_transport,
      pls_transport, app_functional_transport,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 TransferData")) {
          callbacks.progress(percent, line);
        }
      },
      keygen, lp_a12ev_radar_spec(job.profile));
  try {
    flow.run(images, entry_mode, stop);
  } catch (...) {
    const auto warning =
        flow.core_programming_completed()
            ? "LP-A12EV Driver/APP programming and ECU reset completed, but "
              "post-reset cleanup did not complete; do not automatically reflash "
              "before confirming APP is online."
            : "LP-A12EV exited before programming, verification and ECU reset "
              "all completed; the current ECU state is unknown.";
    if (callbacks.log) callbacks.log("WARN: " + std::string(warning));
    report(callbacks, "Failure state", "WARN", warning);
    throw;
  }
  report(callbacks, "Download", "PASS",
         entry_mode == LingpaoRadarEntryMode::app_to_app
             ? "LP-A12EV APP-to-APP sequence completed"
             : "LP-A12EV PLS-to-APP sequence completed");
}

} // namespace uds
