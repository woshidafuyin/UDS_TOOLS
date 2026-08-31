#pragma once

#include "app/probe_plan.hpp"

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
};

} // namespace uds::app::probe_detail
