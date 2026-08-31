#include "flash/baic_radar_workflows.hpp"

#include "core/diagnostic_endpoint.hpp"
#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/baic_radar_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uds {
namespace {

struct BaicRadarProjectSpec {
  BaicRadarProject project;
  std::wstring_view workflow_id;
  std::string_view label;
  std::uint32_t driver_start;
  std::uint32_t driver_length;
  std::uint32_t app_start;
  std::uint32_t app_length;
  bool can_fd;
};

constexpr std::array kProjectSpecs{
    BaicRadarProjectSpec{BaicRadarProject::n61ab, L"baic_n61ab",
                         "BAIC N61AB ARS1.31", 0x08000000, 0x400,
                         0xC0080000, 0xF5000, false},
    BaicRadarProjectSpec{BaicRadarProject::bqb41, L"baic_bqb41",
                         "BAIC BQB41", 0x00000000, 0x400,
                         0x00040000, 0x80000, true}};

const BaicRadarProjectSpec& project_spec(BaicRadarProject project) {
  const auto item = std::find_if(
      kProjectSpecs.begin(), kProjectSpecs.end(),
      [project](const auto& spec) { return spec.project == project; });
  if (item == kProjectSpecs.end()) {
    throw std::logic_error("unknown BAIC radar project");
  }
  return *item;
}

std::filesystem::path resolve_path(
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& selected) {
  if (selected.empty() || selected.is_absolute()) return selected;
  return executable_directory / selected;
}

void record(const FlashWorkflowCallbacks& callbacks, std::string step,
            std::string verdict, std::string detail) {
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

} // namespace

void validate_baic_configurable_endpoint(
    const FlashProfile& profile, BaicRadarProject project) {
  const auto& spec = project_spec(project);
  static_cast<void>(require_configurable_diagnostic_endpoint(
      profile.tx_id, profile.rx_id, false, spec.label));
}

BaicRadarWorkflow::BaicRadarWorkflow(BaicRadarProject project)
    : project_(project) {}

std::wstring_view BaicRadarWorkflow::id() const noexcept {
  return project_spec(project_).workflow_id;
}

std::string BaicRadarWorkflow::report_title(const FlashProfile&) const {
  return std::string(project_spec(project_).label) + " Download Report";
}

void BaicRadarWorkflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  const auto& spec = project_spec(project_);
  if (job.profile.flow != spec.workflow_id) {
    throw std::runtime_error(std::string(spec.label) +
                             " profile/workflow identity mismatch");
  }
  validate_baic_configurable_endpoint(job.profile, project_);
  if (job.profile.functional_id != 0x7DF || job.profile.extended_id ||
      job.profile.power_control || job.profile.supports_ft_entry ||
      job.profile.supports_cal_download) {
    throw std::runtime_error(std::string(spec.label) +
                             " profile violates the frozen CANoe contract");
  }
  if (job.profile.can_fd != spec.can_fd ||
      job.profile.uds_fd != spec.can_fd ||
      job.profile.uds_brs != spec.can_fd) {
    throw std::runtime_error(std::string(spec.label) +
                             (spec.can_fd ? " requires CAN FD+BRS"
                                          : " requires Classic CAN"));
  }
  if (job.profile.nominal_bitrate != 500000 ||
      job.profile.security_level != 0x11 ||
      job.profile.driver_start != spec.driver_start ||
      job.profile.driver_length != spec.driver_length ||
      job.profile.app_start != spec.app_start ||
      job.profile.app_length != spec.app_length) {
    throw std::runtime_error(std::string(spec.label) +
                             " endpoint/security/layout contract mismatch");
  }
  if (!job.entry_mode.empty() && job.entry_mode != L"app") {
    throw std::runtime_error(std::string(spec.label) +
                             " supports the normal APP Download mode only");
  }

  const auto driver_path =
      resolve_path(job.executable_directory, job.driver_file);
  const auto app_path = resolve_path(job.executable_directory, job.app_file);
  const auto dll_path = resolve_path(job.executable_directory, job.security_dll);
  if (driver_path.empty() || app_path.empty()) {
    throw std::runtime_error(std::string(spec.label) +
                             " Driver and APP S19 files must be selected "
                             "before CAN access");
  }
  const auto broker = job.executable_directory / L"keygen_broker.exe";
  if (!std::filesystem::is_regular_file(broker) ||
      !std::filesystem::is_regular_file(dll_path)) {
    throw std::runtime_error(std::string(spec.label) +
                             " x86 keygen_broker or SeedKey DLL is missing");
  }

