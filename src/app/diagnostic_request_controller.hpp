#pragma once

#include "app/diagnostic_request_service.hpp"
#include "app/operation_state.hpp"

#include <functional>
#include <mutex>
#include <thread>

namespace uds::app {

class DiagnosticRequestController {
public:
  using Finished = std::function<void(DiagnosticRequestResult)>;
  explicit DiagnosticRequestController(
      OperationState& state,
      DiagnosticRequestService service = DiagnosticRequestService{});
  ~DiagnosticRequestController();
  bool start(DiagnosticRequest request, Finished finished);
  bool request_stop();
  void wait();

private:
  OperationState& state_;
  DiagnosticRequestService service_;
  std::mutex mutex_;
  std::jthread worker_;
};

} // namespace uds::app
