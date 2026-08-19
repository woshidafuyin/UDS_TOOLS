#pragma once

#include "flash/flash_workflow.hpp"

#include <string>

namespace uds {

// Shared runner for projects whose passing reference follows the same
// ARS1.31 APP Download state machine. Project identity, target endpoints and
// resource/CRC contracts remain in each project's profile.
class Ars131AppWorkflow : public FlashWorkflow {
public:
  Ars131AppWorkflow(std::wstring workflow_id, std::string project_label,
                    std::string report_prefix);

  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;

private:
  std::wstring workflow_id_;
  std::string project_label_;
  std::string report_prefix_;
};

} // namespace uds
