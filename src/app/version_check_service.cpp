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
#include <iomanip>
#include <mutex>
#include <sstream>
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

std::string diagnostic_id(std::uint32_t id) {
  std::ostringstream text;
  text << "0x" << std::uppercase << std::hex << std::setfill('0')
       << std::setw(id > 0x7FFU ? 8 : 3) << id;
  return text.str();
}

std::string version_did(std::span<const std::uint8_t> request) {
  if (request.size() < 3 || request[0] != 0x22) return {};
  std::ostringstream text;
  text << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
       << static_cast<unsigned>(request[1]) << std::setw(2)
       << static_cast<unsigned>(request[2]);
  return text.str();
}

std::string target_display_name(const VersionCheckRequest& request) {
  const auto target = std::find_if(
      request.profile.targets.cbegin(), request.profile.targets.cend(),
      [&request](const FlashTargetProfile& candidate) {
        return candidate.id == request.target_id;
      });
  if (target != request.profile.targets.cend() &&
      !target->display_name.empty()) {
    return utf8(target->display_name);
  }
  return utf8(request.profile.device_name);
}

std::string elapsed_seconds(std::chrono::steady_clock::duration elapsed) {
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  std::ostringstream text;
  text << std::fixed << std::setprecision(1)
       << static_cast<double>(milliseconds) / 1000.0 << " s";
  return text.str();
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
  const auto operation_started = std::chrono::steady_clock::now();
  try {
    const auto plan =
        load_version_check_plan(request.profile_path, request.target_id);
    if (plan.items.empty()) {
      throw std::runtime_error("current profile has no version-check plan");
    }
    progress(callbacks, 0, "版本读取开始");
    log(callbacks, "项目：" + utf8(request.profile.vendor_name) + " / " +
                       utf8(request.profile.project_name) + " / " +
                       target_display_name(request));
    log(callbacks, "通道：CH" + std::to_string(request.channel) +
                       " | TX " + diagnostic_id(request.tx_id) + " | RX " +
                       diagnostic_id(request.rx_id));
    auto bus = bus_factory_(request);
    if (!bus) throw std::runtime_error("version-check bus factory returned null");
    if (!request.trace_file.empty()) {
      auto trace =
          std::make_shared<AscTraceWriter>(request.trace_file, request.channel);
      if (!trace->is_open()) {
        log(callbacks,
            "WARN：ASC日志创建失败，版本读取继续执行：" +
                utf8(request.trace_file.wstring()));
      }
      bus = std::make_unique<TracingCanBus>(std::move(bus), std::move(trace));
    }
    bus->open();

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
    }

    IsoTpConfig transport_config{
        request.tx_id, request.rx_id, request.profile.padding, 0,
        request.profile.isotp_st_min, 1000ms, 1000ms,
        request.profile.extended_id, request.profile.extended_id,
        request.profile.uds_fd, request.profile.uds_brs};
    transport_config.drain_receive_before_send =
        xizhong_supported_flow(request.profile.flow);
    IsoTpSession transport(*bus, transport_config);
    // Raw TX/RX is retained by the ASC trace and returned item data. The
    // execution log intentionally presents one concise decoded line per DID.
    UdsClient client(transport, [](const std::string&) {}, stop);

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
      log(callbacks,
          "进入诊断会话成功（50 " +
              to_hex(std::array<std::uint8_t, 1>{plan.session}) + "）");
    }

    bool required_ok = true;
    unsigned success_count{};
    unsigned failure_count{};
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
      std::string concise_result;
      const auto started = std::chrono::steady_clock::now();
      try {
        const auto response = client.request(item.request, 1000ms);
        item_result.response_hex = to_hex(response.response);
        if (!response.success) {
          item_result.status =
              item.required ? VersionCheckStatus::error
                            : VersionCheckStatus::warning;
          concise_result = response.detail.empty() ? "响应无效" : response.detail;
        } else if (response.response.size() < item.response_prefix.size() ||
                   !std::equal(item.response_prefix.begin(),
                               item.response_prefix.end(),
                               response.response.begin())) {
          item_result.status = VersionCheckStatus::error;
          concise_result = "响应格式不匹配";
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
            concise_result = "与期望值不一致，实际=" + utf8(item_result.actual);
          }
        }
      } catch (const std::exception& error) {
        if (stop.stop_requested()) throw;
        item_result.status =
            item.required ? VersionCheckStatus::error
                          : VersionCheckStatus::warning;
        item_result.response_hex = error.what();
        concise_result = error.what();
      }
      item_result.elapsed_ms = static_cast<unsigned>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
      if (item.required &&
          item_result.status != VersionCheckStatus::pass) {
        required_ok = false;
      }
      const auto did = version_did(item.request);
      const auto label = (did.empty() ? std::string{} : did + " ") +
                         utf8(item.name);
      if (item_result.status == VersionCheckStatus::pass) {
        ++success_count;
        log(callbacks, label + "：" +
                           (item_result.actual.empty()
                                ? std::string("未解析到版本值")
                                : utf8(item_result.actual)));
      } else {
        ++failure_count;
        log(callbacks, label + "：读取失败（" + concise_result + "）");
      }
      result.items.push_back(std::move(item_result));
      progress(callbacks,
               static_cast<int>(((index + 1) * 100) / plan.items.size()),
               "版本信息读取中");
    }
    result.success = required_ok;
    result.message = "版本读取完成：成功 " + std::to_string(success_count) +
                     "，失败 " + std::to_string(failure_count) + "，耗时 " +
                     elapsed_seconds(std::chrono::steady_clock::now() -
                                     operation_started);
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
