#pragma once

#include "app/probe_plan.hpp"
#include "flash/chery_e0y_wakeup.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace uds::app::probe_detail {

class ProbePreconditions final {
public:
  ProbePreconditions(ICanBus& bus, const ProbePlan& plan,
                     const ProbeServiceCallbacks& callbacks,
                     std::stop_token stop);
  ~ProbePreconditions();

  ProbePreconditions(const ProbePreconditions&) = delete;
  ProbePreconditions& operator=(const ProbePreconditions&) = delete;

  void start();
  bool wait_for_e0y_ready(
      const std::function<bool(std::chrono::milliseconds)>& probe);
  void check() const;
  void stop_and_check();

private:
  void stop_sender() noexcept;

  ICanBus& bus_;
  const ProbePlan& plan_;
  const ProbeServiceCallbacks& callbacks_;
  std::stop_token stop_;
  mutable std::mutex error_mutex_;
  std::string error_;
  std::jthread sender_;
  std::unique_ptr<CheryE0yWakeupSession> e0y_wakeup_;
};

} // namespace uds::app::probe_detail
