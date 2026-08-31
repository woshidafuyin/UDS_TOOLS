#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

bool xizhong_rsmr_report_line(std::string_view line) noexcept;

enum class XizhongRadarTarget { rsmr, lsmr };

void validate_xizhong_configurable_endpoint(
    const FlashProfile& profile, XizhongRadarTarget target);

class XizhongRadarWorkflow final : public FlashWorkflow {
public:
  explicit XizhongRadarWorkflow(XizhongRadarTarget target) noexcept;

  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&, std::stop_token) override;

private:
  XizhongRadarTarget target_;
};

} // namespace uds
