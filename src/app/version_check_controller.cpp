#include "app/version_check_controller.hpp"

#include <utility>

namespace uds::app {

VersionCheckController::VersionCheckController(OperationState& state,
                                               VersionCheckService service)
    : state_(state), service_(std::move(service)) {}

VersionCheckController::~VersionCheckController() {
  request_stop();
  wait();
}

bool VersionCheckController::start(
    VersionCheckRequest request, VersionCheckControllerCallbacks callbacks) {
  std::scoped_lock lock(worker_mutex_);
  if (!state_.try_start(OperationKind::version_check)) return false;
  try {
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread(
        [this, request = std::move(request),
         callbacks = std::move(callbacks)](std::stop_token stop) mutable {
          execute(std::move(request), std::move(callbacks), stop);
        });
  } catch (...) {
    state_.finish();
    throw;
  }
  return true;
}

bool VersionCheckController::request_stop() {
  std::scoped_lock lock(worker_mutex_);
  const auto current = state_.snapshot();
  if (current.kind != OperationKind::version_check ||
      current.phase == OperationPhase::idle) {
    return false;
  }
  state_.request_stop();
  if (worker_.joinable()) worker_.request_stop();
  return true;
}

void VersionCheckController::wait() {
  std::scoped_lock lock(worker_mutex_);
  if (worker_.joinable()) worker_.join();
}

bool VersionCheckController::is_active() const {
  const auto current = state_.snapshot();
  return current.kind == OperationKind::version_check &&
         current.phase != OperationPhase::idle;
}

void VersionCheckController::execute(
    VersionCheckRequest request, VersionCheckControllerCallbacks callbacks,
    std::stop_token stop) {
  VersionCheckCallbacks service_callbacks;
  service_callbacks.onLog = callbacks.onLog;
  service_callbacks.onProgress = callbacks.onProgress;
  VersionCheckResult result;
  try {
    result = service_.run(request, service_callbacks, stop);
  } catch (...) {
    result.cancelled = stop.stop_requested();
    result.message = result.cancelled
                         ? "版本读取已停止"
                         : "ERROR：版本读取失败：unknown exception";
  }
  state_.finish();
  if (callbacks.onFinished) {
    try {
      callbacks.onFinished(std::move(result));
    } catch (...) {
    }
  }
}

} // namespace uds::app
