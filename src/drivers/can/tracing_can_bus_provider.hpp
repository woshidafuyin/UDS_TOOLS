#pragma once

#include "core/asc_trace.hpp"
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
    return trace_ && trace_->is_open();
  }
  [[nodiscard]] const std::filesystem::path& trace_path() const noexcept {
    return trace_->path();
  }

private:
  std::shared_ptr<ICanBusProvider> inner_;
  std::shared_ptr<AscTraceWriter> trace_;
};

} // namespace uds
