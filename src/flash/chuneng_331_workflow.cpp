#include "flash/chuneng_331_workflow.hpp"

#include "core/canoe_power.hpp"
#include "core/flash_data.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/chuneng_331_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {
using namespace std::chrono_literals;

void record(const FlashWorkflowCallbacks& callbacks, int percent, std::string step,
            std::string verdict, std::string detail) {
  static_cast<void>(percent);
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

void log(const FlashWorkflowCallbacks& callbacks, const std::string& line) {
  if (callbacks.log) callbacks.log(line);
}

bool is_cbf_file(const std::filesystem::path& path) {
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return extension == L".cbf";
}

bool is_srecord_file(const std::filesystem::path& path) {
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) {
                   return static_cast<wchar_t>(std::towlower(character));
                 });
  return extension == L".s19" || extension == L".srec" ||
         extension == L".s28" || extension == L".s37";
}

std::string cbf_identity(const CbfImage& image) {
  std::ostringstream detail;
  detail << "target=" << image.software_id << image.software_version
         << "; software_id=" << image.software_id
         << "; software_version=" << image.software_version
         << "; software_type=" << image.software_type << "; main=0x"
         << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
         << image.main.address << "/0x" << std::setw(8)
         << image.main.data.size() << "; abt=0x" << std::setw(8)
         << image.abt.address << "/0x" << std::setw(8)
         << image.abt.data.size() << "; dev_signature=" << std::dec
         << image.device_signature.size() << " bytes";
  return detail.str();
}

} // namespace

Chuneng331InputMode resolve_chuneng_331_input_mode(
    const std::filesystem::path& driver,
    const std::filesystem::path& app) {
  const auto driver_is_cbf = is_cbf_file(driver);
  const auto app_is_cbf = is_cbf_file(app);
  if (driver_is_cbf != app_is_cbf) {
    throw std::invalid_argument(
        "ChuNeng input must be a Driver CBF + APP CBF pair or a Driver "
        "S-record + APP S-record pair; mixed CBF/S-record input is not "
        "allowed");
  }
  if (driver_is_cbf) return Chuneng331InputMode::cbf_pair;
  if (!is_srecord_file(driver) || !is_srecord_file(app)) {
    throw std::invalid_argument(
        "ChuNeng non-CBF input must use S19/SREC files for both Driver and "
        "APP");
  }
  return Chuneng331InputMode::srecord_pair;
}

std::wstring_view Chuneng331Workflow::id() const noexcept { return L"chuneng_331"; }

std::string Chuneng331Workflow::report_title(const FlashProfile&) const {
  return "ChuNeng 331 Download Report";
}

std::wstring_view ChunengArc331Workflow::id() const noexcept {
  return L"chuneng_arc331";
}

std::string ChunengArc331Workflow::report_title(const FlashProfile&) const {
  return "ChuNeng ARC331 Radar Download Report";
}

void ChunengArc331Workflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (callbacks.log) {
    callbacks.log(
        "ChuNeng ARC331 dedicated flow selected: input is an atomic "
        "Driver+APP CBF pair or Driver+APP S-record/ASC pair; both use "
        "256-byte 0202 "
        "verification, and LP 6000/6001 certificate routines are not used");
  }
  Chuneng331Workflow implementation;
  implementation.run(job, callbacks, stop);
}

