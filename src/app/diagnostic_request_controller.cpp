#include "app/diagnostic_request_controller.hpp"

namespace uds::app {

DiagnosticRequestController::DiagnosticRequestController(
    OperationState& state, DiagnosticRequestService service)
    : state_(state), service_(std::move(service)) {}

DiagnosticRequestController::~DiagnosticRequestController() {
  request_stop();
  wait();
}

bool DiagnosticRequestController::start(DiagnosticRequest request,
                                        Finished finished) {
  std::scoped_lock lock(mutex_);
  if (!state_.try_start(OperationKind::diagnostic_request)) return false;
  if (worker_.joinable()) worker_.join();
  worker_ = std::jthread(
      [this, request = std::move(request), finished = std::move(finished)](
          std::stop_token stop) mutable {
        auto result = service_.run(request, stop);
        state_.finish();
        if (finished) finished(std::move(result));
      });
  return true;
}

bool DiagnosticRequestController::request_stop() {
  std::scoped_lock lock(mutex_);
  const auto current = state_.snapshot();
  if (current.kind != OperationKind::diagnostic_request) return false;
  const auto accepted = state_.request_stop();
  if (worker_.joinable()) worker_.request_stop();
  return accepted;
}

void DiagnosticRequestController::wait() {
  std::jthread worker;
  {
    std::scoped_lock lock(mutex_);
    worker = std::move(worker_);
  }
  if (worker.joinable()) worker.join();
}

} // namespace uds::app
