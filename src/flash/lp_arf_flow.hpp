#pragma once

#include "flash/lingpao_radar_flow.hpp"

namespace uds {

inline constexpr std::uint32_t kLpArfAppAddress{0x000C0000};
inline constexpr std::uint32_t kLpArfAppLength{0x00180000};
inline constexpr std::size_t kLpArfBlockLength{0x802};
inline constexpr std::size_t kLpArfCertificateLength{1322};

using LpArfEntryMode = LingpaoRadarEntryMode;
using LpArfImages = LingpaoRadarImages;
using LpArfTiming = LingpaoRadarTiming;

LingpaoRadarSpec lp_arf_radar_spec();
LpArfEntryMode resolve_lp_arf_entry_mode(std::wstring_view entry_mode);

class LpArfFlow final : public LingpaoRadarFlow {
public:
  LpArfFlow(UdsClient& physical, UdsClient& app_functional,
            UdsClient& pls_functional, IsoTpSession& physical_transport,
            IsoTpSession& pls_transport, IsoTpSession& functional_transport,
            Log log, KeyGenerator key_generator, LpArfTiming timing = {});
};

} // namespace uds
