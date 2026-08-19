#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

class CheryKp31Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;
};

} // namespace uds
