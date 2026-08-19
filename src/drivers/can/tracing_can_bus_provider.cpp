#include "drivers/can/tracing_can_bus_provider.hpp"

#include <stdexcept>
#include <utility>

namespace uds {

TracingCanBusProvider::TracingCanBusProvider(
    std::shared_ptr<ICanBusProvider> inner, std::filesystem::path trace_path,
    unsigned channel)
    : inner_(std::move(inner)),
      trace_(std::make_shared<AscTraceWriter>(std::move(trace_path), channel)) {
  if (!inner_) {
    throw std::invalid_argument("tracing CAN provider requires an inner provider");
  }
}

std::unique_ptr<ICanBus> TracingCanBusProvider::create(
    CanChannelConfig config) const {
  auto bus = inner_->create(std::move(config));
  if (!bus) throw std::runtime_error("inner CAN provider returned null");
  return std::make_unique<TracingCanBus>(std::move(bus), trace_);
}

} // namespace uds
