#pragma once

#include <string>

namespace uds {

struct CanoePowerResult {
  bool measurement_started{};
  int value{};
  std::wstring configuration;
};

// Controls the same CANoe system variable used by the proven CAPL fixture:
//   IO::VN1600_1::DOUT
// CANoe owns the VN16xx DAIO configuration; this client only uses COM.
CanoePowerResult set_canoe_dout(int value);

} // namespace uds
