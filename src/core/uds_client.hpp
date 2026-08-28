#pragma once

#include "core/isotp.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace uds {

struct UdsResponse {
  bool success{};
  std::vector<std::uint8_t> request;
  std::vector<std::uint8_t> response;
  std::uint8_t nrc{};
  std::string detail;
  std::chrono::milliseconds elapsed{};
};

enum class UdsObservationKind {
  positive,
  negative,
  timeout,
};

struct UdsObservation {
  UdsObservationKind kind{UdsObservationKind::timeout};
  std::vector<std::uint8_t> request;
  std::vector<std::uint8_t> response;
  std::uint8_t nrc{};
  std::string detail;
  std::chrono::milliseconds elapsed{};
};

class UdsClient {
public:
  using Logger = std::function<void(const std::string&)>;
  explicit UdsClient(IsoTpSession& transport, Logger logger = {},
                     std::stop_token stop = {});
  UdsResponse request(std::span<const std::uint8_t> payload,
                       std::chrono::milliseconds p2 = std::chrono::milliseconds(2000),
                       std::chrono::milliseconds p2_star = std::chrono::milliseconds(5000),
                       std::stop_token stop = {});
  // Send a complete UDS request and consume its response, but report a final
  // negative response or no-response timeout as data instead of throwing.
  // Transport/send failures and malformed ISO-TP traffic remain hard errors.
  UdsObservation request_observe(
      std::span<const std::uint8_t> payload,
      std::chrono::milliseconds response_window,
      std::chrono::milliseconds pending_window,
      std::stop_token stop = {});
  void send_only(std::span<const std::uint8_t> payload,
                 std::stop_token stop = {});

private:
  [[nodiscard]] std::stop_token effective_stop(
      std::stop_token request_stop) const noexcept;
  IsoTpSession& transport_;
  Logger logger_;
  std::stop_token stop_;
};

} // namespace uds
