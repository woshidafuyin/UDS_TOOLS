#include "flash/lp_arf_flow.hpp"

#include "core/flash_data.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace uds {

namespace {

std::wstring lowercase_extension(const std::filesystem::path& path) {
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t value) { return std::towlower(value); });
  return extension;
}

SRecordSegment load_external_app(const std::filesystem::path& path) {
  if (path.empty()) {
    throw std::runtime_error("select an LP-ARF APP S19/SREC/BIN or TMP package");
  }
  if (lowercase_extension(path) == L".bin") {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open LP-ARF APP binary");
    std::vector<std::uint8_t> data{
        std::istreambuf_iterator<char>(input), {}};
    if (data.size() != kLpArfAppLength) {
      throw std::runtime_error("LP-ARF APP BIN must be exactly 0x180000 bytes");
    }
    return {kLpArfAppAddress, std::move(data)};
  }
  return {kLpArfAppAddress,
          load_srecord_window(path, kLpArfAppAddress, kLpArfAppLength)};
}

void require_valid_package(const LeapmotorTmpPackage& package) {
  if (package.app.address != kLpArfAppAddress ||
      package.app.data.size() != kLpArfAppLength) {
    throw std::runtime_error(
        "LP-ARF TMP APP must resolve to 000C0000/180000");
  }
  if (package.certificate.size() != kLpArfCertificateLength) {
    throw std::runtime_error(
        "LP-ARF TMP certificate must be exactly 1322 bytes");
  }
}

} // namespace

LpArfArtifacts load_lp_arf_artifacts(
    const std::filesystem::path& app_path,
    const std::filesystem::path& certificate_path) {
  if (lowercase_extension(app_path) == L".tmp") {
    if (!certificate_path.empty()) {
      throw std::runtime_error(
          "LP-ARF TMP already embeds its certificate; clear the external APP verification file");
    }
    auto package = load_leapmotor_tmp(app_path);
    require_valid_package(package);
    return {{{}, {package.app.address, std::move(package.app.data)},
             std::move(package.certificate)},
            true};
  }

  auto app = load_external_app(app_path);
  if (certificate_path.empty()) {
    return {{{}, {app.address, std::move(app.data)}, {}}, false};
  }
  if (lowercase_extension(certificate_path) == L".tmp") {
    auto package = load_leapmotor_tmp(certificate_path);
    require_valid_package(package);
    return {{{}, {app.address, std::move(app.data)},
             std::move(package.certificate)},
            false};
  }
  return {{{}, {app.address, std::move(app.data)},
           load_asc_hex(certificate_path, kLpArfCertificateLength,
                        kLpArfCertificateLength)},
          false};
}

LingpaoRadarSpec lp_arf_radar_spec() {
  // The ARF6.31 CANoe Download() intentionally leaves the Driver 34/36/37
  // and 0202 verification block commented.  Keep the driver fields empty so
  // the shared core cannot accidentally import LP-ARC's Driver phase.
  LingpaoRadarSpec spec{
      "LP-ARF", 0x751, 0x759, 0x701, 0x761, 0x7DF,
      kLpArfAppAddress, kLpArfAppLength, std::nullopt, std::nullopt,
      kLpArfBlockLength, kLpArfCertificateLength};
  // CANoe switches gResId to the APP response ID after transmitting 10 02.
  // When 0x761 first returns 7F 10 78, the final 50 02 therefore arrives on
  // 0x759 and must be received through the APP transport.
  spec.pls_programming_final_on_app = true;
  spec.send_raw_boot_transition = false;
  spec.allow_empty_certificate = true;
  spec.require_certificate_result_code = false;
  return spec;
}

LpArfEntryMode resolve_lp_arf_entry_mode(std::wstring_view entry_mode) {
  return resolve_lingpao_radar_entry_mode(entry_mode, "LP-ARF");
}

LpArfFlow::LpArfFlow(
    UdsClient& physical, UdsClient& app_functional,
    UdsClient& pls_functional, IsoTpSession& physical_transport,
    IsoTpSession& pls_transport, IsoTpSession& functional_transport, Log log,
    KeyGenerator key_generator, LpArfTiming timing)
    : LingpaoRadarFlow(physical, app_functional, pls_functional,
                       physical_transport, pls_transport,
                       functional_transport, std::move(log),
                       std::move(key_generator), lp_arf_radar_spec(), timing) {}

} // namespace uds
