#include "app/operation_state.hpp"

namespace uds::app {

bool OperationState::try_start(OperationKind kind) {
  if (kind == OperationKind::none) return false;

  std::scoped_lock lock(mutex_);
  if (phase_ != OperationPhase::idle) return false;
  kind_ = kind;
  phase_ = OperationPhase::running;
  return true;
}

bool OperationState::request_stop() {
  std::scoped_lock lock(mutex_);
  if (phase_ == OperationPhase::idle) return false;
  phase_ = OperationPhase::stopping;
  return true;
}

void OperationState::finish() {
  std::scoped_lock lock(mutex_);
  kind_ = OperationKind::none;
  phase_ = OperationPhase::idle;
}

OperationSnapshot OperationState::snapshot() const {
  std::scoped_lock lock(mutex_);
  return {kind_, phase_};
}

bool OperationState::is_active() const {
  return snapshot().phase != OperationPhase::idle;
}

bool OperationState::is_running() const {
  return snapshot().phase == OperationPhase::running;
}

} // namespace uds::app
