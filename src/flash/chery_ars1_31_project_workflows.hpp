#pragma once

#include "flash/flash_workflow.hpp"

namespace uds {

class CheryT1ejWorkflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&,
           std::stop_token) override;
};

class CheryT22Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&,
           std::stop_token) override;
};

class CheryE0yWorkflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile&) const override;
  void run(const FlashJob&, const FlashWorkflowCallbacks&,
           std::stop_token) override;
};

} // namespace uds