  BaicRadarImages images;
  try {
    images.driver = load_srecord_window(driver_path, spec.driver_start,
                                         spec.driver_length);
    images.app = load_srecord_window(app_path, spec.app_start,
                                     spec.app_length);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(spec.label) +
                             " S19 preflight failed before CAN access: " +
                             error.what());
  }
  std::ostringstream preflight;
  preflight << "Driver=0x" << std::uppercase << std::hex
            << spec.driver_start << "/0x" << spec.driver_length
            << ", CRC32=0x" << baic_radar_crc32(images.driver)
            << "; APP=0x" << spec.app_start << "/0x" << spec.app_length
            << ", CRC32=0x" << baic_radar_crc32(images.app);
  record(callbacks, "S19 preflight", "PASS", preflight.str());
  if (callbacks.log) callbacks.log("BAIC S19 preflight PASS: " + preflight.str());

  const std::array<std::uint8_t, 4> reference_seed =
      project_ == BaicRadarProject::n61ab
          ? std::array<std::uint8_t, 4>{0x00, 0x00, 0x00, 0x00}
          : std::array<std::uint8_t, 4>{0x1B, 0xFE, 0x6E, 0x44};
  const std::array<std::uint8_t, 16> expected_key =
      project_ == BaicRadarProject::n61ab
          ? std::array<std::uint8_t, 16>{
                0x07, 0x1E, 0x6C, 0x5A, 0x01, 0x2E, 0xBC, 0xFE,
                0xC0, 0x5B, 0x76, 0x15, 0x7C, 0x58, 0x23, 0x64}
          : std::array<std::uint8_t, 16>{
                0x52, 0xAC, 0xA0, 0x70, 0xFE, 0x26, 0x39, 0xA9,
                0x7A, 0xF7, 0xDD, 0x5B, 0x57, 0xD8, 0x8B, 0xAD};
  const auto reference_key = generate_key_x86(
      broker, dll_path, reference_seed, job.profile.security_level,
      job.profile.security_variant);
  if (reference_key.size() != expected_key.size() ||
      !std::equal(reference_key.begin(), reference_key.end(),
                  expected_key.begin())) {
    throw std::runtime_error(std::string(spec.label) +
                             " SeedKey DLL failed its archived known-answer "
                             "test before CAN access");
  }
  record(callbacks, "SeedKey preflight", "PASS",
         "4-byte seed / 16-byte key; archived known-answer vector matched");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});
  IsoTpConfig physical_config;
  physical_config.tx_id = job.profile.tx_id;
  physical_config.rx_id = job.profile.rx_id;
  physical_config.padding = job.profile.padding;
  physical_config.st_min = job.profile.isotp_st_min;
  physical_config.tx_fd = job.profile.uds_fd;
  physical_config.tx_brs = job.profile.uds_brs;
  physical_config.tx_data_length = job.profile.uds_fd ? 64U : 8U;
  IsoTpSession physical_transport(*bus, physical_config);
  auto functional_config = physical_config;
  functional_config.tx_id = job.profile.functional_id;
  IsoTpSession functional_transport(*bus, functional_config);
  auto uds_log = [&](const std::string& line) {
    if (callbacks.log) callbacks.log(line);
    record(callbacks, "UDS", "INFO", line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);

  std::mutex keep_alive_error_mutex;
  std::string keep_alive_error;
  std::vector<std::uint8_t> tester_present(spec.can_fd ? 64U : 8U,
                                            spec.can_fd ? 0x00 : 0x55);
  tester_present[0] = 0x02;
  tester_present[1] = 0x3E;
  tester_present[2] = 0x80;
  std::jthread tester_present_sender(
      [&functional_transport, &keep_alive_error_mutex, &keep_alive_error,
       tester_present](std::stop_token sender_stop) {
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::seconds(4);
        while (!sender_stop.stop_requested()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= next) {
            try {
              functional_transport.send_raw(0x7DF, tester_present);
            } catch (const std::exception& error) {
              std::scoped_lock lock(keep_alive_error_mutex);
              keep_alive_error = error.what();
              return;
            }
            do {
              next += std::chrono::seconds(4);
            } while (next <= now);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
  record(callbacks, "TesterPresent", "INFO",
         spec.can_fd
             ? "Functional 0x7DF CAN FD 02 3E 80 + 0x00 padding @4s"
             : "Functional 0x7DF Classic CAN 02 3E 80 + 0x55 padding @4s");
  auto keygen = [broker, dll_path, level = job.profile.security_level,
                 variant = job.profile.security_variant](
                    std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll_path, seed, level, variant);
  };
  BaicRadarFlow flow(
      physical, functional,
      {std::string(spec.label), spec.driver_start, spec.driver_length,
       spec.app_start, spec.app_length, 0x11, 4, 16},
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 ")) {
          callbacks.progress(percent, line);
        }
        const auto pass = line.find(" PASS:") != std::string::npos ||
                          line.ends_with(" completed");
        record(callbacks, line, pass ? "PASS" : "INFO", line);
      },
      keygen);
  flow.run(images, stop);
  tester_present_sender.request_stop();
  tester_present_sender.join();
  {
    std::scoped_lock lock(keep_alive_error_mutex);
    if (!keep_alive_error.empty()) {
      throw std::runtime_error(std::string(spec.label) +
                               " TesterPresent sender failed: " +
                               keep_alive_error);
    }
  }
  record(callbacks, "Download", "PASS",
         std::string(spec.label) +
             " CANoe-equivalent Driver/APP sequence completed");
}

} // namespace uds
