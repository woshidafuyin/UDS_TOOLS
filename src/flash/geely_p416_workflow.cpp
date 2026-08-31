#include "flash/geely_p416_workflow.hpp"

#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "core/vbf.hpp"
#include "flash/geely_p416_flow.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

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

std::string hex_u32(std::uint32_t value) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << value;
  return output.str();
}

std::string describe(const VbfFile& file, const char* label) {
  std::size_t bytes{};
  for (const auto& block : file.blocks) bytes += block.data.size();
  return std::string(label) + "=" + std::to_string(file.blocks.size()) +
         " blocks/" + std::to_string(bytes) + " bytes, VBF CRC32=" +
         hex_u32(file.file_checksum) +
         (file.block_crc16_verified
              ? ", block CRC16=verified"
              : ", block CRC16=processed-domain (stored value retained)");
}

bool same_key(std::span<const std::uint8_t> seed,
              std::span<const std::uint8_t> expected) {
  const auto key = geely_p416_seed_key(seed);
  return std::equal(key.begin(), key.end(), expected.begin(), expected.end());
}

} // namespace

GeelyP416EndpointRouting resolve_geely_p416_endpoint_routing(
    const FlashProfile& profile) {
  const auto app = require_configurable_diagnostic_endpoint(
      profile.tx_id, profile.rx_id, false, "Geely ARS1.31L");
  const auto programming_tx = profile.programming_tx_id != 0
                                  ? profile.programming_tx_id
                                  : app.tx_id;
  const auto programming_rx = profile.programming_rx_id != 0
                                  ? profile.programming_rx_id
                                  : app.rx_id;
  const auto programming = require_configurable_diagnostic_endpoint(
      programming_tx, programming_rx, false,
      "Geely ARS1.31L SBL programming");
  return {app, programming};
}

std::wstring_view GeelyP416Workflow::id() const noexcept {
  return L"geely_p416";
}

std::string GeelyP416Workflow::report_title(const FlashProfile& profile) const {
  return profile.id == L"geely_p611"
             ? "Geely P611 ARS1.31L Download Report"
             : "Geely P416 ARS1.31L Download Report";
}

