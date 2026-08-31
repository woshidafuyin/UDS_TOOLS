#pragma once

#include "core/asc_trace.hpp"
#include "core/bus_monitor_trace.hpp"
#include "drivers/can/can_bus_provider.hpp"

#include <filesystem>
#include <memory>

namespace uds {

class TracingCanBusProvider final : public ICanBusProvider {
public:
  TracingCanBusProvider(std::shared_ptr<ICanBusProvider> inner,
                        std::filesystem::path trace_path,
                        unsigned channel);

  [[nodiscard]] std::unique_ptr<ICanBus> create(
      CanChannelConfig config) const override;
  [[nodiscard]] bool trace_is_open() const noexcept {
    return asc_trace_ && asc_trace_->is_open() && blf_trace_ &&
           blf_trace_->is_open();
  }
  [[nodiscard]] bool asc_trace_is_open() const noexcept {
    return asc_trace_ && asc_trace_->is_open();
  }
  [[nodiscard]] bool blf_trace_is_open() const noexcept {
    return blf_trace_ && blf_trace_->is_open();
  }
  [[nodiscard]] const std::filesystem::path& asc_trace_path() const noexcept {
    return asc_trace_->path();
  }
  [[nodiscard]] std::filesystem::path blf_trace_path() const {
    return blf_trace_path_;
  }

private:
  std::shared_ptr<ICanBusProvider> inner_;
  std::shared_ptr<AscTraceWriter> asc_trace_;
  std::shared_ptr<BusMonitorTraceSession> blf_trace_;
  std::filesystem::path blf_trace_path_;
};

} // namespace uds
