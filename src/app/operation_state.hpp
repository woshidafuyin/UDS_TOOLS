#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace uds::app {

enum class OperationKind {
  none,
  probe,
  flash,
  version_check,
  diagnostic_request,
};

enum class OperationPhase {
  idle,
  running,
  stopping,
};

struct OperationSnapshot {
  OperationKind kind{OperationKind::none};
  OperationPhase phase{OperationPhase::idle};
};

struct OperationResult {
  bool success{};
  bool cancelled{};
  std::wstring message;
  std::filesystem::path report_path;
};

// Callbacks may be invoked from a controller worker thread. The UI adapter is
// responsible for dispatching them to its own UI thread.
struct OperationCallbacks {
  std::function<void(const std::string&)> onLog;
  std::function<void(int, const std::string&)> onProgress;
  std::function<void(OperationResult)> onFinished;
};

// Shared gate for probe, flash and version-check controllers. It prevents
// hardware operations from running concurrently without a UI dependency.
class OperationState {
public:
  bool try_start(OperationKind kind);
  bool request_stop();
  void finish();

  [[nodiscard]] OperationSnapshot snapshot() const;
  [[nodiscard]] bool is_active() const;
  [[nodiscard]] bool is_running() const;

private:
  mutable std::mutex mutex_;
  OperationKind kind_{OperationKind::none};
  OperationPhase phase_{OperationPhase::idle};
};

} // namespace uds::app
