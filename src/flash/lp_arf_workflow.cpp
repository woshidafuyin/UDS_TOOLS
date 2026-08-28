#include "flash/lp_arf_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/sha256.hpp"
#include "core/uds_client.hpp"
#include "flash/lp_arf_flow.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace uds {
namespace {
using namespace std::chrono_literals;

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

std::string hex_u32(std::uint32_t value) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << value;
  return output.str();
}

} // namespace

std::wstring_view LpArfWorkflow::id() const noexcept {
  return L"lp_arf";
}

std::string LpArfWorkflow::report_title(const FlashProfile&) const {
  return "Leapmotor ARF Download Report";
}

void LpArfWorkflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (!job.profile.can_fd || job.profile.extended_id ||
      job.profile.uds_fd || job.profile.uds_brs ||
      job.profile.tx_id != 0x751 || job.profile.rx_id != 0x759 ||
      job.profile.functional_id != 0x7DF ||
      job.profile.ft_tx_id != 0x701 || job.profile.ft_rx_id != 0x761 ||
      job.profile.ft_extended_id || job.profile.ft_uds_fd ||
      job.profile.ft_uds_brs || job.profile.padding != 0x55 ||
      job.profile.ft_padding != 0x55 ||
      job.profile.nominal_bitrate != 500000 ||
      job.profile.data_bitrate != 2000000 ||
      job.profile.isotp_st_min != 0 ||
      job.profile.security_level != 0x11 ||
      job.profile.app_start != kLpArfAppAddress ||
      job.profile.app_length != kLpArfAppLength ||
      job.profile.driver_length != 0) {
    throw std::runtime_error(
        "LP-ARF requires a CAN-FD-capable 500k/2M channel with Classic UDS, "
        "APP 751/759, PLS 701/761, functional 7DF, padding 55 and STmin 0");
  }
  if (job.profile.power_control) {
    throw std::runtime_error(
        "LP-ARF uses external bench power; CANoe DOUT power control must be disabled");
  }
  if (!job.profile.supports_ft_entry ||
      job.profile.supports_cal_download) {
    throw std::runtime_error(
        "LP-ARF profile must enable PLS entry and disable CAL download");
  }
  if (!job.driver_file.empty()) {
    throw std::runtime_error(
        "LP-ARF Driver must stay empty because the authoritative CANoe Download() disables Driver programming");
  }
  const auto entry_mode = resolve_lp_arf_entry_mode(job.entry_mode);

  const auto app_path = resolve_path(job.executable_directory, job.app_file);
  const auto certificate_path =
      resolve_path(job.executable_directory, job.app_verify_file);
  const auto security_dll =
      resolve_path(job.executable_directory, job.security_dll);
  const auto broker = job.executable_directory / L"keygen_broker.exe";

  LpArfArtifacts artifacts;
  try {
    artifacts = load_lp_arf_artifacts(app_path, certificate_path);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("LP-ARF file preflight failed before CAN access: ") +
        error.what());
  }
  auto& images = artifacts.images;
  if (images.app.address != kLpArfAppAddress ||
      images.app.data.size() != kLpArfAppLength) {
    throw std::runtime_error(
        "LP-ARF S19 must resolve to APP 000C0000/180000");
  }

  const auto app_crc = lingpao_radar_crc32(images.app.data);
  const auto app_hash = sha256(images.app.data);
  const auto verification_layout =
      "; certificate=1322 bytes loaded; 31 01 60 00 and 31 01 60 01 "
      "require CANoe-baseline positive responses; certificate-source=" +
      std::string(artifacts.certificate_embedded ? "TMP embedded" :
                                                   "external file") +
      "; ";
  const auto layout =
      "APP=" + hex_u32(images.app.address) + "/" +
      hex_u32(static_cast<std::uint32_t>(images.app.data.size())) +
      ", CRC32=" + hex_u32(app_crc) + ", SHA-256=" + to_hex(app_hash) +
      verification_layout +
      "Driver=disabled by CANoe baseline";
  if (callbacks.log) callbacks.log("LP-ARF package preflight complete: " + layout);
  report(callbacks, "File preflight", "PASS", layout);

  const auto keygen =
      [broker, security_dll, variant = job.profile.security_variant](
          std::span<const std::uint8_t> seed, unsigned level) {
        return generate_key_x86(broker, security_dll, seed, level, variant);
      };
  report(
      callbacks, "Acceptance boundary", "WARN",
      "Unified ARF entry covers A12/B11 ARF2.31 and ARF6.31. The flashing "
      "service sequence requires a 1322-byte embedded or external "
      "Certificate, sends 31 01 60 00 followed by 31 01 60 01, and requires "
      "the expected positive response for both routines. C++ bench "
      "acceptance remains separate for each ECU variant");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});

  IsoTpConfig app_config{
      job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
      job.profile.isotp_st_min, 1000ms, 1000ms,
      job.profile.extended_id, job.profile.extended_id,
      job.profile.uds_fd, job.profile.uds_brs};
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
  pls_config.padding = job.profile.ft_padding;
  IsoTpSession pls_transport(*bus, pls_config);

  const auto uds_log = [&](const std::string& line) {
    if (callbacks.log) callbacks.log(line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient app_functional(app_functional_transport, uds_log, stop);
  UdsClient pls_functional(pls_functional_transport, uds_log, stop);

  LpArfFlow flow(
      physical, app_functional, pls_functional, physical_transport,
      pls_transport, app_functional_transport,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 TransferData")) {
          callbacks.progress(percent, line);
        }
      },
      keygen);
  try {
    flow.run(images, entry_mode, stop);
  } catch (...) {
    const auto warning =
        flow.core_programming_completed()
            ? "LP-ARF APP programming and ECU reset completed, but post-reset cleanup did not complete; do not automatically reflash before confirming APP is online."
            : "LP-ARF exited before APP programming, verification and ECU reset all completed; the current ECU state is unknown.";
    if (callbacks.log) callbacks.log("WARN: " + std::string(warning));
    report(callbacks, "Failure state", "WARN", warning);
    throw;
  }
  report(callbacks, "Download", "PASS",
         entry_mode == LpArfEntryMode::app_to_app
             ? "LP-ARF APP-to-APP sequence completed"
             : "LP-ARF PLS-to-APP sequence completed");
}

} // namespace uds
