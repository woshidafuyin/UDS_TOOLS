#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

class RadarS19Workflow : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;

protected:
  RadarS19Workflow(std::wstring workflow_id, std::string project_name,
                   std::string report_name, bool send_raw_boot_transition,
                   bool compare_lp_reference_crc);

private:
  std::wstring workflow_id_;
  std::string project_name_;
  std::string report_name_;
  bool send_raw_boot_transition_{};
  bool compare_lp_reference_crc_{};
};

class LpArcWorkflow final : public RadarS19Workflow {
public:
  LpArcWorkflow();
};

} // namespace uds
