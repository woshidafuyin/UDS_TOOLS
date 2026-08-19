#include "flash/lp_arc_flow.hpp"

#include "core/chuneng_arc331_protocol.hpp"

namespace uds {

LingpaoRadarSpec lp_arc_radar_spec(const FlashProfile& profile) {
  LingpaoRadarSpec spec{
      "LP-ARC", profile.tx_id, profile.rx_id, profile.ft_tx_id,
      profile.ft_rx_id, profile.functional_id,
      kLpArcAppAddress, kLpArcAppLength,
      kLpArcDriverAddress, kLpArcDriverLength,
      kLpArcBlockLength, kLpArcCertificateLength};
  // LP_ARC331.zip/ARC/CAPL/Flash20230727.can uses the fixed standard-CAN
  // transition frame 0x771: 03 FB A5 00 before SecurityAccess.  The APP
  // diagnostic endpoint may vary by radar target, but this transition ID does
  // not follow the selected physical request ID.
  spec.raw_boot_transition_tx_id = 0x771;
  spec.security.seed_subfunction =
      static_cast<std::uint8_t>(profile.security_level);
  spec.security.known_answers = {
      {{0xFF, 0xFD, 0x13, 0xDE}, {0xC0, 0x82, 0x85, 0x73}},
      {{0xFF, 0xFD, 0x03, 0xD0}, {0x14, 0x07, 0x37, 0x0F}},
  };
  spec.security.self_test_description =
      "FFFD13DE->C0828573 and FFFD03D0->1407370F";
  return spec;
}

LingpaoRadarSpec chuneng_arc331_radar_spec(const FlashProfile& profile) {
  LingpaoRadarSpec spec{
      "ChuNeng ARC331", profile.tx_id, profile.rx_id, profile.ft_tx_id,
      profile.ft_rx_id, profile.functional_id,
      kLpArcAppAddress, kLpArcAppLength,
      kLpArcDriverAddress, kLpArcDriverLength,
      kLpArcBlockLength, kLpArcCertificateLength};
  spec.send_raw_boot_transition = false;
  spec.raw_boot_transition_tx_id = 0;
  spec.periodic_wakeup_id = kChunengArc331WakeupId;
  spec.periodic_wakeup_period = kChunengArc331WakeupPeriod;
  spec.security.seed_subfunction =
      static_cast<std::uint8_t>(profile.security_level);
  spec.security.seed_length = 16;
  spec.security.key_length = 16;
  spec.security.known_answers = {
      {{0x21, 0x10, 0xF6, 0x0B, 0x7F, 0x45, 0x6E, 0x9A,
        0x67, 0x0F, 0xE4, 0x3D, 0x94, 0xA8, 0x6E, 0x0C},
       {0xFF, 0xD1, 0xFC, 0x2E, 0x7D, 0xC4, 0x4F, 0x24,
        0x27, 0x9B, 0x1A, 0x5E, 0xAA, 0xFF, 0x21, 0xD7}},
  };
  spec.security.self_test_description =
      "2110F60B7F456E9A670FE43D94A86E0C->"
      "FFD1FC2E7DC44F24279B1A5EAAFF21D7 (captured 67 12)";
  return spec;
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
    KeyGenerator key_generator, LingpaoRadarSpec spec, LpArcTiming timing)
    : LingpaoRadarFlow(physical, app_functional, pls_functional,
                       physical_transport, pls_transport,
                       functional_transport, std::move(log),
                       std::move(key_generator), std::move(spec), timing) {}

} // namespace uds
