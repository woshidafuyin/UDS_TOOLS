#pragma once

#include "drivers/can/can_bus_provider.hpp"

#include <memory>

namespace uds {

// CANoe-style channel fan-out: one hardware receive loop per physical channel
// and one independent receive queue per client session. A trace subscriber
// therefore observes frames without consuming diagnostic responses.
class SharedCanBusProvider final : public ICanBusProvider {
public:
  explicit SharedCanBusProvider(std::shared_ptr<ICanBusProvider> inner);
  ~SharedCanBusProvider() override;

  [[nodiscard]] std::unique_ptr<ICanBus> create(
      CanChannelConfig config) const override;

private:
  class State;
  std::shared_ptr<State> state_;
};

} // namespace uds
