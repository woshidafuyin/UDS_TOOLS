#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace uds {

inline constexpr std::chrono::milliseconds kChuneng331TesterPresentPeriod{2000};
inline constexpr std::chrono::milliseconds kChuneng331SessionControlDelay{50};
inline constexpr std::chrono::milliseconds kChuneng331FunctionalControlDelay{100};
inline constexpr std::uint8_t kChuneng331RoutineStatusPassed = 0x04;

inline constexpr std::array<std::uint8_t, 2>
    kChuneng331FunctionalDefaultSessionRequest{0x10, 0x01};
inline constexpr std::array<std::uint8_t, 4>
    kChuneng331ProgrammingPrecondition{0x31, 0x01, 0x02, 0x03};
inline constexpr std::array<std::uint8_t, 2>
    kChuneng331FunctionalExtendedSession{0x10, 0x83};
inline constexpr std::array<std::uint8_t, 2>
    kChuneng331DisableDtc{0x85, 0x82};
inline constexpr std::array<std::uint8_t, 3>
    kChuneng331DisableCommunication{0x28, 0x83, 0x03};
inline constexpr std::array<std::uint8_t, 2>
    kChuneng331ProgrammingSession{0x10, 0x02};
inline constexpr std::array<std::uint8_t, 2>
    kChuneng331EnableDtc{0x85, 0x81};
inline constexpr std::array<std::uint8_t, 3>
    kChuneng331EnableCommunication{0x28, 0x80, 0x03};
inline constexpr std::array<std::uint8_t, 2>
    kChuneng331FunctionalDefaultSession{0x10, 0x81};
inline constexpr std::array<std::uint8_t, 4>
    kChuneng331ClearDtc{0x14, 0xFF, 0xFF, 0xFF};

inline constexpr std::array<std::uint8_t, 5>
chuneng_331_routine_success_prefix(std::uint16_t routine_id) {
  return {0x71, 0x01, static_cast<std::uint8_t>(routine_id >> 8U),
          static_cast<std::uint8_t>(routine_id),
          kChuneng331RoutineStatusPassed};
}

} // namespace uds