void GeelyP416Workflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  const std::string project_label =
      job.profile.id == L"geely_p611" ? "Geely P611" : "Geely P416";
  const auto endpoints =
      resolve_geely_p416_endpoint_routing(job.profile);
  if (!job.profile.can_fd || job.profile.extended_id || job.profile.uds_fd ||
      job.profile.uds_brs ||
      job.profile.functional_id != kGeelyP416AppFunctionalId ||
      (job.profile.programming_tx_id != 0 &&
       job.profile.programming_tx_id != kGeelyP416SblTxId) ||
      (job.profile.programming_rx_id != 0 &&
       job.profile.programming_rx_id != kGeelyP416SblRxId) ||
      job.profile.ft_tx_id != kGeelyP416PlsTxId ||
      job.profile.ft_rx_id != kGeelyP416PlsRxId ||
      job.profile.ft_extended_id || job.profile.ft_uds_fd ||
      job.profile.ft_uds_brs || job.profile.padding != 0x55 ||
      job.profile.ft_padding != 0x55 ||
      job.profile.nominal_bitrate != 500000 ||
      job.profile.data_bitrate != 2000000 || job.profile.isotp_st_min != 0) {
    throw std::runtime_error(
        project_label +
        " requires a CAN-FD-capable 500k/2M channel with Classic UDS, "
        "configurable APP IDs, SBL 716/616, PLS 701/761/7DF, padding 55 "
        "and STmin 0");
  }
  if (job.profile.power_control || !job.profile.supports_ft_entry ||
      job.profile.supports_cal_download) {
    throw std::runtime_error(
        project_label +
        " profile must use external power, enable PLS entry and disable "
        "generic CAL-only modes");
  }
  const auto entry_mode = resolve_geely_p416_entry_mode(job.entry_mode);

  GeelyP416Images images;
  try {
    images.sbl = load_vbf(resolve_path(job.executable_directory,
                                       job.driver_file));
    images.ess = load_vbf(resolve_path(job.executable_directory,
                                       job.cal_file));
    images.app = load_vbf(resolve_path(job.executable_directory,
                                       job.app_file));
  } catch (const std::exception& error) {
    throw std::runtime_error(project_label +
                             " VBF load failed before CAN access: " +
                             error.what());
  }
  images.ess.data_format_identifier =
      geely_p416_family_ess_data_format_identifier(
          job.profile.id, images.ess.data_format_identifier);
  const auto layout = describe(images.sbl, "SBL") + "; " +
                      describe(images.ess, "ESS") + "; " +
                      describe(images.app, "APP");
  if (callbacks.log) callbacks.log(project_label + " VBF loaded: " + layout);
  report(callbacks, "VBF load", "PASS", layout);

  constexpr std::array<std::uint8_t, 3> seed1{0x41, 0x01, 0x4D};
  constexpr std::array<std::uint8_t, 3> key1{0xFA, 0xE1, 0x9E};
  constexpr std::array<std::uint8_t, 3> seed2{0x8B, 0x7F, 0x99};
  constexpr std::array<std::uint8_t, 3> key2{0x27, 0xAB, 0x0B};
  constexpr std::array<std::uint8_t, 3> seed3{0xCB, 0x6C, 0xA9};
  constexpr std::array<std::uint8_t, 3> key3{0x24, 0x04, 0x51};
  constexpr std::array<std::uint8_t, 3> seed4{0x6A, 0x33, 0x20};
  constexpr std::array<std::uint8_t, 3> key4{0x41, 0x0A, 0x97};
  constexpr std::array<std::uint8_t, 3> seed5{0xE6, 0x32, 0x07};
  constexpr std::array<std::uint8_t, 3> key5{0x16, 0xE1, 0xCA};
  if (!same_key(seed1, key1) || !same_key(seed2, key2) ||
      !same_key(seed3, key3) || !same_key(seed4, key4) ||
      !same_key(seed5, key5)) {
    throw std::runtime_error(
        project_label +
        " built-in SeedKey self-test failed before CAN access");
  }
  report(callbacks, "SeedKey preflight", "PASS",
         "five captured APP/PLS seed-key vectors match");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});

  IsoTpConfig app_config;
  app_config.tx_id = endpoints.app.tx_id;
  app_config.rx_id = endpoints.app.rx_id;
  app_config.padding = 0x55;
  app_config.st_min = 0;
  IsoTpSession app_transport(*bus, app_config);
  auto programming_config = app_config;
  programming_config.tx_id = endpoints.programming.tx_id;
  programming_config.rx_id = endpoints.programming.rx_id;
  if (programming_config.tx_id > 0x7FFU ||
      programming_config.rx_id > 0x7FFU) {
    throw std::runtime_error(
        project_label +
        " SBL programming endpoint must use standard CAN IDs");
  }
  IsoTpSession programming_transport(*bus, programming_config);
  auto sbl_transition_config = programming_config;
  if (sbl_transition_config.rx_id != app_config.rx_id) {
    sbl_transition_config.alternate_rx_id = app_config.rx_id;
  }
  IsoTpSession sbl_transition_transport(*bus, sbl_transition_config);
  auto app_functional_config = app_config;
  app_functional_config.tx_id = kGeelyP416AppFunctionalId;
  IsoTpSession app_functional_transport(*bus, app_functional_config);
  auto pls_config = app_config;
  pls_config.tx_id = kGeelyP416PlsTxId;
  pls_config.rx_id = kGeelyP416PlsRxId;
  IsoTpSession pls_transport(*bus, pls_config);
  auto pls_functional_config = pls_config;
  pls_functional_config.tx_id = kGeelyP416PlsFunctionalId;
  IsoTpSession pls_functional_transport(*bus, pls_functional_config);

  const auto uds_log = [&](const std::string& line) {
    if (callbacks.log) callbacks.log(line);
  };
  UdsClient app_physical(app_transport, uds_log, stop);
  UdsClient sbl_transition_physical(sbl_transition_transport, uds_log, stop);
  UdsClient programming_physical(programming_transport, uds_log, stop);
  UdsClient app_functional(app_functional_transport, uds_log, stop);
  UdsClient pls_physical(pls_transport, uds_log, stop);
  UdsClient pls_functional(pls_functional_transport, uds_log, stop);

  GeelyP416Flow flow(
      app_physical, sbl_transition_physical, programming_physical,
      app_functional, pls_physical, pls_functional,
      app_transport, sbl_transition_transport,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 TransferData")) {
          callbacks.progress(percent, line);
        }
      }, {}, endpoints.app.tx_id, endpoints.app.rx_id,
      endpoints.programming.tx_id, endpoints.programming.rx_id);
  try {
    flow.run(images, entry_mode, stop);
  } catch (...) {
    const auto warning =
        flow.core_programming_completed()
            ? project_label +
                  " SBL/ESS/APP programming and ECU reset completed, but "
                  "post-reset session confirmation failed; do not "
                  "automatically reflash before confirming APP is online."
            : project_label +
                  " exited before programming, verification and ECU reset "
                  "all completed; the current ECU state is unknown.";
    if (callbacks.log) callbacks.log("WARN: " + std::string(warning));
    report(callbacks, "Failure state", "WARN", warning);
    throw;
  }
  report(callbacks, "Download", "PASS",
         entry_mode == GeelyP416EntryMode::app_to_app
             ? project_label + " APP-to-APP sequence completed"
             : project_label + " PLS-to-APP sequence completed");
}

} // namespace uds
