#pragma once

#include "core/can_bus.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace uds {

inline constexpr std::uint32_t kCheryE0yWakeupId = 0x600;
inline constexpr std::chrono::milliseconds kCheryE0yWakeupPeriod{1000};
inline constexpr std::chrono::milliseconds kCheryE0yWakeupSettle{15000};

struct CheryE0yWakeupTiming {
  std::chrono::milliseconds period{kCheryE0yWakeupPeriod};
  std::chrono::milliseconds settle{kCheryE0yWakeupSettle};
};

CanFrame chery_e0y_wakeup_frame();

// Owns the E0Y bench wake-up stream.  Both read-only probing and formal
// flashing use this one project-specific implementation so their entry
// conditions cannot drift apart.
class CheryE0yWakeupSession final {
public:
  using Log = std::function<void(const std::string&)>;

  CheryE0yWakeupSession(ICanBus& bus, std::stop_token operation_stop,
                        Log log = {}, CheryE0yWakeupTiming timing = {});
  ~CheryE0yWakeupSession();

  CheryE0yWakeupSession(const CheryE0yWakeupSession&) = delete;
  CheryE0yWakeupSession& operator=(const CheryE0yWakeupSession&) = delete;

  void start();
  void wait_until_settled();
  void check() const;
  void stop_and_check();

private:
  void stop_sender() noexcept;

  ICanBus& bus_;
  std::stop_token operation_stop_;
  Log log_;
  CheryE0yWakeupTiming timing_;
  mutable std::mutex error_mutex_;
  std::string error_;
  std::jthread sender_;
};

} // namespace uds
