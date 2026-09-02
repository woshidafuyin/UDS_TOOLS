#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace uds {

struct Chuneng331FtTransitionEndpoints {
  std::uint32_t request_id{};
  std::uint32_t pending_response_id{};
  std::uint32_t final_response_id{};
};

inline constexpr Chuneng331FtTransitionEndpoints
chuneng_331_ft_transition_endpoints(std::uint32_t ft_request_id,
                                    std::uint32_t ft_response_id,
                                    std::uint32_t app_response_id) noexcept {
  return {ft_request_id, ft_response_id, app_response_id};
}

inline constexpr std::chrono::milliseconds kChuneng331TesterPresentPeriod{2000};
inline constexpr std::chrono::milliseconds kChuneng331SessionControlDelay{50};
inline constexpr std::chrono::milliseconds kChuneng331FunctionalControlDelay{100};
inline constexpr std::chrono::milliseconds
    kChuneng331FtProgrammingTransitionDelay{2000};
inline constexpr std::uint8_t kChuneng331RoutineStatusPassed = 0x04;
inline constexpr std::uint8_t
    kChuneng331UnsupportedProgrammingPreconditionNrc = 0x31;

// The ARC331 reference flow tolerates RequestOutOfRange for RID 0x0203: some
// Boot/DCM variants do not register this optional precondition routine.  Keep
// this project rule here so the probe and the formal flash flow cannot drift.
inline constexpr bool chuneng_331_precondition_nrc_allows_continue(
    std::uint8_t nrc) noexcept {
  return nrc == kChuneng331UnsupportedProgrammingPreconditionNrc;
}

inline constexpr std::array<std::uint8_t, 2>
    kChuneng331FunctionalDefaultSessionRequest{0x10, 0x01};
inline constexpr std::array<std::uint8_t, 2>
    kChuneng331ExtendedSessionRequest{0x10, 0x03};
inline constexpr std::array<std::array<std::uint8_t, 2>, 2>
    kChuneng331FtFunctionalSessionPreamble{
        kChuneng331FunctionalDefaultSessionRequest,
        kChuneng331ExtendedSessionRequest};
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
