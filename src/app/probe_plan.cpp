#include "app/probe_plan.hpp"

#include "app/probe_support.hpp"
#include "flash/longma_ars1_31_flow.hpp"
#include "flash/xizhong_rsmr_flow.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace uds::app::probe_detail {
namespace {

std::pair<std::uint32_t, std::uint32_t> resolve_ft_endpoint(
    const ProbeRequest& request) {
  if (!request.profile.supports_ft_entry) {
    throw std::runtime_error("selected profile does not support FT probing");
  }
  const auto target = std::find_if(
      request.profile.targets.cbegin(), request.profile.targets.cend(),
      [&request](const FlashTargetProfile& candidate) {
        return candidate.tx_id == request.tx_id &&
               candidate.rx_id == request.rx_id;
      });
  const auto tx_id =
      target != request.profile.targets.cend() && target->ft_tx_id != 0
          ? target->ft_tx_id
          : request.profile.ft_tx_id;
  const auto rx_id =
      target != request.profile.targets.cend() && target->ft_rx_id != 0
          ? target->ft_rx_id
          : request.profile.ft_rx_id;
  if (tx_id == 0 || rx_id == 0) {
    throw std::runtime_error("selected target FT endpoint is not configured");
  }
  return {tx_id, rx_id};
}

} // namespace

ProbePlanResolution resolve_probe_plan(const ProbeRequest& requested) {
  ProbePlan plan;
  plan.request = requested;
  plan.ft_probe = plan.request.entry_mode == L"ft";
  plan.boot_probe = plan.request.entry_mode == L"boot";
  if (!plan.request.entry_mode.empty() && plan.request.entry_mode != L"app" &&
      plan.request.entry_mode != L"auto" && !plan.boot_probe &&
      !plan.ft_probe) {
    return {false, {}, "在线探测配置无效"};
  }

  plan.app_tx_id = plan.request.tx_id;
  plan.app_rx_id = plan.request.rx_id;
  if (plan.ft_probe) {
    try {
      std::tie(plan.request.tx_id, plan.request.rx_id) =
          resolve_ft_endpoint(plan.request);
    } catch (const std::exception&) {
      return {false, {}, "FT探测端点未配置"};
    }
  }

  plan.power_managed = plan.request.profile.power_control;
  plan.chuneng_arc331 = plan.request.profile.flow == L"chuneng_arc331";
  plan.shidaixinan_hjzj =
      plan.request.profile.flow == L"shidaixinan_hjzj_fmr";
  const bool lp_arc = plan.request.profile.flow == L"lp_arc";
  const bool lp_arf = plan.request.profile.flow == L"lp_arf";
  plan.lingpao_radar = lp_arc || lp_arf;
  plan.geely_p416 = plan.request.profile.flow == L"geely_p416";
  plan.xizhong = xizhong_supported_flow(plan.request.profile.flow);
  plan.ars131_app =
      plan.request.profile.flow == L"longma_ars1_31" ||
      plan.request.profile.flow == L"changan_c857" ||
      plan.request.profile.flow == L"lingyao_b216";
  plan.secondary_target =
      plan.ars131_app &&
      longma_ars131_secondary_endpoint(plan.app_tx_id, plan.app_rx_id);
  plan.probe_tx_id =
      plan.lingpao_radar
          ? plan.request.profile.functional_id
          : plan.shidaixinan_hjzj && !plan.ft_probe
                ? plan.request.profile.functional_id
                : plan.request.tx_id;
  plan.expected_profile_ids =
      plan.ft_probe ||
      (plan.request.tx_id == plan.request.profile.tx_id &&
       plan.request.rx_id == plan.request.profile.rx_id) ||
      std::any_of(
          plan.request.profile.targets.cbegin(),
          plan.request.profile.targets.cend(),
          [&plan](const FlashTargetProfile& target) {
            return target.tx_id == plan.request.tx_id &&
                   target.rx_id == plan.request.rx_id;
          });
  plan.session = static_cast<std::uint8_t>(
      (plan.lingpao_radar || plan.geely_p416)
          ? 0x01
          : (plan.ft_probe || plan.shidaixinan_hjzj || plan.chuneng_arc331
                 ? 0x03
                 : 0x01));
  plan.attempt_count =
      !plan.ft_probe && (plan.xizhong || plan.shidaixinan_hjzj) ? 3 : 1;
  return {true, std::move(plan), {}};
}

std::string probe_start_description(const ProbePlan& plan) {
  std::ostringstream start;
  start << "探测项目“" << utf8(plan.request.profile.name) << "”：物理CH"
        << plan.request.channel << "，"
        << ((plan.shidaixinan_hjzj || plan.lingpao_radar)
                ? "功能寻址 0x"
                : "物理寻址 0x")
        << std::hex << plan.probe_tx_id << " -> 0x" << plan.request.rx_id;
  if (plan.ars131_app) {
    start << (plan.secondary_target ? "（从雷达）" : "（主雷达）");
  }
  start << (plan.ft_probe ? "，FT入口" : "，APP入口");
  return start.str();
}

std::string probe_session_description(const ProbePlan& plan) {
  if (plan.geely_p416) {
    const std::string project = plan.request.profile.id == L"geely_p611"
                                    ? "吉利P611"
                                    : "吉利P416";
    return plan.ft_probe
               ? "在线探测：" + project +
                     " PLS入口向0x701/0x761发送10 01；收到50 01即判定在线，不执行10 02或刷写。"
               : "在线探测：" + project +
                     " APP入口向0x716/0x616发送10 01；收到50 01即判定在线，不执行10 02或刷写。";
  }
  if (plan.lingpao_radar) {
    return plan.ft_probe
               ? "在线探测：" + utf8(plan.request.profile.name) +
                     " PLS→APP 先向0x7DF/0x761发送10 01；收到50 01后再发送10 03，不执行10 02或刷写。"
               : "在线探测：" + utf8(plan.request.profile.name) +
                     " APP→APP 先向功能ID/APP响应ID发送10 01；收到50 01后再发送10 03，不执行10 02或刷写。";
  }
  if (plan.chuneng_arc331 && !plan.ft_probe) {
    return plan.boot_probe
               ? "在线探测：楚能ARC331 BOOT→APP入口持续发送0x520唤醒，向所选雷达物理端点发送10 03；收到50 03即确认诊断在线，不发送仅APP入口适用的31 01 02 03，也不进入编程会话或刷写。"
               : "在线探测：楚能ARC331 APP入口持续发送0x520唤醒，向所选雷达物理端点发送10 03；收到50 03后检查31 01 02 03。该检查不发送10 02或刷写数据。";
  }
  if (plan.ft_probe) {
    return "在线探测：向FT端点发送扩展会话请求10 03；收到并核验50 03后判定在线，不执行10 02或刷写。";
  }
  if (plan.shidaixinan_hjzj) {
    return "在线探测：向0x7DF发送功能寻址扩展会话10 03；收到0x7AC的50 03后判定时代新安FMR在线。";
  }
  return "在线探测：发送物理默认会话请求10 01；进度保持0%，收到并核验50 01后置100%。";
}

} // namespace uds::app::probe_detail
