#include "app/probe_session.hpp"

#include "app/probe_preconditions.hpp"
#include "app/probe_support.hpp"
#include "core/chuneng_arc331_protocol.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "flash/chuneng_331_protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <sstream>
#include <thread>

namespace uds::app::probe_detail {
namespace {
using namespace std::chrono_literals;
}

std::string execute_probe_session(
    const ProbePlan& plan, ICanBus& bus, ProbePreconditions& preconditions,
    const ProbeServiceCallbacks& callbacks, std::stop_token stop) {
  IsoTpConfig probe_config{
      plan.probe_tx_id, plan.request.rx_id,
      plan.ft_probe ? plan.request.profile.ft_padding : plan.request.padding, 0,
      plan.request.profile.isotp_st_min, 1000ms, 1000ms,
      plan.ft_probe ? plan.request.profile.ft_extended_id
                    : plan.request.profile.extended_id,
      plan.ft_probe ? plan.request.profile.ft_extended_id
                    : plan.request.profile.extended_id,
      plan.ft_probe ? plan.request.profile.ft_uds_fd
                    : plan.request.profile.uds_fd,
      plan.ft_probe ? plan.request.profile.ft_uds_brs
                    : plan.request.profile.uds_brs};
  probe_config.drain_receive_before_send = !plan.ft_probe && plan.xizhong;
  IsoTpSession transport(bus, probe_config);
  UdsClient client(transport,
                   [&](const std::string& line) { log(callbacks, line); }, stop);
  check_stop(stop);
  log(callbacks, probe_session_description(plan));

  std::optional<UdsResponse> response;
  std::string last_request_error;
  const auto request_session = [&](std::chrono::milliseconds timeout) {
    try {
      auto candidate = client.request(
          std::array<std::uint8_t, 2>{0x10, plan.session}, timeout);
      if (candidate.success && candidate.response.size() >= 2 &&
          candidate.response[0] == 0x50 &&
          candidate.response[1] == plan.session) {
        response = std::move(candidate);
        return true;
      }
      std::ostringstream detail;
      if (!candidate.success) {
        detail << "UDS request failed";
        if (candidate.nrc != 0) {
          detail << ", NRC=0x" << std::hex << std::uppercase
                 << static_cast<int>(candidate.nrc);
        }
        if (!candidate.detail.empty()) detail << ", " << candidate.detail;
      } else {
        detail << "unexpected "
               << (plan.ft_probe ? "FT ExtendedSession" : "DefaultSession")
               << " response: " << to_hex(candidate.response);
      }
      last_request_error = detail.str();
    } catch (const std::exception& error) {
      last_request_error = error.what();
    }
    return false;
  };

  if (plan.chery_e0y) {
    log(callbacks,
        "在线探测：0x600唤醒1秒后开始10 01探测；收到50 01立即返回，"
        "15秒仅作为最大等待上限。");
    preconditions.wait_for_e0y_ready(request_session);
  } else {
    for (int attempt = 1; attempt <= plan.attempt_count; ++attempt) {
      check_stop(stop);
      if (plan.attempt_count > 1) {
        log(callbacks,
            (plan.shidaixinan_hjzj ? "时代新安功能10 03探测：第"
                                   : "犀重物理10 01探测：第") +
                std::to_string(attempt) + "/" +
                std::to_string(plan.attempt_count) + "次");
      }
      if (request_session(1000ms)) break;
      if (attempt < plan.attempt_count) {
        for (int elapsed = 0; elapsed < 10; ++elapsed) {
          check_stop(stop);
          preconditions.check();
          std::this_thread::sleep_for(20ms);
        }
      }
    }
  }

  preconditions.stop_and_check();
  check_stop(stop);
  if (!response) {
    throw std::runtime_error(last_request_error.empty()
                                 ? "UDS request failed"
                                 : last_request_error);
  }

  std::string response_summary = to_hex(response->response);
  if (plan.chuneng_arc331 && !plan.ft_probe && plan.boot_probe) {
    log(callbacks,
        "PASS：楚能ARC331 BOOT→APP入口已收到所选物理响应ID的50 03；"
        "本按钮不发送31 01 02 03、10 02或任何刷写数据。");
  } else if (plan.chuneng_arc331 && !plan.ft_probe) {
    log(callbacks,
        "楚能ARC331 APP入口：物理10 03已响应；发送31 01 02 03检查"
        "APP刷新入口，不发送10 02或任何刷写数据。");
    const auto precondition =
        client.request(kChuneng331ProgrammingPrecondition, 1000ms);
    check_stop(stop);
    if (!precondition.success) {
      if (chuneng_331_precondition_nrc_allows_continue(precondition.nrc)) {
        log(callbacks,
            "WARN：楚能ARC331返回7F 31 31；当前Boot/DCM未注册RID 0203，"
            "按项目CANoe参考流程容错继续。正式刷写仍由10 02及后续步骤"
            "确认实际可刷性。");
        response_summary +=
            "；ProgrammingPrecondition=" + to_hex(precondition.response) +
            "（NRC 0x31，容错继续）";
      } else {
        throw std::runtime_error(
            "ARC331 ProgrammingPrecondition NRC/timeout: " +
            precondition.detail);
      }
    } else {
      const std::array<std::uint8_t, 5> passed{0x71, 0x01, 0x02, 0x03,
                                               0x04};
      const std::array<std::uint8_t, 5> not_met{0x71, 0x01, 0x02, 0x03,
                                                0x05};
      const auto has_prefix = [&](const auto& expected) {
        return precondition.response.size() >= expected.size() &&
               std::equal(expected.begin(), expected.end(),
                          precondition.response.begin());
      };
      if (!has_prefix(passed) && !has_prefix(not_met)) {
        throw std::runtime_error(
            "ARC331 ProgrammingPrecondition response mismatch: " +
            to_hex(precondition.response));
      }
      log(callbacks,
          has_prefix(passed)
              ? "PASS：楚能ARC331 APP刷新入口可用（31 01 02 03状态0x04）。"
              : "WARN：楚能ARC331 APP刷新入口可用，但刷新条件状态为0x05；"
                "正式流程将按项目参考策略继续并保留原始响应。");
      response_summary +=
          "；ProgrammingPrecondition=" + to_hex(precondition.response);
    }
  }

  preconditions.stop_and_check();
  if (plan.lingpao_radar) {
    log(callbacks,
        utf8(plan.request.profile.name) +
            "：默认会话已响应；按 CANoe Download() 等待2秒后发送功能寻址扩展会话。");
    for (int elapsed = 0; elapsed < 40; ++elapsed) {
      check_stop(stop);
      std::this_thread::sleep_for(50ms);
    }
    log(callbacks, "在线探测：发送CANoe功能寻址扩展会话请求10 03。");
    const auto extended =
        client.request(std::array<std::uint8_t, 2>{0x10, 0x03}, 800ms);
    check_stop(stop);
    if (!extended.success) {
      throw std::runtime_error("UDS ExtendedSession request failed");
    }
    if (extended.response.size() < 2 || extended.response[0] != 0x50 ||
        extended.response[1] != 0x03) {
      throw std::runtime_error("unexpected ExtendedSession response: " +
                               to_hex(extended.response));
    }
    response_summary += "；ExtendedSession=" + to_hex(extended.response);
  }
  return response_summary;
}

} // namespace uds::app::probe_detail
