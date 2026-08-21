#pragma once

#include "core/can_bus.hpp"
#include "core/profile.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace uds::app {

struct DiagnosticRequest {
  FlashProfile profile;
  unsigned channel{};
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  std::vector<std::uint8_t> payload;
  unsigned timeout_ms{2000};
  std::filesystem::path trace_file;
};

struct DiagnosticRequestResult {
  bool success{};
  bool cancelled{};
  std::string message;
  std::string request_hex;
  std::string response_hex;
  unsigned elapsed_ms{};
  std::uint8_t nrc{};
};

class DiagnosticRequestService {
public:
  using BusFactory =
      std::function<std::unique_ptr<ICanBus>(const DiagnosticRequest&)>;
  explicit DiagnosticRequestService(BusFactory bus_factory = {});

  [[nodiscard]] DiagnosticRequestResult run(const DiagnosticRequest& request,
                                             std::stop_token stop) const;

private:
  BusFactory bus_factory_;
};

} // namespace uds::app
