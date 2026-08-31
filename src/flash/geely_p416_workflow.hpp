#pragma once

#include "core/diagnostic_endpoint.hpp"
#include "flash/flash_workflow.hpp"

namespace uds {

struct GeelyP416EndpointRouting {
  DiagnosticEndpoint app;
  DiagnosticEndpoint programming;
};

GeelyP416EndpointRouting resolve_geely_p416_endpoint_routing(
    const FlashProfile& profile);

class GeelyP416Workflow final : public FlashWorkflow {
public:
  std::wstring_view id() const noexcept override;
  std::string report_title(const FlashProfile& profile) const override;
  void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override;
};

} // namespace uds
