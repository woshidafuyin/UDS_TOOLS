#include "flash/chery_e0y_wakeup.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace uds {
namespace {
using namespace std::chrono_literals;

void throw_if_cancelled(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}
} // namespace

CanFrame chery_e0y_wakeup_frame() {
  return {kCheryE0yWakeupId,
          {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          false, false, false};
}

CheryE0yWakeupSession::CheryE0yWakeupSession(
    ICanBus& bus, std::stop_token operation_stop, Log log,
    CheryE0yWakeupTiming timing)
    : bus_(bus), operation_stop_(operation_stop), log_(std::move(log)),
      timing_(timing) {
  if (timing_.period <= 0ms || timing_.settle <= 0ms ||
      timing_.retry <= 0ms) {
    throw std::invalid_argument("E0Y wake-up timing must be positive");
  }
}

CheryE0yWakeupSession::~CheryE0yWakeupSession() { stop_sender(); }

void CheryE0yWakeupSession::start() {
  if (sender_.joinable()) {
    throw std::logic_error("E0Y wake-up sender already started");
  }
  throw_if_cancelled(operation_stop_);
  started_at_ = std::chrono::steady_clock::now();
  const auto frame = chery_e0y_wakeup_frame();
  bus_.send(frame);
  if (log_) {
    log_("E0Y wake-up active: 0x600 00 00 00 00 00 00 00 00 @" +
         std::to_string(timing_.period.count()) + "ms");
  }
  sender_ = std::jthread([this, frame](std::stop_token sender_stop) {
    auto next = std::chrono::steady_clock::now() + timing_.period;
    while (!sender_stop.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next) {
        try {
          bus_.send(frame);
        } catch (const std::exception& error) {
          std::scoped_lock lock(error_mutex_);
          error_ = error.what();
          return;
        } catch (...) {
          std::scoped_lock lock(error_mutex_);
          error_ = "unknown 0x600 transmit error";
          return;
        }
        do {
          next += timing_.period;
        } while (next <= now);
      }
      std::this_thread::sleep_for(2ms);
    }
  });
}

bool CheryE0yWakeupSession::wait_until_ready(
    const std::function<bool(std::chrono::milliseconds)>& probe) {
  if (!sender_.joinable()) {
    throw std::logic_error("E0Y wake-up sender is not started");
  }
  if (!probe) throw std::invalid_argument("E0Y readiness probe is required");

  // Match the established project pattern: give the network one wake-up
  // period, then let the diagnostic response decide readiness.  The CANoe
  // 15-second value remains the total upper bound instead of an unconditional
  // delay before the first request.
  const auto deadline = started_at_ + timing_.settle;
  const auto first_probe =
      started_at_ + std::min(timing_.period, timing_.settle);
  while (std::chrono::steady_clock::now() < first_probe) {
    throw_if_cancelled(operation_stop_);
    check();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        first_probe - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 20ms));
  }

  std::size_t attempt{};
  while (std::chrono::steady_clock::now() < deadline) {
    throw_if_cancelled(operation_stop_);
    check();
    ++attempt;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto request_timeout = std::min(remaining, 1000ms);
    if (request_timeout > 0ms && probe(request_timeout)) {
      if (log_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_);
        log_("E0Y diagnostic ready after " +
             std::to_string(elapsed.count()) + "ms (attempt " +
             std::to_string(attempt) + ")");
      }
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    const auto retry_at =
        std::min(deadline,
                 std::chrono::steady_clock::now() + timing_.retry);
    while (std::chrono::steady_clock::now() < retry_at) {
      throw_if_cancelled(operation_stop_);
      check();
      const auto remaining_retry =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              retry_at - std::chrono::steady_clock::now());
      std::this_thread::sleep_for(std::min(remaining_retry, 20ms));
    }
  }
  check();
  return false;
}

void CheryE0yWakeupSession::wait_until_settled() {
  if (!sender_.joinable()) {
    throw std::logic_error("E0Y wake-up sender is not started");
  }
  if (log_) {
    log_("E0Y wake-up settling for " +
         std::to_string(timing_.settle.count()) +
         "ms before the first diagnostic request");
  }
  const auto deadline = std::chrono::steady_clock::now() + timing_.settle;
  while (std::chrono::steady_clock::now() < deadline) {
    throw_if_cancelled(operation_stop_);
    check();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 20ms));
  }
  check();
}

void CheryE0yWakeupSession::check() const {
  std::scoped_lock lock(error_mutex_);
  if (!error_.empty()) {
    throw std::runtime_error("E0Y periodic 0x600 wake-up failed: " + error_);
  }
}

void CheryE0yWakeupSession::stop_sender() noexcept {
  if (!sender_.joinable()) return;
  sender_.request_stop();
  sender_.join();
}

void CheryE0yWakeupSession::stop_and_check() {
  stop_sender();
  check();
}

} // namespace uds
