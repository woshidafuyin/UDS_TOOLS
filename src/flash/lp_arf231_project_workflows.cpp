#include "flash/lp_arf231_project_workflows.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/sha256.hpp"
#include "core/uds_client.hpp"
#include "flash/lingpao_radar_flow.hpp"
#include "flash/lp_arf_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <stdexcept>

namespace uds {
namespace {
using namespace std::chrono_literals;

struct ProjectContract {
  std::wstring_view id;
  std::string name;
};

const ProjectContract kA12{L"lp_arf231_a12", "Leapmotor A12 ARF2.31"};
const ProjectContract kB11{L"lp_arf231_b11", "Leapmotor B11 ARF2.31"};

std::filesystem::path resolve(const FlashJob& job,
                              const std::filesystem::path& selected) {
  if (selected.empty() || selected.is_absolute()) return selected;
  return job.executable_directory / selected;
}

void report(const FlashWorkflowCallbacks& callbacks, std::string step,
            std::string verdict, std::string detail) {
  if (callbacks.report) callbacks.report(std::move(step), std::move(verdict),
                                         std::move(detail));
}

bool same(std::span<const std::uint8_t> left,
          std::span<const std::uint8_t> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

SRecordSegment load_app(const std::filesystem::path& path) {
  if (path.empty()) throw std::runtime_error("select the project APP S19/BIN");
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t value) { return std::towlower(value); });
  if (extension == L".bin") {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open APP BIN");
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    if (data.size() != kLpArfAppLength) {
      throw std::runtime_error("ARF2.31 APP BIN must be exactly 0x180000 bytes");
    }
    return {kLpArfAppAddress, std::move(data)};
  }
  return {kLpArfAppAddress,
          load_srecord_window(path, kLpArfAppAddress, kLpArfAppLength)};
}

std::vector<std::uint8_t> load_certificate(
    const std::filesystem::path& path) {
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t value) { return std::towlower(value); });
  if (extension != L".tmp") {
    return load_asc_hex(path, kLpArfCertificateLength,
                        kLpArfCertificateLength);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open Leapmotor TMP package");
  std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(input)), {});
  constexpr std::array<std::uint8_t, 4> magic{'L', 'E', 'A', 'P'};
  if (package.size() <= kLpArfCertificateLength + 8 ||
      !std::equal(magic.begin(), magic.end(), package.begin())) {
    throw std::runtime_error("invalid Leapmotor TMP package header/length");
  }
  return {package.end() - static_cast<std::ptrdiff_t>(kLpArfCertificateLength),
          package.end()};
}

