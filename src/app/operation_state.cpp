#include "app/operation_state.hpp"

namespace uds::app {

bool OperationState::try_start(OperationKind kind, OperationId* started_id) {
  if (kind == OperationKind::none) return false;

  std::scoped_lock lock(mutex_);
  if (phase_ != OperationPhase::idle) return false;
  do {
    ++latest_id_;
  } while (latest_id_ == 0);
  kind_ = kind;
  phase_ = OperationPhase::running;
  if (started_id) *started_id = latest_id_;
  return true;
}

bool OperationState::request_stop() {
  std::scoped_lock lock(mutex_);
  if (phase_ == OperationPhase::idle) return false;
  phase_ = OperationPhase::stopping;
  return true;
}

bool OperationState::finish(OperationId id) {
  if (id == 0) return false;
  std::scoped_lock lock(mutex_);
  if (id != latest_id_ || phase_ == OperationPhase::idle) return false;
  kind_ = OperationKind::none;
  phase_ = OperationPhase::idle;
  return true;
}

OperationSnapshot OperationState::snapshot() const {
  std::scoped_lock lock(mutex_);
  return {kind_, phase_, latest_id_};
}

bool OperationState::is_active() const {
  return snapshot().phase != OperationPhase::idle;
}

bool OperationState::is_running() const {
  return snapshot().phase == OperationPhase::running;
}

bool OperationState::is_latest(OperationId id) const {
  if (id == 0) return false;
  std::scoped_lock lock(mutex_);
  return latest_id_ == id;
}

} // namespace uds::app