void Chuneng331Workflow::run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
                             std::stop_token stop) {
  record(callbacks, 0, "Preflight", "INFO", "Loading and validating Driver/APP/verification files");
  log(callbacks, "预检查：加载并校验刷写文件……");

  Chuneng331Images images;
  const auto resolve = [&](const std::filesystem::path& path) {
    return path.is_absolute() ? path : job.executable_directory / path;
  };
  const auto input_mode =
      resolve_chuneng_331_input_mode(job.driver_file, job.app_file);
  const auto cbf_pair = input_mode == Chuneng331InputMode::cbf_pair;

  if (cbf_pair) {
    const auto driver = load_chuneng_cbf(resolve(job.driver_file));
    if (!is_supported_chuneng_driver_cbf_type(driver.software_type)) {
      throw std::runtime_error(
          "ChuNeng Driver CBF type must be EXE or SBL");
    }
    if (driver.main.address != kChuneng331CbfDriverAddress ||
        driver.main.data.size() != 0x4000U) {
      throw std::runtime_error(
          "ChuNeng Driver CBF main-data layout must be 0x10280000/0x4000");
    }
    // CBF stores the Driver source window at 0x10280000, while the ECU's
    // RequestDownload contract uses the fixed Driver transfer address 0.
    images.driver_address = 0x00000000;
    images.driver = driver.main.data;
    images.driver_verification = driver.device_signature;
    // ABT block: downloaded right after the main image (reference
    // Flash2944_CN_ARC_V1.2 flow) before CheckMemory 0202.
    images.driver_abt_address = driver.abt.address;
    images.driver_abt = driver.abt.data;
    const auto identity = cbf_identity(driver);
    log(callbacks, "Driver CBF identity: " + identity);
    record(callbacks, 0, "Driver CBF", "PASS",
           identity + "; main and ABT extracted and integrity-checked; "
           "transfer address=0x00000000; 256-byte dev_signature extracted");
  } else {
    if (job.driver_verify_file.empty() || job.app_verify_file.empty()) {
      throw std::runtime_error(
          "ChuNeng S-record mode requires both Driver verification ASC and "
          "APP verification ASC");
    }
    images.driver_address = 0x00000000;
    images.driver = load_srecord_window(resolve(job.driver_file), 0x00000000, 0x4000);
    images.driver_verification = load_asc_hex(resolve(job.driver_verify_file), 256, 256);
    record(callbacks, 0, "Driver S-record", "PASS",
           "0x00000000/0x4000 with selected 256-byte verification data");
  }

  if (cbf_pair) {
    const auto app = load_chuneng_cbf(resolve(job.app_file));
    if (!is_supported_chuneng_app_cbf_type(app.software_type)) {
      throw std::runtime_error("ChuNeng APP CBF type must be DATA or APP");
    }
    if (app.main.address != 0x000C0000U ||
        app.main.data.size() != 0x180000U) {
      throw std::runtime_error(
          "ChuNeng APP CBF main-data layout must be 0x000C0000/0x180000");
    }
    images.app = app.main.data;
    images.app_verification = app.device_signature;
    images.app_abt_address = app.abt.address;
    images.app_abt = app.abt.data;
    const auto identity = cbf_identity(app);
    log(callbacks, "APP CBF identity: " + identity);
    record(callbacks, 0, "APP CBF", "PASS",
           identity + "; main and ABT extracted and integrity-checked; "
           "source equals transfer window; 256-byte dev_signature extracted");
  } else {
    images.app = load_srecord_window(resolve(job.app_file), 0x000C0000, 0x180000);
    images.app_verification = load_asc_hex(resolve(job.app_verify_file), 256, 256);
    record(callbacks, 0, "APP S-record", "PASS",
           "0x000C0000/0x180000 with selected 256-byte verification data");
  }
  log(callbacks,
      std::string("ChuNeng ARC331 paired input preflight passed: mode=") +
          (cbf_pair ? "Driver CBF + APP CBF" :
                      "Driver S-record/ASC + APP S-record/ASC") +
          "; both roles enter the same 0202/256-byte-signature state "
          "machine");
  record(callbacks, 0, "Preflight", "PASS",
         "Files validated: Driver=0x4000, APP=0x180000, verification=256+256");
  if (stop.stop_requested()) throw std::runtime_error("operation cancelled by user");

  if (job.profile.power_control) {
    log(callbacks, "上电：写 CANoe IO::VN1600_1::DOUT=1");
    const auto power = set_canoe_dout(1);
    record(callbacks, 0, "PowerOn", "PASS",
           "IO::VN1600_1::DOUT=" + std::to_string(power.value));
    std::this_thread::sleep_for(std::chrono::seconds(1));
  } else {
    log(callbacks, "供电：保持台架现有外部供电状态");
    record(callbacks, 0, "Power", "INFO", "External power unchanged");
  }

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});
  IsoTpConfig physical_config{job.profile.tx_id, job.profile.rx_id,
                              job.profile.padding, 0,
                              job.profile.isotp_st_min};
  // ARC331 requires the 0x520 wake-up frame throughout programming.  A single
  // 0x800-byte TransferData request contains roughly 293 classic-CAN
  // Consecutive Frames; submitting them as one locked batch can starve the
  // 10 ms wake-up sender for the whole batch.  Send CFs individually so the
  // shared channel can interleave 0x520 without changing ISO-TP sequencing.
  physical_config.batch_consecutive_frames = false;
  IsoTpSession physical_transport(*bus, physical_config);
  auto functional_config = physical_config;
  functional_config.tx_id = job.profile.functional_id;
  IsoTpSession functional_transport(*bus, functional_config);
  if (job.profile.ft_tx_id == 0 || job.profile.ft_rx_id == 0) {
    throw std::runtime_error("Chuneng 331 FT endpoint 0x701/0x761 is not configured");
  }
  IsoTpSession ft_transport(
      *bus, {job.profile.ft_tx_id, job.profile.ft_rx_id,
            job.profile.ft_padding, 0, job.profile.isotp_st_min, 1000ms,
            1000ms, job.profile.ft_extended_id,
            job.profile.ft_extended_id, job.profile.ft_uds_fd,
            job.profile.ft_uds_brs});
  auto uds_log = [&](const std::string& line) {
    log(callbacks, line);
    record(callbacks, 0, "UDS", "INFO", line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);
  UdsClient ft_physical(ft_transport, uds_log, stop);
  const auto broker = job.executable_directory / L"keygen_broker.exe";
  const auto security_dll = job.security_dll.is_absolute()
                                ? job.security_dll
                                : job.executable_directory / job.security_dll;
  auto keygen = [broker, dll = security_dll,
                 level = job.profile.security_level,
                 variant = job.profile.security_variant](std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };

  Chuneng331Flow flow(physical, functional, ft_physical, physical_transport,
    functional_transport,
    [&](int percent, const std::string& line) {
      if (callbacks.progress) callbacks.progress(percent, line);
      const auto pass = line.find(" PASS:") != std::string::npos;
      record(callbacks, percent, line, pass ? "PASS" : "INFO", line);
    }, keygen);
  flow.run(images, job.entry_mode, stop);
  record(callbacks, 100, "Download", "PASS",
         "Q/CN A201-2025 compliant ChuNeng 331 sequence completed");
}

} // namespace uds
