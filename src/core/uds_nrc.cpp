#include "core/uds_nrc.hpp"

#include <iomanip>
#include <sstream>

namespace uds {

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
  if (payload_length < 3U ||
      payload_offset + payload_length > frame_data.size()) {
    return std::nullopt;
  }
  return parse_uds_negative_response(
      frame_data.subspan(payload_offset, payload_length));
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

} // namespace uds
