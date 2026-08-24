#include "flash/lp_arf_flow.hpp"

#include <utility>

namespace uds {

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
