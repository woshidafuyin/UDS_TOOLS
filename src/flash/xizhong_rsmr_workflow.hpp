#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

bool xizhong_rsmr_report_line(std::string_view line) noexcept;

class XizhongRsmrWorkflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&, std::stop_token) override;
};

} // namespace uds
