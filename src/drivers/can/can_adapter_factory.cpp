#include "drivers/can/can_adapter_factory.hpp"

#include "drivers/can/other/other_can_adapter.hpp"
#include "drivers/can/unsupported_can_adapter.hpp"

#if UDS_ENABLE_VECTOR
#include "drivers/can/vector/vector_can_adapter.hpp"
#endif
#if UDS_ENABLE_TOSUN
#include "drivers/can/tosun/tosun_can_adapter.hpp"
#endif
#if UDS_ENABLE_ZLG
#include "drivers/can/zlg/zlg_can_adapter.hpp"
#endif
#if UDS_ENABLE_KVASER
#include "drivers/can/kvaser/kvaser_can_adapter.hpp"
#endif

#include <memory>

namespace uds {

std::unique_ptr<ICanHardwareAdapter> CanAdapterFactory::create(
    CanVendor vendor) {
  switch (vendor) {
  case CanVendor::Vector:
#if UDS_ENABLE_VECTOR
    return std::make_unique<VectorCanAdapter>();
#else
    return std::make_unique<UnsupportedCanAdapter>(
        CanVendor::Vector, "Vector",
        CanAdapterErrorCode::Unsupported);
#endif
  case CanVendor::Tosun:
#if UDS_ENABLE_TOSUN
    return std::make_unique<TosunCanAdapter>();
#else
    return std::make_unique<UnsupportedCanAdapter>(
        CanVendor::Tosun, "TOSUN/libTSCAN",
        CanAdapterErrorCode::Unsupported);
#endif
  case CanVendor::Zlg:
#if UDS_ENABLE_ZLG
    return std::make_unique<ZlgCanAdapter>();
#else
    return std::make_unique<UnsupportedCanAdapter>(
        CanVendor::Zlg, "ZLG/ZCAN",
        CanAdapterErrorCode::Unsupported);
#endif
  case CanVendor::Kvaser:
#if UDS_ENABLE_KVASER
    return std::make_unique<KvaserCanAdapter>();
#else
    return std::make_unique<UnsupportedCanAdapter>(
        CanVendor::Kvaser, "Kvaser CANlib",
        CanAdapterErrorCode::Unsupported);
#endif
  case CanVendor::Other:
    return std::make_unique<OtherCanAdapter>();
  }
  return std::make_unique<OtherCanAdapter>();
}

} // namespace uds
