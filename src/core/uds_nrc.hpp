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

// Parses an already reassembled UDS payload such as 7F 31 31.
[[nodiscard]] std::optional<UdsNegativeResponse>
parse_uds_negative_response(std::span<const std::uint8_t> payload) noexcept;

// Parses a complete ISO-TP single frame.  Both classic-CAN SF encoding
// (03 7F 31 31) and CAN-FD escape-length SF encoding
// (00 03 7F 31 31) are accepted.
[[nodiscard]] std::optional<UdsNegativeResponse>
parse_isotp_single_frame_negative_response(
    std::span<const std::uint8_t> frame_data) noexcept;

[[nodiscard]] std::string uds_nrc_name(std::uint8_t nrc);
[[nodiscard]] std::string uds_nrc_explanation_zh(std::uint8_t nrc);

// Example: NRC 0x31 RequestOutOfRange（请求超出范围：当前例程/参数不支持）
[[nodiscard]] std::string format_uds_nrc(std::uint8_t nrc);

} // namespace uds
