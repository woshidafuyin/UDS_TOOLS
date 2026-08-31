#include "drivers/can/tracing_can_bus_provider.hpp"

#include <stdexcept>
#include <utility>

namespace uds {

TracingCanBusProvider::TracingCanBusProvider(
    std::shared_ptr<ICanBusProvider> inner, std::filesystem::path trace_path,
    unsigned channel)
    : inner_(std::move(inner)) {
  if (!inner_) {
    throw std::invalid_argument("tracing CAN provider requires an inner provider");
  }
  auto blf_path = trace_path;
  blf_path.replace_extension(L".blf");
  blf_trace_path_ = blf_path;
  asc_trace_ =
      std::make_shared<AscTraceWriter>(std::move(trace_path), channel);
  blf_trace_ = std::make_shared<BusMonitorTraceSession>(
      blf_path.parent_path(), blf_path);
  (void)blf_trace_->start(channel);
}

std::unique_ptr<ICanBus> TracingCanBusProvider::create(
    CanChannelConfig config) const {
  auto bus = inner_->create(std::move(config));
  if (!bus) throw std::runtime_error("inner CAN provider returned null");
  return std::make_unique<TracingCanBus>(
      std::move(bus),
      std::vector<std::shared_ptr<ICanTraceWriter>>{asc_trace_, blf_trace_});
}

} // namespace uds
