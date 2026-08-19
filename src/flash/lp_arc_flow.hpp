#pragma once

#include "core/profile.hpp"
#include "flash/lingpao_radar_flow.hpp"

namespace uds {

inline constexpr std::uint32_t kLpArcDriverAddress{0x00000000};
inline constexpr std::uint32_t kLpArcDriverLength{0x00004000};
inline constexpr std::uint32_t kLpArcAppAddress{0x000C0000};
inline constexpr std::uint32_t kLpArcAppLength{0x00180000};
inline constexpr std::size_t kLpArcBlockLength{0x802};
inline constexpr std::size_t kLpArcCertificateLength{1322};
inline constexpr std::uint32_t kLpArcReferenceDriverCrc32{0x8509E388};
inline constexpr std::uint32_t kLpArcReferenceAppCrc32{0xB3879B50};

using LpArcEntryMode = LingpaoRadarEntryMode;
using LpArcImages = LingpaoRadarImages;
using LpArcTiming = LingpaoRadarTiming;

LingpaoRadarSpec lp_arc_radar_spec(const FlashProfile& profile);
LingpaoRadarSpec chuneng_arc331_radar_spec(const FlashProfile& profile);
LpArcEntryMode resolve_lp_arc_entry_mode(std::wstring_view entry_mode);
std::uint32_t lp_arc_crc32(std::span<const std::uint8_t> data) noexcept;
std::vector<std::uint8_t> lp_arc_request_download(std::uint32_t address,
                                                  std::uint32_t length);
std::vector<std::uint8_t> lp_arc_erase_memory(std::uint32_t address,
                                              std::uint32_t length);
std::vector<std::uint8_t> lp_arc_driver_crc_request(std::uint32_t crc);
std::vector<std::uint8_t> lp_arc_programming_date(const std::tm& local_time);
std::size_t lp_arc_max_block_length(
    std::span<const std::uint8_t> request_download_response);

class LpArcFlow final : public LingpaoRadarFlow {
public:
  LpArcFlow(UdsClient& physical, UdsClient& app_functional,
            UdsClient& pls_functional, IsoTpSession& physical_transport,
            IsoTpSession& pls_transport, IsoTpSession& functional_transport,
            Log log, KeyGenerator key_generator, LingpaoRadarSpec spec,
            LpArcTiming timing = {});
};

} // namespace uds
