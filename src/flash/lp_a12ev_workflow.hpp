#pragma once

#include "flash/flash_workflow.hpp"
#include "flash/lingpao_radar_flow.hpp"

namespace uds {

LingpaoRadarSpec lp_a12ev_radar_spec(const FlashProfile& profile);

class LpA12evWorkflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;
};

} // namespace uds
