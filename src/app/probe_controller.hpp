#pragma once

#include "app/operation_state.hpp"
#include "app/probe_service.hpp"

#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace uds::app {

struct ProbeControllerCallbacks {
  std::function<void(const std::string&)> onLog;
  std::function<void(int, const std::string&)> onProgress;
  std::function<void(ProbeResult)> onFinished;
};

class ProbeController {
public:
  explicit ProbeController(OperationState& state,
                           ProbeService service = ProbeService{});
  ~ProbeController();

  ProbeController(const ProbeController&) = delete;
  ProbeController& operator=(const ProbeController&) = delete;

  bool start(ProbeRequest request, ProbeControllerCallbacks callbacks);
  bool request_stop();
  void wait();

  [[nodiscard]] bool is_active() const;

private:
  void execute(ProbeRequest request, ProbeControllerCallbacks callbacks,
               std::stop_token stop);

  OperationState& state_;
  ProbeService service_;
  mutable std::mutex worker_mutex_;
  std::jthread worker_;
};

} // namespace uds::app
