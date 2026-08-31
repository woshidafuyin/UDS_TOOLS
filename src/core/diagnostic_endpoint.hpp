#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace uds {

struct DiagnosticEndpoint {
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  bool extended{};
};

inline DiagnosticEndpoint require_configurable_diagnostic_endpoint(
    std::uint32_t tx_id, std::uint32_t rx_id, bool extended,
    std::string_view project_label) {
  const auto maximum = extended ? 0x1FFFFFFFU : 0x7FFU;
  if (tx_id == 0 || rx_id == 0 || tx_id > maximum || rx_id > maximum) {
    throw std::runtime_error(
        std::string(project_label) +
        (extended
             ? " requires non-zero 29-bit APP diagnostic Tx/Rx IDs"
             : " requires non-zero 11-bit APP diagnostic Tx/Rx IDs"));
  }
  return {tx_id, rx_id, extended};
}

} // namespace uds