void run_project(const ProjectContract& contract, const FlashJob& job,
                 const FlashWorkflowCallbacks& callbacks,
                 std::stop_token stop) {
  if (job.profile.flow != contract.id) {
    throw std::runtime_error(contract.name + " workflow/profile mismatch");
  }
  if (!job.profile.can_fd || job.profile.extended_id || job.profile.uds_fd ||
      job.profile.uds_brs || job.profile.power_control ||
      !job.profile.supports_ft_entry || job.profile.supports_cal_download ||
      job.profile.tx_id != 0x751 || job.profile.rx_id != 0x759 ||
      job.profile.ft_tx_id != 0x701 || job.profile.ft_rx_id != 0x761 ||
      job.profile.functional_id != 0x7DF || job.profile.padding != 0x55 ||
      job.profile.ft_padding != 0x55 ||
      job.profile.nominal_bitrate != 500000 ||
      job.profile.data_bitrate != 2000000 ||
      job.profile.security_level != 0x11 ||
      job.profile.app_start != kLpArfAppAddress ||
      job.profile.app_length != kLpArfAppLength ||
      job.profile.driver_length != 0) {
    throw std::runtime_error(contract.name +
                             " Profile conflicts with the frozen ARF2.31 contract");
  }
  if (!job.driver_file.empty()) {
    throw std::runtime_error(contract.name + " normal requirement disables Driver download");
  }
  const auto entry = resolve_lingpao_radar_entry_mode(job.entry_mode,
                                                       contract.name);
  const auto app_path = resolve(job, job.app_file);
  const auto certificate_path = resolve(job, job.app_verify_file);
  const auto dll = resolve(job, job.security_dll);
  const auto broker = job.executable_directory / L"keygen_broker.exe";
  if (!std::filesystem::is_regular_file(dll)) {
    throw std::runtime_error(contract.name + " SeedKey DLL is missing");
  }

  LingpaoRadarImages images;
  try {
    images.app = load_app(app_path);
    images.certificate = load_certificate(certificate_path);
  } catch (const std::exception& error) {
    throw std::runtime_error(contract.name +
                             " file preflight failed before CAN access: " +
                             error.what());
  }
  const auto app_hash = sha256(images.app.data);
  if (!same(app_hash, std::span(images.certificate).first(app_hash.size()))) {
    throw std::runtime_error(contract.name +
                             " TMP/ASC certificate is not bound to the selected APP");
  }

  const auto keygen = [broker, dll, variant = job.profile.security_variant](
                          std::span<const std::uint8_t> seed, unsigned level) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };
  constexpr std::array<std::uint8_t, 4> seed1{0xFF, 0xFD, 0x13, 0xDE};
  constexpr std::array<std::uint8_t, 4> key1{0xC0, 0x82, 0x85, 0x73};
  constexpr std::array<std::uint8_t, 4> seed2{0xFF, 0xFD, 0x03, 0xD0};
  constexpr std::array<std::uint8_t, 4> key2{0x14, 0x07, 0x37, 0x0F};
  try {
    if (!same(keygen(seed1, 0x11), key1) ||
        !same(keygen(seed2, 0x11), key2)) {
      throw std::runtime_error("captured SeedKey vectors do not match");
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(contract.name +
                             " SeedKey preflight failed before CAN access: " +
                             error.what());
  }
  report(callbacks, "Requirement contract", "PASS",
         contract.name +
             "; APP=751/759; PLS=701/761; APP window=000C0000/00180000; TMP certificate=1322 bytes and SHA-256 bound");
  report(callbacks, "Acceptance boundary", "WARN",
         "A12 and B11 share the parameterized ARF2.31 protocol engine, but retain independent Profiles/resources and require independent bench acceptance");

  if (!job.can_bus_provider) throw std::runtime_error("CAN bus provider is not configured");
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, job.profile.can_fd, L"UDSToolCpp"});
  IsoTpConfig app_config{job.profile.tx_id, job.profile.rx_id,
                         job.profile.padding, 0, job.profile.isotp_st_min,
                         1000ms, 1000ms, false, false, false, false};
  IsoTpSession physical_transport(*bus, app_config);
  auto app_func_config = app_config;
  app_func_config.tx_id = job.profile.functional_id;
  IsoTpSession app_functional_transport(*bus, app_func_config);
  auto pls_func_config = app_func_config;
  pls_func_config.rx_id = job.profile.ft_rx_id;
  IsoTpSession pls_functional_transport(*bus, pls_func_config);
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
  auto spec = lp_arf_radar_spec();
  spec.name = contract.name;
  LingpaoRadarFlow flow(
      physical, app_functional, pls_functional, physical_transport,
      pls_transport, app_functional_transport,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 TransferData")) {
          callbacks.progress(percent, line);
        }
      },
      keygen, std::move(spec));
  try {
    flow.run(images, entry, stop);
  } catch (...) {
    report(callbacks, "Failure state", "WARN",
           flow.core_programming_completed()
               ? "Programming/reset completed but cleanup failed; confirm ECU state before retry"
               : "Programming/verification/reset did not all complete; ECU state is unknown");
    throw;
  }
  report(callbacks, "Download", "PASS", contract.name + " normal flow completed");
}
} // namespace

std::wstring_view LpArf231A12Workflow::id() const noexcept { return kA12.id; }
std::string LpArf231A12Workflow::report_title(const FlashProfile&) const {
  return "零跑 A12 ARF2.31 刷写报告";
}
void LpArf231A12Workflow::run(const FlashJob& job,
                              const FlashWorkflowCallbacks& callbacks,
                              std::stop_token stop) {
  run_project(kA12, job, callbacks, stop);
}

std::wstring_view LpArf231B11Workflow::id() const noexcept { return kB11.id; }
std::string LpArf231B11Workflow::report_title(const FlashProfile&) const {
  return "零跑 B11 ARF2.31 刷写报告";
}
void LpArf231B11Workflow::run(const FlashJob& job,
                              const FlashWorkflowCallbacks& callbacks,
                              std::stop_token stop) {
  run_project(kB11, job, callbacks, stop);
}

} // namespace uds
