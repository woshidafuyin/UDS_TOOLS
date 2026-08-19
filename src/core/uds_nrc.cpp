#include "core/uds_nrc.hpp"

#include <iomanip>
#include <sstream>

namespace uds {
namespace {

std::optional<std::span<const std::uint8_t>>
isotp_single_frame_payload(std::span<const std::uint8_t> frame_data) noexcept {
  if (frame_data.empty() || (frame_data[0] & 0xF0U) != 0x00U) {
    return std::nullopt;
  }
  std::size_t payload_offset{1U};
  std::size_t payload_length{frame_data[0] & 0x0FU};
  if (payload_length == 0U) {
    if (frame_data.size() < 2U) return std::nullopt;
    payload_offset = 2U;
    payload_length = frame_data[1];
  }
  if (payload_offset + payload_length > frame_data.size()) return std::nullopt;
  return frame_data.subspan(payload_offset, payload_length);
}

} // namespace

std::optional<UdsNegativeResponse>
parse_uds_negative_response(std::span<const std::uint8_t> payload) noexcept {
  if (payload.size() < 3U || payload[0] != 0x7FU) return std::nullopt;
  return UdsNegativeResponse{
      payload[1], payload[2],
      payload[2] == 0x78U ? UdsNegativeResponseKind::pending
                          : UdsNegativeResponseKind::failure};
}

std::optional<UdsNegativeResponse>
parse_isotp_single_frame_negative_response(
    std::span<const std::uint8_t> frame_data) noexcept {
  const auto payload = isotp_single_frame_payload(frame_data);
  return payload ? parse_uds_negative_response(*payload) : std::nullopt;
}

std::optional<UdsRoutineResult>
parse_uds_routine_result(std::span<const std::uint8_t> payload) noexcept {
  if (payload.size() < 5U || payload[0] != 0x71U) return std::nullopt;
  const auto routine_id = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(payload[2]) << 8U) | payload[3]);
  const auto status = payload[4];
  if (status != 0x04U && status != 0x05U) return std::nullopt;
  // ARC331 bench/reference behavior explicitly treats 0203/05 as a warning:
  // vehicle preconditions are unavailable on the flash bench, but the
  // reference flow continues.  Verification routines such as 0202/05 remain
  // final failures.
  return UdsRoutineResult{routine_id, status,
                          status == 0x05U && routine_id != 0x0203U};
}

std::optional<UdsRoutineResult>
parse_isotp_single_frame_routine_result(
    std::span<const std::uint8_t> frame_data) noexcept {
  const auto payload = isotp_single_frame_payload(frame_data);
  return payload ? parse_uds_routine_result(*payload) : std::nullopt;
}

std::string uds_nrc_name(std::uint8_t nrc) {
  switch (nrc) {
  case 0x10: return "GeneralReject";
  case 0x11: return "ServiceNotSupported";
  case 0x12: return "SubFunctionNotSupported";
  case 0x13: return "IncorrectMessageLengthOrInvalidFormat";
  case 0x21: return "BusyRepeatRequest";
  case 0x22: return "ConditionsNotCorrect";
  case 0x24: return "RequestSequenceError";
  case 0x31: return "RequestOutOfRange";
  case 0x33: return "SecurityAccessDenied";
  case 0x35: return "InvalidKey";
  case 0x36: return "ExceedNumberOfAttempts";
  case 0x37: return "RequiredTimeDelayNotExpired";
  case 0x70: return "UploadDownloadNotAccepted";
  case 0x71: return "TransferDataSuspended";
  case 0x72: return "GeneralProgrammingFailure";
  case 0x73: return "WrongBlockSequenceCounter";
  case 0x78: return "RequestCorrectlyReceivedResponsePending";
  case 0x7E: return "SubFunctionNotSupportedInActiveSession";
  case 0x7F: return "ServiceNotSupportedInActiveSession";
  default: return "UnknownNRC";
  }
}

std::string uds_nrc_explanation_zh(std::uint8_t nrc) {
  switch (nrc) {
  case 0x10: return "一般拒绝：ECU无法完成请求";
  case 0x11: return "服务不支持";
  case 0x12: return "子功能不支持";
  case 0x13: return "消息长度或格式错误";
  case 0x21: return "ECU忙，请稍后重试";
  case 0x22: return "条件不满足：当前ECU状态不允许";
  case 0x24: return "请求顺序错误：前置步骤未完成";
  case 0x31: return "请求超出范围：当前例程/参数不支持";
  case 0x33: return "安全访问被拒绝：未解锁或权限不足";
  case 0x35: return "安全访问密钥错误";
  case 0x36: return "安全访问尝试次数超限";
  case 0x37: return "安全访问等待时间未到";
  case 0x70: return "上传或下载未被接受";
  case 0x71: return "数据传输已暂停";
  case 0x72: return "通用编程失败";
  case 0x73: return "传输块序号错误";
  case 0x78: return "请求已接收，ECU仍在处理；不是最终失败";
  case 0x7E: return "当前会话不支持该子功能";
  case 0x7F: return "当前会话不支持该服务";
  default: return "未收录的否定响应码，请结合项目诊断规范确认";
  }
}

std::string format_uds_nrc(std::uint8_t nrc) {
  std::ostringstream stream;
  stream << "NRC 0x" << std::uppercase << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned>(nrc) << ' '
         << uds_nrc_name(nrc) << "（" << uds_nrc_explanation_zh(nrc)
         << "）";
  return stream.str();
}

std::string uds_routine_name_zh(std::uint16_t routine_id) {
  switch (routine_id) {
  case 0x0202: return "数据/软件签名校验";
  case 0x0203: return "编程条件检查";
  case 0x0301: return "Flash Driver 激活";
  case 0xFF00: return "应用区擦除";
  case 0xFF01: return "依赖性/兼容性检查";
  default: return "例程执行";
  }
}

std::string format_uds_routine_result(const UdsRoutineResult& result) {
  std::ostringstream stream;
  stream << "RoutineControl 0x" << std::uppercase << std::hex << std::setw(4)
         << std::setfill('0') << result.routine_id << "（"
         << uds_routine_name_zh(result.routine_id) << "）状态 0x"
         << std::setw(2) << static_cast<unsigned>(result.status) << "：";
  if (result.routine_id == 0x0203U && result.status == 0x05U) {
    stream << "刷新条件未满足（WARN；当前项目按参考流程继续）";
  } else {
    stream << (result.failure ? "校验/执行失败" : "通过");
  }
  return stream.str();
}

} // namespace uds
