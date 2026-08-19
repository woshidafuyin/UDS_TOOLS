#pragma once

#include "core/can_bus.hpp"
#include "drivers/can/can_hardware_adapter.hpp"

#include <memory>
#include <string_view>

namespace uds {

class ICanBusProvider {
public:
  virtual ~ICanBusProvider() = default;
  [[nodiscard]] virtual std::unique_ptr<ICanBus> create(
      CanChannelConfig config) const = 0;
};

class DefaultCanBusProvider final : public ICanBusProvider {
public:
  explicit DefaultCanBusProvider(CanVendor vendor = CanVendor::Vector)
      : vendor_(vendor) {}

  [[nodiscard]] std::unique_ptr<ICanBus> create(
      CanChannelConfig config) const override;
  [[nodiscard]] CanVendor vendor() const noexcept { return vendor_; }

private:
  CanVendor vendor_;
};

[[nodiscard]] std::shared_ptr<ICanBusProvider> default_can_bus_provider();
void set_default_can_vendor(CanVendor vendor) noexcept;
[[nodiscard]] CanVendor default_can_vendor() noexcept;
[[nodiscard]] std::string_view can_vendor_name(CanVendor vendor) noexcept;

} // namespace uds
