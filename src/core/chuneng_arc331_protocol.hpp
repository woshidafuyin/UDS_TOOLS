#pragma once

#include <chrono>
#include <cstdint>

namespace uds {

// The user-provided CANoe ARC331 setup transmits the all-zero 0x520 wake-up
// frame every 10 ms.  Keep probe and flash on one project-level definition so
// a cold ECU is not tested with a different wake-up cadence than programming.
inline constexpr std::uint32_t kChunengArc331WakeupId = 0x520;
inline constexpr std::chrono::milliseconds kChunengArc331WakeupPeriod{10};

} // namespace uds
