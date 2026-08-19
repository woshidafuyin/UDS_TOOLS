#pragma once

#include "drivers/can/can_hardware_adapter.hpp"

#include <memory>

namespace uds {

class CanAdapterFactory {
public:
  [[nodiscard]] static std::unique_ptr<ICanHardwareAdapter> create(
      CanVendor vendor);
};

} // namespace uds
