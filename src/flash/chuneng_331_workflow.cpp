#include "flash/chuneng_331_workflow.hpp"

#include "core/canoe_power.hpp"
#include "core/flash_data.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/sha256.hpp"
#include "core/uds_client.hpp"
#include "flash/chuneng_331_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {
using namespace std::chrono_literals;

void record(const FlashWorkflowCallbacks& callbacks, int percent, std::string step,
            std::string verdict, std::string detail,
            FlashStage stage = FlashStage::unspecified,
            std::optional<std::uint8_t> uds_service = {},
            FlashImageRole image_role = FlashImageRole::none) {
  static_cast<void>(percent);
  if (stage != FlashStage::unspecified && callbacks.event) {
    callbacks.event({{}, 0, stage, uds_service, image_role, std::move(step),
                     std::move(verdict), std::move(detail)});
    return;
  }
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

std::uint32_t read_be32(std::span<const std::uint8_t> bytes,
                        std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::string hex_u32(std::uint32_t value) {
  std::ostringstream stream;
  stream << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << value;
  return stream.str();
}

} // namespace

std::filesystem::path chuneng_331_abt_sidecar_path(
    const std::filesystem::path& verification_path) {
  auto stem = verification_path.stem().wstring();
  const auto suffix = std::wstring_view{L"_Ver"};
  if (stem.size() < suffix.size() ||
      stem.substr(stem.size() - suffix.size()) != suffix) {
    throw std::invalid_argument(
        "ChuNeng S-record verification sidecar must be named *_Ver.asc so "
        "the paired *_ABT.asc can be resolved without mixing package sources");
  }
  stem.replace(stem.size() - suffix.size(), suffix.size(), L"_ABT");
  return verification_path.parent_path() / (stem + L".asc");
}

Chuneng331AbtMetadata validate_chuneng_331_abt(
    std::span<const std::uint8_t> abt,
    std::span<const std::uint8_t> image) {
  if (abt.size() != 0x2CU || abt[0] != 0x00U || abt[1] != 0x00U ||
      abt[2] != 0x00U || abt[3] != 0x01U) {
    throw std::runtime_error(
        "ChuNeng ABT must be 44 bytes and start with hash-type/count "
        "00 00 00 01");
  }
  const auto metadata =
      Chuneng331AbtMetadata{read_be32(abt, 4U), read_be32(abt, 8U)};
  if (metadata.image_length != image.size()) {
    throw std::runtime_error(
        "ChuNeng ABT image length does not match the selected S-record");
  }
  const auto digest = sha256(image);
  if (!std::equal(digest.begin(), digest.end(), abt.begin() + 12)) {
    throw std::runtime_error(
        "ChuNeng ABT SHA-256 does not match the selected S-record; do not "
        "mix Driver/APP artifacts from different CBF packages");
  }
  return metadata;
}

Chuneng331InputMode resolve_chuneng_331_input_mode(
    const std::filesystem::path& driver,
    const std::filesystem::path& app) {
  const auto driver_is_cbf = is_cbf_file(driver);
  const auto app_is_cbf = is_cbf_file(app);
  if (driver_is_cbf && app_is_cbf) {
    return Chuneng331InputMode::cbf_pair;
  }
  if (driver_is_cbf && is_srecord_file(app)) {
    return Chuneng331InputMode::driver_cbf_app_srecord;
  }
  if (is_srecord_file(driver) && is_srecord_file(app)) {
    return Chuneng331InputMode::srecord_pair;
  }
  if (is_srecord_file(driver) && app_is_cbf) {
    throw std::invalid_argument(
        "ChuNeng Driver S-record + APP CBF input is not supported");
  }
  throw std::invalid_argument(
      "ChuNeng Driver/APP input must use CBF or S19/SREC files");
}

std::wstring_view ChunengArc331Workflow::id() const noexcept {
  return L"chuneng_arc331";
}

std::string ChunengArc331Workflow::report_title(const FlashProfile&) const {
  return "ChuNeng ARC331 Radar Download Report";
}

void ChunengArc331Workflow::run(const FlashJob& job,
                                const FlashWorkflowCallbacks& callbacks,
                                std::stop_token stop) {
  if (callbacks.log) {
    callbacks.log(
        "ChuNeng ARC331 dedicated flow selected: input may be Driver+APP "
        "CBF, Driver CBF + APP S-record/ASC, or Driver+APP S-record/ASC; "
        "both roles use 256-byte 0202 "
        "verification, and LP 6000/6001 certificate routines are not used");
  }
  record(callbacks, 0, "Preflight", "INFO",
         "Loading and validating Driver/APP/verification files",
         FlashStage::pre_flash_check);
  log(callbacks, "预检查：加载并校验刷写文件……");

  Chuneng331Images images;
  const auto resolve = [&](const std::filesystem::path& path) {
    return path.is_absolute() ? path : job.executable_directory / path;
  };
  const auto input_mode =
      resolve_chuneng_331_input_mode(job.driver_file, job.app_file);
  const auto driver_is_cbf =
      input_mode != Chuneng331InputMode::srecord_pair;
  const auto app_is_cbf = input_mode == Chuneng331InputMode::cbf_pair;

  if (driver_is_cbf) {
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
    // Match CN2944LC_Flash.cfg: RequestDownload uses the parsed CBF address.
    images.driver_address = driver.main.address;
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
           "transfer address=" + hex_u32(images.driver_address) +
           "; 256-byte dev_signature extracted",
           FlashStage::configuration, {}, FlashImageRole::driver);
  } else {
    if (job.driver_verify_file.empty()) {
      throw std::runtime_error(
          "ChuNeng Driver S-record requires a Driver verification ASC");
    }
    const auto driver_verification_path = resolve(job.driver_verify_file);
    const auto driver_abt_path =
        chuneng_331_abt_sidecar_path(driver_verification_path);
    images.driver_abt = load_asc_hex(driver_abt_path, 0x2C, 0x2C);
    const auto driver_source_address = read_be32(images.driver_abt, 4U);
    images.driver_address = 0x00000000;
    images.driver = load_srecord_window(resolve(job.driver_file),
                                        driver_source_address, 0x4000);
    const auto driver_abt =
        validate_chuneng_331_abt(images.driver_abt, images.driver);
    images.driver_abt_address = 0x000C0000U;
    images.driver_verification =
        load_asc_hex(driver_verification_path, 256, 256);
    log(callbacks, "Driver S-record ABT sidecar: " +
                       driver_abt_path.string());
    record(callbacks, 0, "Driver S-record", "PASS",
           "source=" + hex_u32(driver_abt.source_address) +
               "/0x4000; transfer=0x00000000; ABT SHA-256 matched; "
               "256-byte verification data loaded",
           FlashStage::configuration, {}, FlashImageRole::driver);
  }

  if (app_is_cbf) {
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
           "source equals transfer window; 256-byte dev_signature extracted",
           FlashStage::configuration, {}, FlashImageRole::app);
  } else {
    const auto use_mixed_fallback =
        input_mode == Chuneng331InputMode::driver_cbf_app_srecord &&
        job.app_verify_file.empty();
    if (use_mixed_fallback) {
      constexpr auto kPackagedAppCbf =
          L"resources/chuneng_d7_arc331_zip/CBF/APP/7052A5023002AB.cbf";
      const auto fallback =
          load_chuneng_cbf(job.executable_directory / kPackagedAppCbf);
      if (!is_supported_chuneng_app_cbf_type(fallback.software_type) ||
          fallback.main.address != 0x000C0000U ||
          fallback.main.data.size() != 0x180000U) {
        throw std::runtime_error(
            "ChuNeng packaged APP CBF fallback has an unsupported layout");
      }
      images.app = load_srecord_window(resolve(job.app_file),
                                       0x000C0000U, 0x180000U);
      images.app_verification = fallback.device_signature;
      images.app_abt_address = fallback.abt.address;
      images.app_abt = fallback.abt.data;
      log(callbacks,
          "WARNING: APP S-record verification ASC is empty; using packaged "
          "APP CBF ABT/signature without local S-record binding validation; "
          "ECU RoutineControl 0202 decides compatibility");
      record(callbacks, 0, "APP S-record", "WARN",
             "APP window loaded; packaged APP CBF ABT and 256-byte signature "
             "used without local hash binding; ECU 0202 remains authoritative",
             FlashStage::configuration, {}, FlashImageRole::app);
    } else {
      if (job.app_verify_file.empty()) {
        throw std::runtime_error(
            "ChuNeng APP S-record requires an APP verification ASC");
      }
      const auto app_verification_path = resolve(job.app_verify_file);
      const auto app_abt_path =
          chuneng_331_abt_sidecar_path(app_verification_path);
      images.app_abt = load_asc_hex(app_abt_path, 0x2C, 0x2C);
      const auto app_source_address = read_be32(images.app_abt, 4U);
      images.app = load_srecord_window(resolve(job.app_file),
                                       app_source_address, 0x180000);
      const auto app_abt =
          validate_chuneng_331_abt(images.app_abt, images.app);
      images.app_abt_address = 0x000C0000U;
      images.app_verification =
          load_asc_hex(app_verification_path, 256, 256);
      log(callbacks, "APP S-record ABT sidecar: " + app_abt_path.string());
      record(callbacks, 0, "APP S-record", "PASS",
             "source=" + hex_u32(app_abt.source_address) +
                 "/0x180000; ABT SHA-256 matched; 256-byte verification "
                 "data loaded",
             FlashStage::configuration, {}, FlashImageRole::app);
    }
  }
  log(callbacks,
      std::string("ChuNeng ARC331 input preflight passed: mode=") +
          (input_mode == Chuneng331InputMode::cbf_pair
               ? "Driver CBF + APP CBF"
               : input_mode == Chuneng331InputMode::driver_cbf_app_srecord
                     ? "Driver CBF + APP S-record/ASC"
                     : "Driver S-record/ASC + APP S-record/ASC") +
          "; both roles enter the same 0202/256-byte-signature state "
          "machine");
  record(callbacks, 0, "Preflight", "PASS",
         "Files validated: Driver=0x4000+ABT, APP=0x180000+ABT, "
         "verification=256+256", FlashStage::pre_flash_check);
  if (stop.stop_requested()) throw std::runtime_error("operation cancelled by user");

  if (job.profile.power_control) {
    log(callbacks, "上电：写 CANoe IO::VN1600_1::DOUT=1");
    const auto power = set_canoe_dout(1);
    record(callbacks, 0, "PowerOn", "PASS",
           "IO::VN1600_1::DOUT=" + std::to_string(power.value),
           FlashStage::pre_flash_check);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  } else {
    log(callbacks, "供电：保持台架现有外部供电状态");
    record(callbacks, 0, "Power", "INFO", "External power unchanged",
           FlashStage::pre_flash_check);
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
    throw std::runtime_error(
        "Chuneng ARC331 FT endpoint 0x701/0x761 is not configured");
  }
  const auto ft_endpoints = chuneng_331_ft_transition_endpoints(
      job.profile.ft_tx_id, job.profile.ft_rx_id, physical_config.rx_id);
  IsoTpConfig ft_config{
      ft_endpoints.request_id, ft_endpoints.pending_response_id,
      job.profile.ft_padding, 0, job.profile.isotp_st_min, 1000ms,
      1000ms, job.profile.ft_extended_id,
      job.profile.ft_extended_id, job.profile.ft_uds_fd,
      job.profile.ft_uds_brs};
  // ARC331 FT 10 02 first reports NRC 0x78 on 0x761, then switches
  // its final 50 02 to the selected radar's APP response ID (0x72D/0x72F).
  // Limit the alternate endpoint to the selected target so a wrong-side radar
  // cannot silently qualify the flash job.
  if (ft_endpoints.pending_response_id != ft_endpoints.final_response_id) {
    ft_config.alternate_rx_id = ft_endpoints.final_response_id;
  }
  IsoTpSession ft_transport(*bus, ft_config);
  auto ft_functional_config = ft_config;
  ft_functional_config.tx_id = job.profile.functional_id;
  ft_functional_config.alternate_rx_id = 0;
  IsoTpSession ft_functional_transport(*bus, ft_functional_config);
  auto uds_log = [&](const std::string& line) {
    log(callbacks, line);
    record(callbacks, 0, "UDS", "INFO", line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);
  UdsClient ft_physical(ft_transport, uds_log, stop);
  UdsClient ft_functional(ft_functional_transport, uds_log, stop);
  const auto broker = job.executable_directory / L"keygen_broker.exe";
  const auto security_dll = job.security_dll.is_absolute()
                                ? job.security_dll
                                : job.executable_directory / job.security_dll;
  auto keygen = [broker, dll = security_dll,
                 level = job.profile.security_level,
                 variant = job.profile.security_variant](std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };

  Chuneng331Flow flow(physical, functional, ft_physical, ft_functional,
    physical_transport,
    functional_transport,
    [&](int percent, const std::string& line) {
      if (callbacks.progress) callbacks.progress(percent, line);
      const auto pass = line.find(" PASS:") != std::string::npos;
      const auto fail = line.find(" FAIL:") != std::string::npos;
      const auto warning = line.starts_with("WARNING:");
      record(callbacks, percent, line,
             fail ? "FAIL" : pass ? "PASS" : warning ? "WARN" : "INFO", line);
    }, keygen);
  flow.run(images, job.entry_mode, stop);
  record(callbacks, 100, "Download", "PASS",
         "Q/CN A201-2025 compliant ChuNeng ARC331 sequence completed",
         FlashStage::cycle_overview);
}

} // namespace uds
