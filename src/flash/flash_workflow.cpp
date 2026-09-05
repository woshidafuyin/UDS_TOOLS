#include "flash/flash_workflow.hpp"
#include "flash/perodua_p02c_workflow.hpp"

#include "flash/baic_radar_workflows.hpp"
#include "flash/chery_ars1_33_workflow.hpp"
#include "flash/chery_ars1_31_project_workflows.hpp"
#include "flash/chery_kp31_workflow.hpp"
#include "flash/chuneng_331_workflow.hpp"
#include "flash/geely_p416_workflow.hpp"
#include "flash/c857_project_workflows.hpp"
#include "flash/longma_ars1_31_workflow.hpp"
#include "flash/lp_arc_workflow.hpp"
#include "flash/lp_arf_workflow.hpp"
#include "flash/shidaixinan_hjzj_fmr_workflow.hpp"
#include "flash/xizhong_rsmr_workflow.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace uds {
namespace {

using WorkflowFactory = std::unique_ptr<FlashWorkflow> (*)();

struct WorkflowRegistration {
  std::wstring_view id;
  WorkflowFactory create;
};

std::unique_ptr<FlashWorkflow> create_chery_ars1_33() {
  return std::make_unique<CheryArs133Workflow>();
}

std::unique_ptr<FlashWorkflow> create_chery_kp31() {
  return std::make_unique<CheryKp31Workflow>();
}

std::unique_ptr<FlashWorkflow> create_chery_t1ej() {
  return std::make_unique<CheryT1ejWorkflow>();
}

std::unique_ptr<FlashWorkflow> create_chery_t22() {
  return std::make_unique<CheryT22Workflow>();
}

std::unique_ptr<FlashWorkflow> create_chery_e0y() {
  return std::make_unique<CheryE0yWorkflow>();
}

std::unique_ptr<FlashWorkflow> create_longma_ars1_31() {
  return std::make_unique<LongmaArs131Workflow>();
}

std::unique_ptr<FlashWorkflow> create_changan_c857() {
  return std::make_unique<ChanganC857Workflow>();
}

std::unique_ptr<FlashWorkflow> create_lingyao_b216() {
  return std::make_unique<LingyaoB216Workflow>();
}

std::unique_ptr<FlashWorkflow> create_xizhong_rsmr() {
  return std::make_unique<XizhongRadarWorkflow>(XizhongRadarTarget::rsmr);
}

std::unique_ptr<FlashWorkflow> create_xizhong_lsmr() {
  return std::make_unique<XizhongRadarWorkflow>(XizhongRadarTarget::lsmr);
}

std::unique_ptr<FlashWorkflow> create_shidaixinan_hjzj_fmr() {
  return std::make_unique<ShidaixinanHjzjFmrWorkflow>();
}

std::unique_ptr<FlashWorkflow> create_lp_arc() {
  return std::make_unique<LpArcWorkflow>();
}

std::unique_ptr<FlashWorkflow> create_chuneng_arc331() {
  return std::make_unique<ChunengArc331Workflow>();
}

std::unique_ptr<FlashWorkflow> create_lp_arf() {
  return std::make_unique<LpArfWorkflow>();
}

std::unique_ptr<FlashWorkflow> create_geely_p416() {
  return std::make_unique<GeelyP416Workflow>();
}

std::unique_ptr<FlashWorkflow> create_baic_n61ab() {
  return std::make_unique<BaicRadarWorkflow>(BaicRadarProject::n61ab);
}

std::unique_ptr<FlashWorkflow> create_baic_bqb41() {
  return std::make_unique<BaicRadarWorkflow>(BaicRadarProject::bqb41);
}

// 新增项目专用刷写流程时，只需在这里增加一条注册记录。
constexpr std::array kWorkflowRegistrations{
  WorkflowRegistration{L"perodua_p02c", []() -> std::unique_ptr<FlashWorkflow> {
    return std::make_unique<PeroduaP02cWorkflow>();
  }},
  WorkflowRegistration{L"chuneng_arc331", &create_chuneng_arc331},
  WorkflowRegistration{L"chery_ars1_33", &create_chery_ars1_33},
  WorkflowRegistration{L"chery_kp31", &create_chery_kp31},
  WorkflowRegistration{L"chery_e0y", &create_chery_e0y},
  WorkflowRegistration{L"chery_t22", &create_chery_t22},
  WorkflowRegistration{L"chery_t1ej", &create_chery_t1ej},
  WorkflowRegistration{L"changan_c857", &create_changan_c857},
  WorkflowRegistration{L"longma_ars1_31", &create_longma_ars1_31},
  WorkflowRegistration{L"lingyao_b216", &create_lingyao_b216},
  WorkflowRegistration{L"xizhong_rsmr", &create_xizhong_rsmr},
  WorkflowRegistration{L"xizhong_lsmr", &create_xizhong_lsmr},
  WorkflowRegistration{L"shidaixinan_hjzj_fmr",
                       &create_shidaixinan_hjzj_fmr},
  WorkflowRegistration{L"lp_arc", &create_lp_arc},
  WorkflowRegistration{L"lp_arf", &create_lp_arf},
  WorkflowRegistration{L"geely_p416", &create_geely_p416},
  WorkflowRegistration{L"baic_n61ab", &create_baic_n61ab},
  WorkflowRegistration{L"baic_bqb41", &create_baic_bqb41},
};

const WorkflowRegistration* find_registration(std::wstring_view flow_id) noexcept {
  const auto item = std::find_if(kWorkflowRegistrations.begin(), kWorkflowRegistrations.end(),
                                 [flow_id](const auto& entry) { return entry.id == flow_id; });
  return item == kWorkflowRegistrations.end() ? nullptr : &*item;
}

} // namespace

std::unique_ptr<FlashWorkflow> create_flash_workflow(std::wstring_view flow_id) {
  if (const auto* registration = find_registration(flow_id)) return registration->create();
  throw std::runtime_error("unsupported flash workflow");
}

bool is_flash_workflow_registered(std::wstring_view flow_id) noexcept {
  return find_registration(flow_id) != nullptr;
}

std::vector<std::wstring> registered_flash_workflows() {
  std::vector<std::wstring> result;
  result.reserve(kWorkflowRegistrations.size());
  for (const auto& registration : kWorkflowRegistrations) result.emplace_back(registration.id);
  return result;
}

} // namespace uds
