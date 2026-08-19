#include "flash/chery_kp31_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/chery_kp31_flow.hpp"

#include <sstream>
#include <stdexcept>
#include <string_view>

namespace uds {
namespace {

void record(const FlashWorkflowCallbacks& callbacks, int percent,
            std::string step, std::string verdict, std::string detail) {
  static_cast<void>(percent);
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

void log(const FlashWorkflowCallbacks& callbacks, const std::string& line) {
  if (callbacks.log) callbacks.log(line);
}

std::string layout_detail(const CheryKp31Layout& layout) {
  std::ostringstream out;
  out << std::hex << std::uppercase
      << "Driver=0x" << layout.driver_start << "/0x" << layout.driver_length
      << ", APP=0x" << layout.app_start << "/0x" << layout.app_length
      << ", CAL=0x" << layout.cal_start << "/0x" << layout.cal_length;
  return out.str();
}

std::string_view mode_name(CheryKp31FlashMode mode) {
  switch (mode) {
    case CheryKp31FlashMode::AppOnly:
      return "APP";
    case CheryKp31FlashMode::CalOnly:
      return "CAL";
    case CheryKp31FlashMode::AppCal:
      return "APP+CAL";
  }
  return "unknown";
}

std::string_view capl_entry(CheryKp31FlashMode mode) {
  switch (mode) {
    case CheryKp31FlashMode::AppOnly:
      return "APP/FileInit->Download";
    case CheryKp31FlashMode::CalOnly:
      return "CAL/TC_7";
    case CheryKp31FlashMode::AppCal:
      return "APPAndCAL/TC_2";
  }
  return "unknown";
}

void require_selected_file(const std::filesystem::path& path,
                           const char* label) {
  if (path.empty()) {
    throw std::runtime_error(std::string("KP31 ") + label +
                             " is not configured; select it in the UI");
  }
}

} // namespace

std::wstring_view CheryKp31Workflow::id() const noexcept {
  return L"chery_kp31";
}

std::string CheryKp31Workflow::report_title(const FlashProfile&) const {
  return "奇瑞 KP31 刷写报告";
}

void CheryKp31Workflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  const auto t1ej = job.profile.flow == L"chery_t1ej";
  const auto e0y = job.profile.flow == L"chery_e0y";
  const auto t22 = job.profile.flow == L"chery_t22";
  if (!t1ej && !e0y && !t22 && job.profile.flow != L"chery_kp31") {
    throw std::runtime_error("Chery ARS1.31 workflow received an unsupported profile");
  }
  const auto plan = resolve_chery_kp31_download_plan(job.entry_mode);
  if ((t1ej || e0y || t22) && plan.mode != CheryKp31FlashMode::AppOnly) {
    throw std::runtime_error("This Chery normal-flow profile currently supports APP mode only");
  }
  if (job.profile.can_fd || job.profile.nominal_bitrate != 500000) {
    throw std::runtime_error("Chery KP31 requires Classic CAN 500 kbit/s");
  }
  if (job.profile.functional_id != 0x7DF) {
    throw std::runtime_error(
        "Chery ARS1.31 functional diagnostic endpoint must be 0x7DF");
  }
  if (job.profile.padding != 0x55) {
    throw std::runtime_error("Chery KP31 ISO-TP padding must be 0x55");
  }
  if (job.profile.power_control) {
    throw std::runtime_error(
        "Chery KP31 uses external bench power and must not control CANoe DOUT");
  }
  if (job.profile.security_level != 0x11 ||
      !job.profile.security_variant.empty()) {
    throw std::runtime_error(
        "Chery KP31 security must use level 0x11 and an empty variant");
  }

