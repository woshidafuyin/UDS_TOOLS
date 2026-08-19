#pragma once

#include "app/operation_state.hpp"
#include "app/version_check_service.hpp"

#include <functional>
#include <mutex>
#include <thread>

namespace uds::app {

struct VersionCheckControllerCallbacks {
  std::function<void(const std::string&)> onLog;
  std::function<void(int, const std::string&)> onProgress;
  std::function<void(VersionCheckResult)> onFinished;
};

class VersionCheckController {
public:
  explicit VersionCheckController(
      OperationState& state,
      VersionCheckService service = VersionCheckService{});
  ~VersionCheckController();

  VersionCheckController(const VersionCheckController&) = delete;
  VersionCheckController& operator=(const VersionCheckController&) = delete;

  bool start(VersionCheckRequest request,
             VersionCheckControllerCallbacks callbacks);
  bool request_stop();
  void wait();
  [[nodiscard]] bool is_active() const;

private:
  void execute(VersionCheckRequest request,
               VersionCheckControllerCallbacks callbacks,
               std::stop_token stop);

  OperationState& state_;
  VersionCheckService service_;
  mutable std::mutex worker_mutex_;
  std::jthread worker_;
};

} // namespace uds::app
