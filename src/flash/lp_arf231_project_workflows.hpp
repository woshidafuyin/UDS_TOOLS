#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

class LpArf231A12Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&,
           std::stop_token) override;
};

class LpArf231B11Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&,
           std::stop_token) override;
};

} // namespace uds
