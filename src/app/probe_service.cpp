#include "app/probe_service.hpp"

#include "core/chuneng_arc331_protocol.hpp"

#include "core/canoe_power.hpp"
#include "core/asc_trace.hpp"
#include "core/high_resolution_timer.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "drivers/can/can_bus_provider.hpp"
#include "flash/chery_ars1_33_flow.hpp"
#include "flash/chuneng_331_protocol.hpp"
#include "flash/geely_p416_flow.hpp"
#include "flash/longma_ars1_31_flow.hpp"
#include "flash/shidaixinan_hjzj_fmr_flow.hpp"
#include "flash/xizhong_rsmr_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uds::app {
namespace {
using namespace std::chrono_literals;

constexpr std::string_view kCancelled = "operation cancelled by user";

std::string utf8(std::wstring_view text) {
  const auto encoded = std::filesystem::path(std::wstring(text)).u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void log(const ProbeServiceCallbacks& callbacks, const std::string& line) {
  if (callbacks.onLog) callbacks.onLog(line);
}

void progress(const ProbeServiceCallbacks& callbacks, int value,
              const std::string& line) {
  if (callbacks.onProgress) callbacks.onProgress(value, line);
}

void check_stop(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error(kCancelled.data());
}

std::string concise_probe_failure(std::string_view detail) {
  if (detail.find("timeout") != std::string_view::npos ||
      detail.find("response wait failed") != std::string_view::npos ||
      detail.find("no valid UDS response") != std::string_view::npos) {
    return "未收到设备响应";
  }
  if (detail.find("unexpected") != std::string_view::npos ||
      detail.find("UDS request failed") != std::string_view::npos ||
      detail.find("NRC") != std::string_view::npos) {
    return "设备响应无效";
  }
  return "在线探测失败";
}

std::pair<std::uint32_t, std::uint32_t> resolve_ft_endpoint(
    const ProbeRequest& request) {
  if (!request.profile.supports_ft_entry) {
    throw std::runtime_error("selected profile does not support FT probing");
  }
  const auto target = std::find_if(
      request.profile.targets.cbegin(), request.profile.targets.cend(),
      [&request](const FlashTargetProfile& candidate) {
        return candidate.tx_id == request.tx_id &&
               candidate.rx_id == request.rx_id;
      });
  const auto tx_id =
      target != request.profile.targets.cend() && target->ft_tx_id != 0
          ? target->ft_tx_id
          : request.profile.ft_tx_id;
  const auto rx_id =
      target != request.profile.targets.cend() && target->ft_rx_id != 0
          ? target->ft_rx_id
          : request.profile.ft_rx_id;
  if (tx_id == 0 || rx_id == 0) {
    throw std::runtime_error("selected target FT endpoint is not configured");
  }
  return {tx_id, rx_id};
}

} // namespace

ProbeService::ProbeService(BusFactory bus_factory)
    : bus_factory_(std::move(bus_factory)) {
  if (!bus_factory_) {
    bus_factory_ = [](const ProbeRequest& request) {
      return default_can_bus_provider()->create(
          {"", request.channel, request.nominal_bitrate,
           request.data_bitrate, request.profile.can_fd, L"UDSToolCpp"});
    };
  }
}

ProbeResult ProbeService::run(const ProbeRequest& requested,
                              const ProbeServiceCallbacks& callbacks,
                              std::stop_token stop) const {
  ProbeResult result;
  auto request = requested;
  const bool ft_probe = request.entry_mode == L"ft";
  const bool boot_probe = request.entry_mode == L"boot";
  if (!request.entry_mode.empty() && request.entry_mode != L"app" &&
      request.entry_mode != L"auto" && !boot_probe && !ft_probe) {
    result.message = "在线探测配置无效";
    return result;
  }
  const auto app_tx_id = request.tx_id;
  const auto app_rx_id = request.rx_id;
  if (ft_probe) {
    try {
      std::tie(request.tx_id, request.rx_id) = resolve_ft_endpoint(request);
    } catch (const std::exception&) {
      result.message = "FT探测端点未配置";
      return result;
    }
  }
  const bool power_managed = request.profile.power_control;
  const bool chuneng_331 = request.profile.flow == L"chuneng_331";
  const bool chuneng_arc331 = request.profile.flow == L"chuneng_arc331";
  const bool chuneng_wakeup_probe = chuneng_331 || chuneng_arc331;
  const bool shidaixinan_hjzj =
      request.profile.flow == L"shidaixinan_hjzj_fmr";
  const bool lp_arc = request.profile.flow == L"lp_arc";
  const bool lp_arf = request.profile.flow == L"lp_arf";
  const bool lingpao_radar = lp_arc || lp_arf;
  const bool geely_p416 = request.profile.flow == L"geely_p416";
  const bool xizhong = xizhong_supported_flow(request.profile.flow);
  const bool ars131_app =
      request.profile.flow == L"longma_ars1_31" ||
      request.profile.flow == L"changan_c857" ||
      request.profile.flow == L"lingyao_b216";
  const bool secondary_target =
      ars131_app && longma_ars131_secondary_endpoint(app_tx_id, app_rx_id);
  const auto probe_tx_id =
      lingpao_radar
          ? request.profile.functional_id
          : shidaixinan_hjzj && !ft_probe
          ? request.profile.functional_id
          : request.tx_id;
  const bool expected_profile_ids =
      ft_probe ||
      (request.tx_id == request.profile.tx_id &&
       request.rx_id == request.profile.rx_id) ||
      std::any_of(request.profile.targets.cbegin(), request.profile.targets.cend(),
                  [&request](const FlashTargetProfile& target) {
                    return target.tx_id == request.tx_id &&
                           target.rx_id == request.rx_id;
                  });
  try {
    progress(callbacks, 0, "在线探测开始");
    if (power_managed) {
      log(callbacks,
          "在线探测：先通过 CANoe 写 IO::VN1600_1::DOUT=1 上电");
      const auto power = set_canoe_dout(1);
      log(callbacks, "PASS：已上电，IO::VN1600_1::DOUT=" +
                         std::to_string(power.value));
      if (!power.configuration.empty())
        log(callbacks, "CANoe工程：" + utf8(power.configuration));
      log(callbacks, "在线探测：等待目标 ECU 上电稳定 1 秒……");
      for (int elapsed = 0; elapsed < 20; ++elapsed) {
        check_stop(stop);
        std::this_thread::sleep_for(50ms);
      }
    } else {
      log(callbacks,
          "在线探测：该项目不使用 CANoe DOUT，保持台架现有外部供电状态。");
    }
    log(callbacks, "在线探测：供电状态准备完成；进度保持0%，直到收到有效诊断响应。");

    std::ostringstream start;
    start << "探测项目“" << utf8(request.profile.name) << "”：物理CH"
          << request.channel << "，"
          << ((shidaixinan_hjzj || lingpao_radar)
                  ? "功能寻址 0x"
                  : "物理寻址 0x")
          << std::hex << probe_tx_id << " -> 0x" << request.rx_id;
    if (ars131_app) {
      start << (secondary_target ? "（从雷达）" : "（主雷达）");
    }

    start << (ft_probe ? "，FT入口" : "，APP入口");
    log(callbacks, start.str());
    check_stop(stop);

    auto bus = bus_factory_(request);
    if (!bus) throw std::runtime_error("probe bus factory returned null");
    if (!request.trace_file.empty()) {
      auto trace =
          std::make_shared<AscTraceWriter>(request.trace_file, request.channel);
      if (trace->is_open()) {
        log(callbacks, "ASC原始总线日志：" + utf8(request.trace_file.wstring()));
      } else {
        log(callbacks,
            "WARN：ASC日志创建失败，在线探测继续执行：" +
                utf8(request.trace_file.wstring()));
      }
      bus = std::make_unique<TracingCanBus>(std::move(bus), std::move(trace));
    }
    bus->open();
    log(callbacks, "PASS：CAN硬件物理CH" + std::to_string(request.channel) +
                       " 已打开（后端：" +
                       std::string(can_vendor_name(default_can_vendor())) +
                       "）");
    log(callbacks, "在线探测：CAN通道已打开；尚不能据此判定ECU在线。");

    std::mutex precondition_error_mutex;
    std::string precondition_error;
    std::jthread precondition_sender;
    const auto check_precondition_sender = [&] {
      std::scoped_lock lock(precondition_error_mutex);
      if (!precondition_error.empty()) {
        throw std::runtime_error("probe precondition sender failed: " +
                                 precondition_error);
      }
    };

    if (chuneng_wakeup_probe && !ft_probe) {
      const auto wakeup_period =
          chuneng_arc331 ? kChunengArc331WakeupPeriod : 500ms;
      const CanFrame wakeup{
          kChunengArc331WakeupId,
          {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          false, false, false};
      bus->send(wakeup);
      log(callbacks,
          "在线探测：楚能331持续发送0x520全零标准CAN唤醒报文，周期" +
              std::to_string(wakeup_period.count()) +
              "ms；保持到在线检查结束。");
      precondition_sender = std::jthread(
          [&bus, wakeup, &precondition_error_mutex,
           &precondition_error,
           wakeup_period](std::stop_token sender_stop) {
            ScopedHighResolutionTimer timer_resolution;
            auto next = std::chrono::steady_clock::now() + wakeup_period;
            while (!sender_stop.stop_requested()) {
              const auto now = std::chrono::steady_clock::now();
              if (now >= next) {
                try {
                  bus->send(wakeup);
                } catch (const std::exception& error) {
                  std::scoped_lock lock(precondition_error_mutex);
                  precondition_error = error.what();
                  return;
                } catch (...) {
                  std::scoped_lock lock(precondition_error_mutex);
                  precondition_error = "unknown 0x520 transmit error";
                  return;
                }
                do {
                  next += wakeup_period;
                } while (next <= now);
              }
              std::this_thread::sleep_for(2ms);
            }
          });
      // Let the first wake-up frame take effect before entering the extended
      // session. The sender remains active throughout both probe requests.
      for (int elapsed = 0; elapsed < 25; ++elapsed) {
        check_stop(stop);
        check_precondition_sender();
        std::this_thread::sleep_for(20ms);
      }
    }

    if (geely_p416) {
      const auto wakeup = geely_p416_nm_wakeup_frame();
      bus->send(wakeup);
      log(callbacks,
          "在线探测：按成功BLF持续发送0x53F "
          "3F FF FF FF FF FF FF FF标准CAN帧，周期200ms；"
          "稳定1秒后只发送安全的10 01，不进入编程会话。");
      precondition_sender = std::jthread(
          [&bus, wakeup, &precondition_error_mutex,
           &precondition_error](std::stop_token sender_stop) {
            auto next = std::chrono::steady_clock::now() + 200ms;
            while (!sender_stop.stop_requested()) {
              const auto now = std::chrono::steady_clock::now();
              if (now >= next) {
                try {
                  bus->send(wakeup);
                } catch (const std::exception& error) {
                  std::scoped_lock lock(precondition_error_mutex);
                  precondition_error = error.what();
                  return;
                } catch (...) {
                  std::scoped_lock lock(precondition_error_mutex);
                  precondition_error = "unknown 0x53F transmit error";
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
        check_stop(stop);
        check_precondition_sender();
        std::this_thread::sleep_for(20ms);
      }
    }

    if (request.profile.flow == L"chery_ars1_33") {
      const auto frames = chery_ars133_precondition_frames();
      for (const auto& item : frames) bus->send(item.frame);
      log(callbacks,
          "在线探测：发送 0x600 全零唤醒报文(100ms)、"
          "0x25B PowerMode=ON(20ms) 和 0x4B4 Gear=P(100ms)，"
          "稳定1秒后再发送UDS请求。");
      precondition_sender = std::jthread(
          [&bus, frames, &precondition_error_mutex,
           &precondition_error](std::stop_token sender_stop) {
            auto now = std::chrono::steady_clock::now();
            std::array<std::chrono::steady_clock::time_point, 3> next{
                now + frames[0].period, now + frames[1].period,
                now + frames[2].period};
            while (!sender_stop.stop_requested()) {
              now = std::chrono::steady_clock::now();
              for (std::size_t index = 0; index < frames.size(); ++index) {
                if (now < next[index]) continue;
                try {
                  bus->send(frames[index].frame);
                } catch (const std::exception& error) {
                  std::scoped_lock lock(precondition_error_mutex);
                  precondition_error = error.what();
                  return;
                } catch (...) {
                  std::scoped_lock lock(precondition_error_mutex);
                  precondition_error = "unknown CAN transmit error";
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
        check_stop(stop);
        check_precondition_sender();
        std::this_thread::sleep_for(20ms);
      }
    }

    if (ars131_app) {
      const CanFrame precondition{
          0x400, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          false, false, false};
      log(callbacks,
          "在线探测：按成功基线先发送 0x400 全零前置报文；在线结论仍只依据 UDS 响应。");
      for (int count = 0; count < 10; ++count) {
        check_stop(stop);
        bus->send(precondition);
        std::this_thread::sleep_for(100ms);
      }
    }

    if (shidaixinan_hjzj) {
      const auto wakeup = shidaixinan_hjzj_wakeup_frame();
      bus->send(wakeup);
      log(callbacks,
          "在线探测：按完整成功ASC持续发送0x425全零标准CAN FD+BRS，"
          "周期10ms；稳定1秒后功能寻址发送10 03。");
      precondition_sender = std::jthread(
          [&bus, wakeup, &precondition_error_mutex,
           &precondition_error](std::stop_token sender_stop) {
            ScopedHighResolutionTimer timer_resolution;
            auto next = std::chrono::steady_clock::now() +
                        kShidaixinanHjzjWakeupPeriod;
            std::size_t consecutive_failures{};
            while (!sender_stop.stop_requested()) {
              const auto now = std::chrono::steady_clock::now();
              if (now >= next) {
                try {
                  bus->send(wakeup);
                  consecutive_failures = 0;
                } catch (const std::exception& error) {
                  ++consecutive_failures;
                  if (consecutive_failures >=
                      kShidaixinanHjzjMaximumWakeFailures) {
                    std::scoped_lock lock(
                        precondition_error_mutex);
                    precondition_error = error.what();
                    return;
                  }
                } catch (...) {
                  ++consecutive_failures;
                  if (consecutive_failures >=
                      kShidaixinanHjzjMaximumWakeFailures) {
                    std::scoped_lock lock(
                        precondition_error_mutex);
                    precondition_error =
                        "unknown 0x425 transmit error";
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
        check_stop(stop);
        check_precondition_sender();
        std::this_thread::sleep_for(10ms);
      }
    }

    if (xizhong) {
      const auto nm_frame = xizhong_nm_wakeup_frame_for_flow(request.profile.flow);
      if (!nm_frame) {
        throw std::runtime_error("Xizhong profile has no NM wakeup frame");
      }
      std::string last_nm_error;
      bool nm_transmitted{};
      for (std::size_t attempt = 1;
           attempt <= kXizhongNmMaxConsecutiveFailures; ++attempt) {
        check_stop(stop);
        try {
          bus->send(*nm_frame);
          nm_transmitted = true;
          if (attempt > 1) {
            log(callbacks,
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
          log(callbacks,
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
      log(callbacks,
          "在线探测：按项目协议发送NM_ICG 0x" +
              to_hex(std::array<std::uint8_t, 4>{
                  static_cast<std::uint8_t>(nm_frame->id >> 24U),
                  static_cast<std::uint8_t>(nm_frame->id >> 16U),
                  static_cast<std::uint8_t>(nm_frame->id >> 8U),
                  static_cast<std::uint8_t>(nm_frame->id)}) +
              "全零扩展帧，"
          "每200ms保持网络唤醒1秒；在线结论仍只依据物理10 01响应。");
      precondition_sender = std::jthread(
          [&bus, nm_frame, &callbacks, &precondition_error_mutex,
           &precondition_error](std::stop_token sender_stop) {
            auto next = std::chrono::steady_clock::now() + kXizhongNmPeriod;
            std::size_t consecutive_failures{};
            while (!sender_stop.stop_requested()) {
              const auto now = std::chrono::steady_clock::now();
              if (now >= next) {
                try {
                  bus->send(*nm_frame);
                  if (consecutive_failures > 0) {
                    log(callbacks,
                        "在线探测：NM短暂无ACK后已恢复持续发送。");
                  }
                  consecutive_failures = 0;
                } catch (const std::exception& error) {
                  ++consecutive_failures;
                  if (consecutive_failures >=
                      kXizhongNmMaxConsecutiveFailures) {
                    std::scoped_lock lock(precondition_error_mutex);
                    precondition_error = error.what();
                    return;
                  }
                } catch (...) {
                  ++consecutive_failures;
                  if (consecutive_failures >=
                      kXizhongNmMaxConsecutiveFailures) {
                    std::scoped_lock lock(precondition_error_mutex);
                    precondition_error = "unknown CAN transmit error";
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
        check_stop(stop);
        check_precondition_sender();
        std::this_thread::sleep_for(20ms);
      }
    }

    IsoTpConfig probe_config{
        probe_tx_id, request.rx_id,
        ft_probe ? request.profile.ft_padding : request.padding, 0,
        request.profile.isotp_st_min, 1000ms, 1000ms,
        ft_probe ? request.profile.ft_extended_id
                 : request.profile.extended_id,
        ft_probe ? request.profile.ft_extended_id
                 : request.profile.extended_id,
        ft_probe ? request.profile.ft_uds_fd : request.profile.uds_fd,
        ft_probe ? request.profile.ft_uds_brs : request.profile.uds_brs};
    probe_config.drain_receive_before_send =
        !ft_probe && xizhong;
    IsoTpSession transport(*bus, probe_config);
    UdsClient client(transport, [&](const std::string& line) {
      log(callbacks, line);
    }, stop);
    check_stop(stop);
    const auto session = static_cast<std::uint8_t>(
        (lingpao_radar || geely_p416)
            ? 0x01
            : (ft_probe || shidaixinan_hjzj || chuneng_wakeup_probe ? 0x03
                                                                    : 0x01));
    std::string probe_description;
    if (geely_p416) {
      probe_description =
          ft_probe
              ? "在线探测：吉利P416 PLS入口向0x701/0x761发送10 01；收到50 01即判定在线，不执行10 02或刷写。"
              : "在线探测：吉利P416 APP入口向0x716/0x616发送10 01；收到50 01即判定在线，不执行10 02或刷写。";
    } else if (lingpao_radar) {
      probe_description =
          ft_probe
              ? "在线探测：" + utf8(request.profile.name) +
                    " PLS→APP 先向0x7DF/0x761发送10 01；收到50 01后再发送10 03，不执行10 02或刷写。"
              : "在线探测：" + utf8(request.profile.name) +
                    " APP→APP 先向功能ID/APP响应ID发送10 01；收到50 01后再发送10 03，不执行10 02或刷写。";
    } else if (chuneng_arc331 && !ft_probe) {
      probe_description =
          "在线探测：楚能ARC331持续发送0x520唤醒，向所选雷达物理端点发送10 03；收到对应响应ID的50 03即判定在线，不进入编程会话或刷写。";
    } else if (chuneng_331 && !ft_probe) {
      probe_description =
          boot_probe
              ? "在线探测：楚能331 BOOT→APP入口按正式规范向所选物理端点发送10 03；收到50 03即确认诊断在线，不发送仅APP入口适用的31 01 02 03，也不进入编程会话或刷写。"
              : "在线探测：楚能331 APP入口按正式规范向所选物理端点发送10 03；收到50 03后继续检查31 01 02 03，不进入编程会话或刷写。";
    } else if (ft_probe) {
      probe_description =
          "在线探测：向FT端点发送扩展会话请求10 03；收到并核验50 03后判定在线，不执行10 02或刷写。";
    } else if (shidaixinan_hjzj) {
      probe_description =
          "在线探测：向0x7DF发送功能寻址扩展会话10 03；收到0x7AC的50 03后判定时代新安FMR在线。";
    } else {
      probe_description =
          "在线探测：发送物理默认会话请求10 01；进度保持0%，收到并核验50 01后置100%。";
    }
    log(callbacks, probe_description);
    std::optional<UdsResponse> response;
    bool chuneng_functional_fallback = false;
    std::string last_request_error;
    const auto attempt_count =
        !ft_probe &&
                (xizhong ||
                 shidaixinan_hjzj)
            ? 3
            : 1;
    for (int attempt = 1; attempt <= attempt_count; ++attempt) {
      check_stop(stop);
      if (attempt_count > 1) {
        log(callbacks,
            (shidaixinan_hjzj
                 ? "时代新安功能10 03探测：第"
                 : "犀重物理10 01探测：第") +
                std::to_string(attempt) + "/" +
                std::to_string(attempt_count) + "次");
      }
      try {
        auto candidate = client.request(
            std::array<std::uint8_t, 2>{0x10, session}, 1000ms);
        if (candidate.success && candidate.response.size() >= 2 &&
            candidate.response[0] == 0x50 &&
            candidate.response[1] == session) {
          response = std::move(candidate);
          break;
        }
        std::ostringstream detail;
        if (!candidate.success) {
          detail << "UDS request failed";
          if (candidate.nrc != 0) {
            detail << ", NRC=0x" << std::hex << std::uppercase
                   << static_cast<int>(candidate.nrc);
          }
          if (!candidate.detail.empty()) detail << ", " << candidate.detail;
        } else {
          detail << "unexpected "
                 << (ft_probe ? "FT ExtendedSession" : "DefaultSession")
                 << " response: "
                 << to_hex(candidate.response);
        }
        last_request_error = detail.str();
      } catch (const std::exception& error) {
        last_request_error = error.what();
      }
      if (attempt < attempt_count) {
        for (int elapsed = 0; elapsed < 10; ++elapsed) {
          check_stop(stop);
          check_precondition_sender();
          std::this_thread::sleep_for(20ms);
        }
      }
    }
    if (!response && chuneng_331 && !ft_probe) {
      log(callbacks,
          "楚能331物理10 03无响应；按已验证的Boot兼容入口尝试功能寻址"
          "10 01→10 03，仍只接收当前配置Rx ID的响应。");
      try {
        IsoTpConfig functional_config = probe_config;
        functional_config.tx_id = request.profile.functional_id;
        IsoTpSession functional_transport(*bus, functional_config);
        UdsClient functional_client(functional_transport,
                                    [&](const std::string& line) {
                                      log(callbacks, line);
                                    },
                                    stop);
        const auto default_session = functional_client.request(
            std::array<std::uint8_t, 2>{0x10, 0x01}, 1000ms);
        if (!default_session.success || default_session.response.size() < 2 ||
            default_session.response[0] != 0x50 ||
            default_session.response[1] != 0x01) {
          throw std::runtime_error(
              "functional 10 01 did not receive expected 50 01");
        }
        auto extended_session = functional_client.request(
            std::array<std::uint8_t, 2>{0x10, 0x03}, 1000ms);
        if (!extended_session.success || extended_session.response.size() < 2 ||
            extended_session.response[0] != 0x50 ||
            extended_session.response[1] != 0x03) {
          throw std::runtime_error(
              "functional 10 03 did not receive expected 50 03");
        }
        response = std::move(extended_session);
        chuneng_functional_fallback = true;
        log(callbacks,
            "PASS：楚能331功能寻址兼容入口收到当前Rx ID的50 01/50 03；"
            "目标诊断在线。");
      } catch (const std::exception& error) {
        last_request_error =
            "physical 10 03 and functional 10 01/10 03 both failed: " +
            std::string(error.what());
      }
    }
    if (precondition_sender.joinable() && !chuneng_331) {
      precondition_sender.request_stop();
      precondition_sender.join();
    }
    check_precondition_sender();
    check_stop(stop);
    if (!response) {
      throw std::runtime_error(
          last_request_error.empty() ? "UDS request failed"
                                     : last_request_error);
    }

    std::string response_summary = to_hex(response->response);
    if (chuneng_functional_fallback) {
      response_summary += "（功能寻址Boot兼容入口）";
    }
    if (chuneng_arc331 && !ft_probe) {
      log(callbacks,
          "PASS：楚能ARC331所选物理诊断响应ID已返回50 03；目标在线。"
          "本按钮不发送31 01 02 03、10 02或任何刷写数据。");
    } else if (chuneng_331 && !ft_probe && !boot_probe) {
      log(callbacks,
          "楚能331 APP入口：物理10 03已响应；发送31 01 02 03检查刷新条件。"
          "仅状态0x04判定可刷写。");
      const auto precondition = client.request(
          kChuneng331ProgrammingPrecondition, 1000ms);
      check_stop(stop);
      const auto expected = chuneng_331_routine_success_prefix(0x0203);
      if (!precondition.success ||
          precondition.response.size() < expected.size() ||
          !std::equal(expected.begin(), expected.end(),
                      precondition.response.begin())) {
        throw std::runtime_error(
            "Chuneng programming precondition failed: " +
            to_hex(precondition.response));
      }
      response_summary +=
          "；ProgrammingPrecondition=" + to_hex(precondition.response);
    } else if (chuneng_331 && boot_probe) {
      log(callbacks,
          "楚能331 BOOT→APP：10 03已响应；仅Boot入口不发送31 01 02 03。"
          "该结果只证明诊断在线，完整可刷性由正式10 02及后续流程确认。");
    }
    if (precondition_sender.joinable()) {
      precondition_sender.request_stop();
      precondition_sender.join();
    }
    check_precondition_sender();
    if (lingpao_radar) {
      log(callbacks,
          utf8(request.profile.name) +
              "：默认会话已响应；按 CANoe Download() 等待2秒后发送功能寻址扩展会话。");
      for (int elapsed = 0; elapsed < 40; ++elapsed) {
        check_stop(stop);
        std::this_thread::sleep_for(50ms);
      }
      log(callbacks, "在线探测：发送CANoe功能寻址扩展会话请求10 03。");
      const auto extended = client.request(
          std::array<std::uint8_t, 2>{0x10, 0x03}, 800ms);
      check_stop(stop);
      if (!extended.success) {
        throw std::runtime_error("UDS ExtendedSession request failed");
      }
      if (extended.response.size() < 2 || extended.response[0] != 0x50 ||
          extended.response[1] != 0x03) {
        throw std::runtime_error("unexpected ExtendedSession response: " +
                                 to_hex(extended.response));
      }
      response_summary += "；ExtendedSession=" + to_hex(extended.response);
    }

    result.success = true;
    result.message = expected_profile_ids ? "设备在线：响应 "
                                          : "自定义端点在线：响应 ";
    result.message += response_summary;
    progress(callbacks, 100, "在线探测完成：已收到并核验物理诊断响应");
  } catch (const std::exception& error) {
    result.cancelled = stop.stop_requested() || error.what() == kCancelled;
    if (!result.cancelled) {
      log(callbacks, "ERROR：在线探测异常：" + std::string(error.what()));
    }
    result.message =
        result.cancelled ? "在线探测已停止"
                         : concise_probe_failure(error.what());
  } catch (...) {
    result.cancelled = stop.stop_requested();
    if (!result.cancelled) {
      log(callbacks, "ERROR：在线探测异常：unknown exception");
    }
    result.message =
        result.cancelled ? "在线探测已停止" : "在线探测失败";
  }
  return result;
}

} // namespace uds::app
