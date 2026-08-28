#include "core/uds_client.hpp"
#include "core/hex.hpp"
#include "core/uds_nrc.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace uds {
namespace {
std::string log_payload(std::span<const std::uint8_t> payload) {
  constexpr std::size_t preview_size = 16;
  if (payload.size() <= 64) return to_hex(payload);
  return to_hex(payload.first(preview_size)) + " ... [" +
         std::to_string(payload.size()) + " bytes]";
}

std::string format_can_id(std::uint32_t can_id) {
  std::ostringstream stream;
  stream << "0x" << std::uppercase << std::hex << can_id;
  return stream.str();
}
} // namespace

UdsClient::UdsClient(IsoTpSession& transport, Logger logger,
                     std::stop_token stop)
    : transport_(transport), logger_(std::move(logger)), stop_(stop) {}

std::stop_token UdsClient::effective_stop(
    std::stop_token request_stop) const noexcept {
  return request_stop.stop_possible() ? request_stop : stop_;
}

void UdsClient::send_only(std::span<const std::uint8_t> payload,
                          std::stop_token stop) {
  if (logger_) {
    logger_("TX [" + format_can_id(transport_.tx_id()) + "] " + log_payload(payload));
  }
  try {
    transport_.send(payload, effective_stop(stop));
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("UDS transport send failed: ") + error.what());
  }
}

UdsResponse UdsClient::request(std::span<const std::uint8_t> payload,
                               std::chrono::milliseconds p2,
                               std::chrono::milliseconds p2_star,
                               std::stop_token stop) {
  if (payload.empty()) throw std::invalid_argument("empty UDS request");
  const auto started = std::chrono::steady_clock::now();
  if (logger_) {
    logger_("TX [" + format_can_id(transport_.tx_id()) + "] " + log_payload(payload));
  }
  const auto cancellation = effective_stop(stop);
  transport_.send(payload, cancellation);
  auto timeout = p2;
  for (;;) {
    std::vector<std::uint8_t> response;
    try {
      response = transport_.receive(timeout, cancellation);
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("UDS response wait failed: ") + error.what());
    }
    const auto negative = parse_uds_negative_response(response);
    const auto matching_negative =
        negative && negative->request_sid == payload[0];
    if (logger_) {
      const auto response_id = transport_.last_rx_id() != 0
                                   ? transport_.last_rx_id()
                                   : transport_.rx_id();
      auto line =
          "RX [" + format_can_id(response_id) + "] " + to_hex(response);
      if (matching_negative) line += " | " + format_uds_nrc(negative->nrc);
      logger_(line);
    }
    if (matching_negative) {
      if (negative->kind == UdsNegativeResponseKind::pending) {
        timeout = p2_star;
        continue;
      }
      const auto nrc = negative->nrc;
      return {false, {payload.begin(), payload.end()}, std::move(response), nrc,
              format_uds_nrc(nrc), std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)};
    }
    if (!response.empty() && response[0] == static_cast<std::uint8_t>(payload[0] + 0x40U)) {
      return {true, {payload.begin(), payload.end()}, std::move(response), 0, "OK",
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)};
    }
  }
}

UdsObservation UdsClient::request_observe(
    std::span<const std::uint8_t> payload,
    std::chrono::milliseconds response_window,
    std::chrono::milliseconds pending_window,
    std::stop_token stop) {
  if (payload.empty()) throw std::invalid_argument("empty UDS request");
  if (response_window <= std::chrono::milliseconds::zero() ||
      pending_window <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("invalid UDS observation window");
  }

  const auto started = std::chrono::steady_clock::now();
  if (logger_) {
    logger_("TX [" + format_can_id(transport_.tx_id()) + "] " +
            log_payload(payload));
  }
  const auto cancellation = effective_stop(stop);
  try {
    transport_.send(payload, cancellation);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("UDS transport send failed: ") +
                             error.what());
  }

  auto deadline = std::chrono::steady_clock::now() + response_window;
  bool pending_window_started = false;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return {UdsObservationKind::timeout,
              {payload.begin(), payload.end()},
              {},
              0,
              "no response within observation window",
              std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                    started)};
    }
    const auto remaining = std::max(
        std::chrono::milliseconds{1},
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
    std::vector<std::uint8_t> response;
    try {
      response = transport_.receive(remaining, cancellation);
    } catch (const IsoTpReceiveTimeout&) {
      const auto finished = std::chrono::steady_clock::now();
      return {UdsObservationKind::timeout,
              {payload.begin(), payload.end()},
              {},
              0,
              "no response within observation window",
              std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                    started)};
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("UDS response observation failed: ") +
                               error.what());
    }

    const auto negative = parse_uds_negative_response(response);
    const auto matching_negative =
        negative && negative->request_sid == payload[0];
    if (logger_) {
      const auto response_id = transport_.last_rx_id() != 0
                                   ? transport_.last_rx_id()
                                   : transport_.rx_id();
      auto line =
          "RX [" + format_can_id(response_id) + "] " + to_hex(response);
      if (matching_negative) line += " | " + format_uds_nrc(negative->nrc);
      logger_(line);
    }
    if (matching_negative) {
      if (negative->kind == UdsNegativeResponseKind::pending) {
        if (!pending_window_started) {
          deadline = std::chrono::steady_clock::now() + pending_window;
          pending_window_started = true;
        }
        continue;
      }
      const auto finished = std::chrono::steady_clock::now();
      return {UdsObservationKind::negative,
              {payload.begin(), payload.end()},
              std::move(response),
              negative->nrc,
              format_uds_nrc(negative->nrc),
              std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                    started)};
    }
    if (!response.empty() &&
        response[0] == static_cast<std::uint8_t>(payload[0] + 0x40U)) {
      const auto finished = std::chrono::steady_clock::now();
      return {UdsObservationKind::positive,
              {payload.begin(), payload.end()},
              std::move(response),
              0,
              "OK",
              std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                    started)};
    }
  }
}

} // namespace uds
