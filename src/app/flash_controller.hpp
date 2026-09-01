#pragma once

#include "app/flash_request.hpp"
#include "app/operation_state.hpp"
#include "flash/flash_workflow.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>

namespace uds::app {

class FlashController {
public:
  using WorkflowFactory =
      std::function<std::unique_ptr<FlashWorkflow>(std::wstring_view)>;

  explicit FlashController(OperationState& state,
                           WorkflowFactory workflow_factory = {},
                           std::shared_ptr<ICanBusProvider> bus_provider = {});
  ~FlashController();

  FlashController(const FlashController&) = delete;
  FlashController& operator=(const FlashController&) = delete;

  bool start(FlashRequest request, OperationCallbacks callbacks,
             OperationId* started_id = nullptr);
  bool request_stop();
  void wait();

  [[nodiscard]] bool is_active() const;

private:
  void execute(FlashRequest request, OperationCallbacks callbacks,
               std::stop_token stop, OperationId operation_id);

  OperationState& state_;
  WorkflowFactory workflow_factory_;
  std::shared_ptr<ICanBusProvider> bus_provider_;
  mutable std::mutex worker_mutex_;
  std::jthread worker_;
};

} // namespace uds::app
