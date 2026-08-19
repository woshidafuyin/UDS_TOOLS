#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace uds {

enum class UdsNegativeResponseKind {
  pending,
  failure,
};

struct UdsNegativeResponse {
  std::uint8_t request_sid{};
  std::uint8_t nrc{};
  UdsNegativeResponseKind kind{UdsNegativeResponseKind::failure};
};

struct UdsRoutineResult {
  std::uint16_t routine_id{};
  std::uint8_t status{};
  bool failure{};
};

// Parses an already reassembled UDS payload such as 7F 31 31.
[[nodiscard]] std::optional<UdsNegativeResponse>
parse_uds_negative_response(std::span<const std::uint8_t> payload) noexcept;

// Parses a complete ISO-TP single frame.  Both classic-CAN SF encoding
// (03 7F 31 31) and CAN-FD escape-length SF encoding
// (00 03 7F 31 31) are accepted.
[[nodiscard]] std::optional<UdsNegativeResponse>
parse_isotp_single_frame_negative_response(
    std::span<const std::uint8_t> frame_data) noexcept;

// Parses the project routine result carried by a positive 0x71 response,
// e.g. 71 01 02 02 05.  Status 0x04 means pass and 0x05 means fail in the
// ChuNeng/LP flashing contract.
[[nodiscard]] std::optional<UdsRoutineResult>
parse_uds_routine_result(std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] std::optional<UdsRoutineResult>
parse_isotp_single_frame_routine_result(
    std::span<const std::uint8_t> frame_data) noexcept;

[[nodiscard]] std::string uds_nrc_name(std::uint8_t nrc);
[[nodiscard]] std::string uds_nrc_explanation_zh(std::uint8_t nrc);

// Example: NRC 0x31 RequestOutOfRange（请求超出范围：当前例程/参数不支持）
[[nodiscard]] std::string format_uds_nrc(std::uint8_t nrc);
[[nodiscard]] std::string uds_routine_name_zh(std::uint16_t routine_id);
[[nodiscard]] std::string format_uds_routine_result(
    const UdsRoutineResult& result);

} // namespace uds
