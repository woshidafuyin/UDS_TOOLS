#include "flash/lp_arc_flow.hpp"

namespace uds {

LingpaoRadarSpec lp_arc_radar_spec() {
  return {"LP-ARC", 0x772, 0x77A, 0x701, 0x761, 0x7DF,
          kLpArcAppAddress, kLpArcAppLength,
          kLpArcDriverAddress, kLpArcDriverLength,
          kLpArcBlockLength, kLpArcCertificateLength};
}

LpArcEntryMode resolve_lp_arc_entry_mode(std::wstring_view entry_mode) {
  return resolve_lingpao_radar_entry_mode(entry_mode, "LP-ARC");
}

std::uint32_t lp_arc_crc32(std::span<const std::uint8_t> data) noexcept {
  return lingpao_radar_crc32(data);
}

std::vector<std::uint8_t> lp_arc_request_download(std::uint32_t address,
                                                  std::uint32_t length) {
  return lingpao_radar_request_download(address, length);
}

std::vector<std::uint8_t> lp_arc_erase_memory(std::uint32_t address,
                                              std::uint32_t length) {
  return lingpao_radar_erase_memory(address, length);
}

std::vector<std::uint8_t> lp_arc_driver_crc_request(std::uint32_t crc) {
  return lingpao_radar_driver_crc_request(crc);
}

std::vector<std::uint8_t> lp_arc_programming_date(const std::tm& local_time) {
  return lingpao_radar_programming_date(local_time);
}

std::size_t lp_arc_max_block_length(
    std::span<const std::uint8_t> response) {
  return lingpao_radar_max_block_length(response, kLpArcBlockLength,
                                        "LP-ARC");
}

LpArcFlow::LpArcFlow(
    UdsClient& physical, UdsClient& app_functional,
    UdsClient& pls_functional, IsoTpSession& physical_transport,
    IsoTpSession& pls_transport, IsoTpSession& functional_transport, Log log,
    KeyGenerator key_generator, LpArcTiming timing)
    : LingpaoRadarFlow(physical, app_functional, pls_functional,
                       physical_transport, pls_transport,
                       functional_transport, std::move(log),
                       std::move(key_generator), lp_arc_radar_spec(), timing) {}

} // namespace uds
