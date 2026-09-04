#include "app/probe_preconditions.hpp"

#include "app/probe_support.hpp"
#include "core/chuneng_arc331_protocol.hpp"
#include "core/high_resolution_timer.hpp"
#include "core/hex.hpp"
#include "flash/chery_ars1_33_flow.hpp"
#include "flash/geely_p416_flow.hpp"
#include "flash/shidaixinan_hjzj_fmr_flow.hpp"
#include "flash/xizhong_rsmr_flow.hpp"

#include <array>
#include <chrono>

namespace uds::app::probe_detail {
namespace {
using namespace std::chrono_literals;
}

ProbePreconditions::ProbePreconditions(
    ICanBus& bus, const ProbePlan& plan,
    const ProbeServiceCallbacks& callbacks, std::stop_token stop)
    : bus_(bus), plan_(plan), callbacks_(callbacks), stop_(stop) {}

ProbePreconditions::~ProbePreconditions() { stop_sender(); }

void ProbePreconditions::stop_sender() noexcept {
  e0y_wakeup_.reset();
  if (!sender_.joinable()) return;
  sender_.request_stop();
  sender_.join();
}

void ProbePreconditions::check() const {
  if (e0y_wakeup_) e0y_wakeup_->check();
  std::scoped_lock lock(error_mutex_);
  if (!error_.empty()) {
    throw std::runtime_error("probe precondition sender failed: " + error_);
  }
}

void ProbePreconditions::stop_and_check() {
  if (e0y_wakeup_) {
    e0y_wakeup_->stop_and_check();
    e0y_wakeup_.reset();
  }
  stop_sender();
  check();
}

void ProbePreconditions::start() {
  if (plan_.chery_e0y) {
    e0y_wakeup_ = std::make_unique<CheryE0yWakeupSession>(
        bus_, stop_, [this](const std::string& message) {
          log(callbacks_, "在线探测：" + message);
        });
    e0y_wakeup_->start();
    e0y_wakeup_->wait_until_settled();
  }

  if (plan_.chuneng_arc331 && !plan_.ft_probe) {
    const auto wakeup_period = kChunengArc331WakeupPeriod;
    const CanFrame wakeup{
        kChunengArc331WakeupId,
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        false, false, false};
    bus_.send(wakeup);
    log(callbacks_,
        "在线探测：楚能ARC331持续发送0x520全零标准CAN唤醒报文，周期" +
            std::to_string(wakeup_period.count()) +
            "ms；保持到在线检查结束。");
    sender_ = std::jthread([this, wakeup, wakeup_period](
                               std::stop_token sender_stop) {
      ScopedHighResolutionTimer timer_resolution;
      auto next = std::chrono::steady_clock::now() + wakeup_period;
      while (!sender_stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
          try {
            bus_.send(wakeup);
          } catch (const std::exception& error) {
            std::scoped_lock lock(error_mutex_);
            error_ = error.what();
            return;
          } catch (...) {
            std::scoped_lock lock(error_mutex_);
            error_ = "unknown 0x520 transmit error";
            return;
          }
          do {
            next += wakeup_period;
          } while (next <= now);
        }
        std::this_thread::sleep_for(2ms);
      }
    });
    for (int elapsed = 0; elapsed < 25; ++elapsed) {
      check_stop(stop_);
      check();
      std::this_thread::sleep_for(20ms);
    }
  }

