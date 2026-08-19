#pragma once

#include "drivers/can/unsupported_can_adapter.hpp"

namespace uds {

class OtherCanAdapter final : public UnsupportedCanAdapter {
public:
  OtherCanAdapter() : UnsupportedCanAdapter(CanVendor::Other, "Other") {}
};

} // namespace uds
