#include "app/version_check_service.hpp"
#include "app/version_value_decoder.hpp"

#include "core/hex.hpp"
#include "core/asc_trace.hpp"
#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "drivers/can/can_bus_provider.hpp"
#include "flash/xizhong_rsmr_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace uds::app {
namespace {
using namespace std::chrono_literals;

constexpr std::string_view kCancelled = "operation cancelled by user";

std::string utf8(std::wstring_view text) {
  const auto encoded = std::filesystem::path(std::wstring(text)).u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void check_stop(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error(kCancelled.data());
}

void log(const VersionCheckCallbacks& callbacks, const std::string& line) {
  if (callbacks.onLog) callbacks.onLog(line);
}

void progress(const VersionCheckCallbacks& callbacks, int percent,
              const std::string& line) {
  if (callbacks.onProgress) callbacks.onProgress(percent, line);
}

std::string status_text(VersionCheckStatus status) {
  switch (status) {
  case VersionCheckStatus::pass:
    return "PASS";
  case VersionCheckStatus::fail:
    return "FAIL";
  case VersionCheckStatus::warning:
    return "WARN";
  case VersionCheckStatus::info:
    return "INFO";
  case VersionCheckStatus::error:
  default:
    return "ERROR";
  }
}

} // namespace

VersionCheckService::VersionCheckService(BusFactory bus_factory)
    : bus_factory_(std::move(bus_factory)) {
  if (!bus_factory_) {
    bus_factory_ = [](const VersionCheckRequest& request) {
      return default_can_bus_provider()->create(
          {"", request.channel, request.profile.nominal_bitrate,
           request.profile.data_bitrate, request.profile.can_fd,
           L"UDSToolVersionCheck"});
    };
  }
}

VersionCheckResult VersionCheckService::run(
    const VersionCheckRequest& request,
    const VersionCheckCallbacks& callbacks, std::stop_token stop) const {
  VersionCheckResult result;
  try {
    const auto plan =
        load_version_check_plan(request.profile_path, request.target_id);
    if (plan.items.empty()) {
      throw std::runtime_error("current profile has no version-check plan");
    }
    progress(callbacks, 0, "版本读取开始");
    auto bus = bus_factory_(request);
    if (!bus) throw std::runtime_error("version-check bus factory returned null");
    if (!request.trace_file.empty()) {
      auto trace =
          std::make_shared<AscTraceWriter>(request.trace_file, request.channel);
      if (trace->is_open()) {
        log(callbacks, "ASC原始总线日志：" + utf8(request.trace_file.wstring()));
      } else {
        log(callbacks,
            "WARN：ASC日志创建失败，版本读取继续执行：" +
                utf8(request.trace_file.wstring()));
      }
      bus = std::make_unique<TracingCanBus>(std::move(bus), std::move(trace));
    }
    bus->open();
    log(callbacks, "PASS：版本读取CAN通道已打开");

    std::mutex precondition_error_mutex;
    std::string precondition_error;
    std::jthread precondition_sender;
    const auto check_precondition_sender = [&] {
      std::scoped_lock lock(precondition_error_mutex);
      if (!precondition_error.empty()) {
        throw std::runtime_error("version-read precondition failed: " +
                                 precondition_error);
      }
    };

    if (plan.precondition == L"ars131_0x400") {
      const CanFrame wakeup{
          0x400, {0, 0, 0, 0, 0, 0, 0, 0}, false, false, false};
      for (int count = 0; count < 10; ++count) {
        check_stop(stop);
        bus->send(wakeup);
        std::this_thread::sleep_for(100ms);
      }
      log(callbacks, "版本读取：已发送ARS1.31 0x400前置报文");
    } else if (plan.precondition == L"chuneng_520") {
      const CanFrame wakeup{
          0x520, {0, 0, 0, 0, 0, 0, 0, 0}, false, false, false};
      bus->send(wakeup);
      precondition_sender = std::jthread(
          [&bus, wakeup, &precondition_error_mutex,
           &precondition_error](std::stop_token sender_stop) {
            auto next = std::chrono::steady_clock::now() + 500ms;
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
                  next += 500ms;
                } while (next <= now);
              }
              std::this_thread::sleep_for(10ms);
            }
          });
      for (int elapsed = 0; elapsed < 100; ++elapsed) {
        check_stop(stop);
        check_precondition_sender();
        std::this_thread::sleep_for(10ms);
      }
      log(callbacks,
          "版本读取：楚能ARC331 0x520周期唤醒已启动（500 ms）");
    } else if (plan.precondition == L"xizhong_nm") {
      const auto nm_frame = xizhong_nm_wakeup_frame_for_flow(request.profile.flow);
      if (!nm_frame) {
        throw std::runtime_error(
            "xizhong_nm precondition requires an Xizhong profile flow");
      }
      bool initial_nm_sent{};
      std::string initial_nm_error;
      for (unsigned attempt = 1;
           attempt <= kXizhongNmMaxConsecutiveFailures; ++attempt) {
        check_stop(stop);
        try {
          bus->send(*nm_frame);
          initial_nm_sent = true;
          if (attempt > 1) {
            log(callbacks,
                "版本读取：首帧NM无ACK后继续唤醒，第" +
                    std::to_string(attempt) + "帧发送成功。");
          }
          break;
        } catch (const std::exception& error) {
          initial_nm_error = error.what();
          if (attempt == 1) {
            log(callbacks,
                "版本读取：NM第1帧暂未获得ACK，200ms后继续唤醒。");
          }
          if (attempt < kXizhongNmMaxConsecutiveFailures) {
            std::this_thread::sleep_for(200ms);
          }
        }
      }
      if (!initial_nm_sent) {
        throw std::runtime_error(
            "版本读取：连续NM唤醒均发送失败：" + initial_nm_error);
      }
      precondition_sender = std::jthread(
          [&bus, nm_frame, &precondition_error_mutex,
           &precondition_error](std::stop_token sender_stop) {
            auto next = std::chrono::steady_clock::now() + 200ms;
            unsigned consecutive_failures{};
            while (!sender_stop.stop_requested()) {
              const auto now = std::chrono::steady_clock::now();
              if (now >= next) {
                try {
              bus->send(*nm_frame);
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
      log(callbacks, "版本读取：犀重网络唤醒完成");
    }

    IsoTpConfig transport_config{
        request.tx_id, request.rx_id, request.profile.padding, 0,
        request.profile.isotp_st_min, 1000ms, 1000ms,
        request.profile.extended_id, request.profile.extended_id,
        request.profile.uds_fd, request.profile.uds_brs};
    transport_config.drain_receive_before_send =
        xizhong_supported_flow(request.profile.flow);
    IsoTpSession transport(*bus, transport_config);
    UdsClient client(transport, [&](const std::string& line) {
      log(callbacks, line);
    }, stop);

    if (plan.session != 0) {
      check_precondition_sender();
      const std::array<std::uint8_t, 2> session{0x10, plan.session};
      UdsResponse response;
      try {
        response = client.request(session, 1000ms);
      } catch (const std::exception& error) {
        throw std::runtime_error(
            "诊断会话请求=" + to_hex(session) + " 失败；原因=" +
            error.what());
      }
      if (!response.success || response.response.size() < 2 ||
          response.response[0] != 0x50 ||
          response.response[1] != plan.session) {
        throw std::runtime_error(
            "诊断会话 10 " + to_hex(std::array<std::uint8_t, 1>{plan.session}) +
            " 失败；响应=" + to_hex(response.response) +
            "；原因=" + response.detail);
      }
    }

    bool required_ok = true;
    result.items.reserve(plan.items.size());
    for (std::size_t index = 0; index < plan.items.size(); ++index) {
      check_stop(stop);
      check_precondition_sender();
      const auto& item = plan.items[index];
      VersionCheckItemResult item_result;
      item_result.name = item.name;
      item_result.expected = item.expected;
      item_result.request_hex = to_hex(item.request);
      item_result.required = item.required;
      const auto started = std::chrono::steady_clock::now();
      try {
        const auto response = client.request(item.request, 1000ms);
        item_result.response_hex = to_hex(response.response);
        if (!response.success) {
          item_result.status =
              item.required ? VersionCheckStatus::error
                            : VersionCheckStatus::warning;
        } else if (response.response.size() < item.response_prefix.size() ||
                   !std::equal(item.response_prefix.begin(),
                               item.response_prefix.end(),
                               response.response.begin())) {
          item_result.status = VersionCheckStatus::error;
        } else {
          item_result.actual = decode_version_value(
              std::span<const std::uint8_t>(response.response)
                  .subspan(item.response_prefix.size()),
              item.decoder);
          if (item.expected.empty()) {
            // A read-only item succeeds when a valid response is decoded.
            // Optional expected values remain available for projects that
            // explicitly need comparison, without coupling that policy to UI.
            item_result.status = VersionCheckStatus::pass;
          } else if (item_result.actual == item.expected) {
            item_result.status = VersionCheckStatus::pass;
          } else {
            item_result.status = VersionCheckStatus::fail;
          }
        }
      } catch (const std::exception& error) {
        if (stop.stop_requested()) throw;
        item_result.status =
            item.required ? VersionCheckStatus::error
                          : VersionCheckStatus::warning;
        item_result.response_hex = error.what();
      }
      item_result.elapsed_ms = static_cast<unsigned>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
      if (item.required &&
          item_result.status != VersionCheckStatus::pass) {
        required_ok = false;
      }
      log(callbacks, status_text(item_result.status) + "：" +
                         utf8(item.name) + "；请求=" +
                         item_result.request_hex + "；响应/原因=" +
                         item_result.response_hex);
      result.items.push_back(std::move(item_result));
      progress(callbacks,
               static_cast<int>(((index + 1) * 100) / plan.items.size()),
               "版本信息读取中");
    }
    result.success = required_ok;
    result.message =
        required_ok ? "读取完成：全部必读版本信息读取成功"
                    : "读取失败：存在版本读取错误";
  } catch (const std::exception& error) {
    result.cancelled =
        stop.stop_requested() || error.what() == kCancelled;
    result.message =
        result.cancelled
            ? "版本读取已停止"
            : "ERROR：版本读取失败：" + std::string(error.what());
  }
  return result;
}

} // namespace uds::app
