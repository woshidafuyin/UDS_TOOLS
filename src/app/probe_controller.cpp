#include "app/probe_controller.hpp"

#include <utility>

namespace uds::app {

ProbeController::ProbeController(OperationState& state, ProbeService service)
    : state_(state), service_(std::move(service)) {}

ProbeController::~ProbeController() {
  request_stop();
  wait();
}

bool ProbeController::start(ProbeRequest request,
                            ProbeControllerCallbacks callbacks,
                            OperationId* started_id) {
  std::scoped_lock lock(worker_mutex_);
  OperationId operation_id{};
  if (!state_.try_start(OperationKind::probe, &operation_id)) return false;
  if (started_id) *started_id = operation_id;
  try {
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread(
        [this, request = std::move(request), callbacks = std::move(callbacks),
         operation_id](
            std::stop_token stop) mutable {
          execute(std::move(request), std::move(callbacks), stop, operation_id);
        });
  } catch (...) {
    state_.finish(operation_id);
    throw;
  }
  return true;
}

bool ProbeController::request_stop() {
  std::scoped_lock lock(worker_mutex_);
  const auto current = state_.snapshot();
  if (current.kind != OperationKind::probe ||
      current.phase == OperationPhase::idle) {
    return false;
  }
  state_.request_stop();
  if (worker_.joinable()) worker_.request_stop();
  return true;
}

void ProbeController::wait() {
  std::scoped_lock lock(worker_mutex_);
  if (worker_.joinable()) worker_.join();
}

bool ProbeController::is_active() const {
  const auto current = state_.snapshot();
  return current.kind == OperationKind::probe &&
         current.phase != OperationPhase::idle;
}

void ProbeController::execute(ProbeRequest request,
                              ProbeControllerCallbacks callbacks,
                              std::stop_token stop,
                              OperationId operation_id) {
  ProbeServiceCallbacks service_callbacks;
  service_callbacks.onLog = [&](const std::string& line) {
    if (callbacks.onLog) callbacks.onLog(line);
  };
  service_callbacks.onProgress = [&](int value, const std::string& line) {
    if (callbacks.onProgress) callbacks.onProgress(value, line);
  };

  ProbeResult result;
  try {
    result = service_.run(request, service_callbacks, stop);
  } catch (...) {
    result.cancelled = stop.stop_requested();
    result.message = result.cancelled ? "在线探测已停止"
                                      : "在线探测失败";
  }

  state_.finish(operation_id);
  if (callbacks.onFinished) {
    try {
      callbacks.onFinished(std::move(result));
    } catch (...) {
      // A presentation callback must not escape the worker thread.
    }
  }
}

} // namespace uds::app
