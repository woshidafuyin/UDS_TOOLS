#pragma once
#include "flash/flash_workflow.hpp"

namespace uds {
class PeroduaP02cWorkflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override { return L"perodua_p02c"; }
  std::string report_title(const FlashProfile&) const override {
    return "Perodua P02C CPD CES012 Flash Report";
  }
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;
};
} // namespace uds