  const CheryKp31Layout layout{job.profile.driver_start,
                               job.profile.driver_length,
                               job.profile.app_start,
                               job.profile.app_length,
                               job.profile.cal_start,
                               job.profile.cal_length};
  if (layout.driver_start != 0x08000000 || layout.driver_length != 0x400 ||
      layout.app_start != 0xC0080000 || layout.app_length != 0xF5000 ||
      layout.cal_start != 0xC0180000 || layout.cal_length != 0xC8) {
    throw std::runtime_error(
        "Chery KP31 layout must be Driver=0x08000000/0x400 and "
        "APP=0xC0080000/0xF5000 and CAL=0xC0180000/0xC8");
  }

  require_selected_file(job.driver_file, "Driver S19");
  require_selected_file(job.driver_verify_file, "Driver RSA");
  if (plan.download_app) {
    require_selected_file(job.app_file, "APP S19");
    require_selected_file(job.app_verify_file, "APP RSA");
  }
  if (plan.download_cal) {
    require_selected_file(job.cal_file, "CAL S19");
    require_selected_file(job.cal_verify_file, "CAL RSA");
  }
  require_selected_file(job.security_dll, "SeedKey DLL");

  const auto selected_mode = std::string(mode_name(plan.mode));
  record(callbacks, 0, "Preflight", "INFO",
         "Loading user-selected KP31 files for " + selected_mode + "; " +
             layout_detail(layout));
  log(callbacks,
      "预检查：按 KP31 " + selected_mode +
          " FileInit 规则加载所需 S19 与 512 字节 RSA……");

  CheryKp31Images images;
  images.driver = load_srecord_window(job.driver_file, layout.driver_start,
                                      layout.driver_length);
  images.driver_verification =
      load_hex_bytes(job.driver_verify_file, 512, 512);
  if (plan.download_app) {
    images.app = load_srecord_window(job.app_file, layout.app_start,
                                     layout.app_length);
    images.app_verification =
        load_hex_bytes(job.app_verify_file, 512, 512);
  }
  if (plan.download_cal) {
    images.cal = load_srecord_window(job.cal_file, layout.cal_start,
                                     layout.cal_length);
    images.cal_verification =
        load_hex_bytes(job.cal_verify_file, 512, 512);
  }
  record(callbacks, 0, "Preflight", "PASS",
         "Files validated for " + selected_mode + "; " +
             layout_detail(layout));
  if (stop.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }

  log(callbacks,
      "供电：KP31 CANoe 基线不调用 DOUT，保持台架现有外部供电状态。");
  record(callbacks, 0, "Power", "INFO", "External power unchanged");
  record(callbacks, 0, "CAN precondition", "INFO",
         "KP31 " + std::string(capl_entry(plan.mode)) +
             " baseline has no 0x600/0x25B/0x4B4 periodic sender");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, false, L"UDSToolCpp"});
  IsoTpSession physical_transport(
      *bus, {job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
             job.profile.isotp_st_min});
  IsoTpSession functional_transport(
      *bus, {job.profile.functional_id, job.profile.rx_id,
             job.profile.padding, 0, job.profile.isotp_st_min});
  auto uds_log = [&](const std::string& line) {
    log(callbacks, line);
    record(callbacks, 0, "UDS", "INFO", line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);

  const auto broker = job.executable_directory / L"keygen_broker.exe";
  auto keygen = [broker, dll = job.security_dll,
                 level = job.profile.security_level,
                 variant = job.profile.security_variant](
                    std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };

  CheryKp31Flow flow(
      physical, functional, layout,
      [&](int percent, const std::string& line) {
        if (callbacks.progress) callbacks.progress(percent, line);
        const auto pass = line.find(" PASS:") != std::string::npos ||
                          line.find(" completed") != std::string::npos;
        record(callbacks, percent, line, pass ? "PASS" : "INFO", line);
      },
      keygen);
  if (t1ej) {
    flow.run_t1ej_app_only(images, stop);
  } else {
    flow.run(images, plan.mode, stop);
  }
  record(callbacks, 100, selected_mode, "PASS",
         "KP31 Flash.can::maintest " + std::string(capl_entry(plan.mode)) +
             " sequence completed");
}

} // namespace uds
