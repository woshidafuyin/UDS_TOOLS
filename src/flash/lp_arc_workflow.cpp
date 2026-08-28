#include "flash/lp_arc_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/lp_arc_flow.hpp"

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
    callbacks.report(std::move(step), std::move(verdict),
                     std::move(detail));
  }
}

std::string hex_u32(std::uint32_t value) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << value;
  return output.str();
}

} // namespace

LpArcWorkflow::LpArcWorkflow()
    : RadarS19Workflow(L"lp_arc", "LP-ARC",
                       "Leapmotor LP-ARC Download Report", true) {}

RadarS19Workflow::RadarS19Workflow(std::wstring workflow_id,
                                   std::string project_name,
                                   std::string report_name,
                                   bool send_raw_boot_transition)
    : workflow_id_(std::move(workflow_id)),
      project_name_(std::move(project_name)),
      report_name_(std::move(report_name)),
      send_raw_boot_transition_(send_raw_boot_transition) {}

std::wstring_view RadarS19Workflow::id() const noexcept {
  return workflow_id_;
}

std::string RadarS19Workflow::report_title(const FlashProfile&) const {
  return report_name_;
}

void RadarS19Workflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (!job.profile.can_fd || job.profile.extended_id ||
      job.profile.uds_fd || job.profile.uds_brs ||
      job.profile.functional_id != 0x7DF ||
      job.profile.ft_tx_id != 0x701 ||
      job.profile.ft_rx_id != 0x761 ||
      job.profile.ft_extended_id || job.profile.ft_uds_fd ||
      job.profile.ft_uds_brs || job.profile.padding != 0x55 ||
      job.profile.ft_padding != 0x55 ||
      job.profile.nominal_bitrate != 500000 ||
      job.profile.isotp_st_min != 0) {
    throw std::runtime_error(
        project_name_ +
        " requires a CAN-FD-capable 500k/2M channel with Classic UDS, "
        "PLS 701/761, functional 7DF, padding 55 and STmin 0");
  }
  if (job.profile.power_control) {
    throw std::runtime_error(
        project_name_ +
        " uses external bench power; CANoe DOUT power control must be disabled");
  }
  if (!job.profile.supports_ft_entry ||
      job.profile.supports_cal_download) {
    throw std::runtime_error(
        project_name_ +
        " profile must enable PLS entry and disable CAL download");
  }
  const auto entry_mode = resolve_lp_arc_entry_mode(job.entry_mode);
  const auto certificate_provided = !job.app_verify_file.empty();
  auto radar_spec = send_raw_boot_transition_
                        ? lp_arc_radar_spec(job.profile)
                        : chuneng_arc331_radar_spec(job.profile);

  const auto driver_path =
      resolve_path(job.executable_directory, job.driver_file);
  const auto app_path =
      resolve_path(job.executable_directory, job.app_file);
  const auto certificate_path =
      resolve_path(job.executable_directory, job.app_verify_file);
  const auto security_dll =
      resolve_path(job.executable_directory, job.security_dll);
  const auto broker = job.executable_directory / L"keygen_broker.exe";

  LpArcImages images;
  try {
    images.driver = load_single_srecord_segment(driver_path);
    images.app = load_single_srecord_segment(app_path);
    if (certificate_provided) {
      images.certificate =
          load_asc_hex(certificate_path, kLpArcCertificateLength,
                       kLpArcCertificateLength);
    } else if (!send_raw_boot_transition_) {
      throw std::runtime_error(project_name_ +
                               " requires a 1322-byte certificate file");
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string(
            project_name_ + " file preflight failed before CAN access: ") +
        error.what());
  }
  if (images.driver.address != kLpArcDriverAddress ||
      images.driver.data.size() != kLpArcDriverLength ||
      images.app.address != kLpArcAppAddress ||
      images.app.data.size() != kLpArcAppLength) {
    throw std::runtime_error(
        project_name_ + " S19 auto-analysis does not match the required "
        "Driver 00000000/4000 and APP 000C0000/180000 layout");
  }

  const auto driver_crc = lp_arc_crc32(images.driver.data);
  const auto app_crc = lp_arc_crc32(images.app.data);
  const auto layout =
      "Driver=" + hex_u32(images.driver.address) + "/" +
      hex_u32(static_cast<std::uint32_t>(images.driver.data.size())) +
      ", CRC32=" + hex_u32(driver_crc) + "; APP=" +
      hex_u32(images.app.address) + "/" +
      hex_u32(static_cast<std::uint32_t>(images.app.data.size())) +
      ", CRC32=" + hex_u32(app_crc) +
      (certificate_provided
           ? "; certificate=1322 bytes; 6000/6001 require positive responses"
           : "; certificate=not selected; CANoe-style Skip omits 6000/6001");
  if (callbacks.log) {
    callbacks.log(project_name_ + " S19 auto-analysis complete: " + layout);
  }
  report(callbacks, "File preflight", "PASS", layout);
  const auto keygen =
      [broker, security_dll, variant = job.profile.security_variant](
          std::span<const std::uint8_t> seed, unsigned level) {
        return generate_key_x86(broker, security_dll, seed, level,
                                variant);
      };

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});

  IsoTpConfig app_config{
      job.profile.tx_id, job.profile.rx_id, 0x55, 0, 0, 1000ms, 1000ms,
      false, false, false, false};
  IsoTpSession physical_transport(*bus, app_config);
  auto app_functional_config = app_config;
  app_functional_config.tx_id = 0x7DF;
  IsoTpSession app_functional_transport(*bus, app_functional_config);
  auto pls_functional_config = app_functional_config;
  pls_functional_config.rx_id = 0x761;
  IsoTpSession pls_functional_transport(*bus, pls_functional_config);
  auto pls_config = app_config;
  pls_config.tx_id = 0x701;
  pls_config.rx_id = 0x761;
  IsoTpSession pls_transport(*bus, pls_config);

  const auto uds_log = [&](const std::string& line) {
    if (callbacks.log) callbacks.log(line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient app_functional(app_functional_transport, uds_log, stop);
  UdsClient pls_functional(pls_functional_transport, uds_log, stop);

  LpArcFlow flow(
      physical, app_functional, pls_functional, physical_transport,
      pls_transport, app_functional_transport,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress &&
            !line.starts_with("36 TransferData")) {
          callbacks.progress(percent, line);
        }
      },
      keygen, std::move(radar_spec));
  try {
    flow.run(images, entry_mode, stop);
  } catch (...) {
    const auto warning =
        flow.core_programming_completed()
            ? project_name_ +
              " Driver/APP programming and ECU reset completed, "
              "but post-reset cleanup did not complete; do not "
              "automatically reflash before confirming APP is online."
            : project_name_ +
              " exited before programming, verification and ECU "
              "reset all completed; the current ECU state is unknown.";
    if (callbacks.log) callbacks.log("WARN: " + std::string(warning));
    report(callbacks, "Failure state", "WARN", warning);
    throw;
  }
  report(callbacks, "Download", "PASS",
         entry_mode == LpArcEntryMode::app_to_app
             ? project_name_ + " APP-to-APP sequence completed"
             : project_name_ + " PLS-to-APP sequence completed");
}

} // namespace uds
