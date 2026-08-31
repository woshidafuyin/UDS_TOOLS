#include "app/probe_support.hpp"

namespace uds::app::probe_detail {

std::string concise_probe_failure(std::string_view detail) {
  if (detail.find("timeout") != std::string_view::npos ||
      detail.find("response wait failed") != std::string_view::npos ||
      detail.find("no valid UDS response") != std::string_view::npos) {
    return "未收到设备响应";
  }
  if (detail.find("unexpected") != std::string_view::npos ||
      detail.find("UDS request failed") != std::string_view::npos ||
      detail.find("NRC") != std::string_view::npos) {
    return "设备响应无效";
  }
  return "在线探测失败";
}

} // namespace uds::app::probe_detail
