#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

enum class BaicRadarProject { n61ab, bqb41 };

class BaicRadarWorkflow final : public FlashWorkflow {
public:
  explicit BaicRadarWorkflow(BaicRadarProject project);
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;

private:
  BaicRadarProject project_;
};

} // namespace uds