  if (plan_.geely_p416) {
    const auto wakeup = geely_p416_nm_wakeup_frame();
    bus_.send(wakeup);
    log(callbacks_,
        "在线探测：按成功BLF持续发送0x53F "
        "3F FF FF FF FF FF FF FF标准CAN帧，周期200ms；"
        "稳定1秒后只发送安全的10 01，不进入编程会话。");
    sender_ = std::jthread([this, wakeup](std::stop_token sender_stop) {
      auto next = std::chrono::steady_clock::now() + 200ms;
      while (!sender_stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
          try {
            bus_.send(wakeup);
          } catch (const std::exception& error) {
            std::scoped_lock lock(error_mutex_);
            error_ = error.what();
            return;
          } catch (...) {
            std::scoped_lock lock(error_mutex_);
            error_ = "unknown 0x53F transmit error";
            return;
          }
          do {
            next += 200ms;
          } while (next <= now);
        }
        std::this_thread::sleep_for(2ms);
      }
    });
    for (int elapsed = 0; elapsed < 50; ++elapsed) {
      check_stop(stop_);
      check();
      std::this_thread::sleep_for(20ms);
    }
  }

  if (plan_.request.profile.flow == L"chery_ars1_33") {
    const auto frames = chery_ars133_precondition_frames();
    for (const auto& item : frames) bus_.send(item.frame);
    log(callbacks_,
        "在线探测：发送 0x600 全零唤醒报文(100ms)、"
        "0x25B PowerMode=ON(20ms) 和 0x4B4 Gear=P(100ms)，"
        "稳定1秒后再发送UDS请求。");
    sender_ = std::jthread([this, frames](std::stop_token sender_stop) {
      auto now = std::chrono::steady_clock::now();
      std::array<std::chrono::steady_clock::time_point, 3> next{
          now + frames[0].period, now + frames[1].period,
          now + frames[2].period};
      while (!sender_stop.stop_requested()) {
        now = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < frames.size(); ++index) {
          if (now < next[index]) continue;
          try {
            bus_.send(frames[index].frame);
          } catch (const std::exception& error) {
            std::scoped_lock lock(error_mutex_);
            error_ = error.what();
            return;
          } catch (...) {
            std::scoped_lock lock(error_mutex_);
            error_ = "unknown CAN transmit error";
            return;
          }
          do {
            next[index] += frames[index].period;
          } while (next[index] <= now);
        }
        std::this_thread::sleep_for(1ms);
      }
    });
    for (int elapsed = 0; elapsed < 50; ++elapsed) {
      check_stop(stop_);
      check();
      std::this_thread::sleep_for(20ms);
    }
  }

  if (plan_.ars131_app) {
    const CanFrame precondition{
        0x400, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        false, false, false};
    log(callbacks_,
        "在线探测：按成功基线先发送 0x400 全零前置报文；在线结论仍只依据 UDS 响应。");
    for (int count = 0; count < 10; ++count) {
      check_stop(stop_);
      bus_.send(precondition);
      std::this_thread::sleep_for(100ms);
    }
  }

  if (plan_.shidaixinan_hjzj) {
    const auto wakeup = shidaixinan_hjzj_wakeup_frame();
    bus_.send(wakeup);
    log(callbacks_,
        "在线探测：按完整成功ASC持续发送0x425全零标准CAN FD+BRS，"
        "周期10ms；稳定1秒后功能寻址发送10 03。");
    sender_ = std::jthread([this, wakeup](std::stop_token sender_stop) {
      ScopedHighResolutionTimer timer_resolution;
      auto next = std::chrono::steady_clock::now() +
                  kShidaixinanHjzjWakeupPeriod;
      std::size_t consecutive_failures{};
      while (!sender_stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
          try {
            bus_.send(wakeup);
            consecutive_failures = 0;
          } catch (const std::exception& error) {
            ++consecutive_failures;
            if (consecutive_failures >=
                kShidaixinanHjzjMaximumWakeFailures) {
              std::scoped_lock lock(error_mutex_);
              error_ = error.what();
              return;
            }
          } catch (...) {
            ++consecutive_failures;
            if (consecutive_failures >=
                kShidaixinanHjzjMaximumWakeFailures) {
              std::scoped_lock lock(error_mutex_);
              error_ = "unknown 0x425 transmit error";
              return;
            }
          }
          do {
            next += kShidaixinanHjzjWakeupPeriod;
          } while (next <= now);
        }
        std::this_thread::sleep_for(1ms);
      }
    });
    for (int elapsed = 0; elapsed < 100; ++elapsed) {
      check_stop(stop_);
      check();
      std::this_thread::sleep_for(10ms);
    }
  }

  if (plan_.xizhong) {
    const auto nm_frame =
        xizhong_nm_wakeup_frame_for_flow(plan_.request.profile.flow);
    if (!nm_frame) {
      throw std::runtime_error("Xizhong profile has no NM wakeup frame");
    }
    std::string last_nm_error;
    bool nm_transmitted{};
    for (std::size_t attempt = 1;
         attempt <= kXizhongNmMaxConsecutiveFailures; ++attempt) {
      check_stop(stop_);
      try {
        bus_.send(*nm_frame);
        nm_transmitted = true;
        if (attempt > 1) {
          log(callbacks_,
              "在线探测：首帧NM无ACK后继续唤醒，第" +
                  std::to_string(attempt) + "帧发送成功。");
        }
        break;
      } catch (const std::exception& error) {
        last_nm_error = error.what();
      } catch (...) {
        last_nm_error = "unknown CAN NM transmit error";
      }
      if (attempt < kXizhongNmMaxConsecutiveFailures) {
        log(callbacks_,
            "在线探测：NM第" + std::to_string(attempt) +
                "帧暂未获得ACK，200ms后继续唤醒。");
        std::this_thread::sleep_for(kXizhongNmPeriod);
      }
    }
    if (!nm_transmitted) {
      throw std::runtime_error(
          "Xizhong NM wakeup failed after " +
          std::to_string(kXizhongNmMaxConsecutiveFailures) +
          " attempts: " + last_nm_error);
    }
    log(callbacks_,
        "在线探测：按项目协议发送NM_ICG 0x" +
            to_hex(std::array<std::uint8_t, 4>{
                static_cast<std::uint8_t>(nm_frame->id >> 24U),
                static_cast<std::uint8_t>(nm_frame->id >> 16U),
                static_cast<std::uint8_t>(nm_frame->id >> 8U),
                static_cast<std::uint8_t>(nm_frame->id)}) +
            "全零扩展帧，"
            "每200ms保持网络唤醒1秒；在线结论仍只依据物理10 01响应。");
    sender_ = std::jthread([this, nm_frame](std::stop_token sender_stop) {
      auto next = std::chrono::steady_clock::now() + kXizhongNmPeriod;
      std::size_t consecutive_failures{};
      while (!sender_stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
          try {
            bus_.send(*nm_frame);
            if (consecutive_failures > 0) {
              log(callbacks_, "在线探测：NM短暂无ACK后已恢复持续发送。");
            }
            consecutive_failures = 0;
          } catch (const std::exception& error) {
            ++consecutive_failures;
            if (consecutive_failures >= kXizhongNmMaxConsecutiveFailures) {
              std::scoped_lock lock(error_mutex_);
              error_ = error.what();
              return;
            }
          } catch (...) {
            ++consecutive_failures;
            if (consecutive_failures >= kXizhongNmMaxConsecutiveFailures) {
              std::scoped_lock lock(error_mutex_);
              error_ = "unknown CAN transmit error";
              return;
            }
          }
          do {
            next += kXizhongNmPeriod;
          } while (next <= now);
        }
        std::this_thread::sleep_for(2ms);
      }
    });
    for (int elapsed = 0; elapsed < 50; ++elapsed) {
      check_stop(stop_);
      check();
      std::this_thread::sleep_for(20ms);
    }
  }
}

} // namespace uds::app::probe_detail
