#include "drivers/can/can_bus_provider.hpp"

#include "drivers/can/can_adapter_factory.hpp"
#include "drivers/can/can_bus_session.hpp"
#include "drivers/can/shared_can_bus_provider.hpp"

#include <array>
#include <atomic>
#include <utility>

namespace uds {
namespace {

std::atomic<CanVendor> selected_vendor{CanVendor::Vector};

class SelectedCanBusProvider final : public ICanBusProvider {
public:
  [[nodiscard]] std::unique_ptr<ICanBus> create(
      CanChannelConfig config) const override {
    const auto vendor = selected_vendor.load(std::memory_order_relaxed);
    return provider(vendor)->create(std::move(config));
  }

private:
  static const std::shared_ptr<ICanBusProvider>& provider(CanVendor vendor) {
    static const std::array<std::shared_ptr<ICanBusProvider>, 5> providers{
        std::make_shared<SharedCanBusProvider>(
            std::make_shared<DefaultCanBusProvider>(CanVendor::Vector)),
        std::make_shared<SharedCanBusProvider>(
            std::make_shared<DefaultCanBusProvider>(CanVendor::Zlg)),
        std::make_shared<SharedCanBusProvider>(
            std::make_shared<DefaultCanBusProvider>(CanVendor::Tosun)),
        std::make_shared<SharedCanBusProvider>(
            std::make_shared<DefaultCanBusProvider>(CanVendor::Kvaser)),
        std::make_shared<SharedCanBusProvider>(
            std::make_shared<DefaultCanBusProvider>(CanVendor::Other)),
    };
    return providers.at(static_cast<std::size_t>(vendor));
  }
};

} // namespace

std::unique_ptr<ICanBus> DefaultCanBusProvider::create(
    CanChannelConfig config) const {
  return std::make_unique<CanBusSession>(CanAdapterFactory::create(vendor_),
                                         std::move(config));
}

std::shared_ptr<ICanBusProvider> default_can_bus_provider() {
  static const auto provider = std::make_shared<SelectedCanBusProvider>();
  return provider;
}

void set_default_can_vendor(CanVendor vendor) noexcept {
  selected_vendor.store(vendor, std::memory_order_relaxed);
}

CanVendor default_can_vendor() noexcept {
  return selected_vendor.load(std::memory_order_relaxed);
}

std::string_view can_vendor_name(CanVendor vendor) noexcept {
  switch (vendor) {
  case CanVendor::Vector:
    return "Vector XL";
  case CanVendor::Tosun:
    return "TOSUN / TSMaster (TSCAN)";
  case CanVendor::Zlg:
    return "ZLG / ZCANPRO (ZCAN)";
  case CanVendor::Kvaser:
    return "Kvaser (CANlib)";
  case CanVendor::Other:
    return "Other";
  }
  return "Other";
}

} // namespace uds
