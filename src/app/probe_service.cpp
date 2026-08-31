#include "app/probe_service.hpp"

#include "app/probe_plan.hpp"
#include "app/probe_preconditions.hpp"
#include "app/probe_session.hpp"
#include "app/probe_support.hpp"
#include "core/asc_trace.hpp"
#include "core/canoe_power.hpp"
#include "drivers/can/can_bus_provider.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uds::app {
namespace {
using namespace std::chrono_literals;
using namespace probe_detail;
}

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
  auto resolution = resolve_probe_plan(requested);
  if (!resolution.valid) {
    result.message = std::move(resolution.message);
    return result;
  }
  const auto& plan = resolution.plan;

  try {
    progress(callbacks, 0, "在线探测开始");
    if (plan.power_managed) {
      log(callbacks,
          "在线探测：先通过 CANoe 写 IO::VN1600_1::DOUT=1 上电");
      const auto power = set_canoe_dout(1);
      log(callbacks, "PASS：已上电，IO::VN1600_1::DOUT=" +
                         std::to_string(power.value));
      if (!power.configuration.empty()) {
        log(callbacks, "CANoe工程：" + utf8(power.configuration));
      }
      log(callbacks, "在线探测：等待目标 ECU 上电稳定 1 秒……");
      for (int elapsed = 0; elapsed < 20; ++elapsed) {
        check_stop(stop);
        std::this_thread::sleep_for(50ms);
      }
    } else {
      log(callbacks,
          "在线探测：该项目不使用 CANoe DOUT，保持台架现有外部供电状态。");
    }
    log(callbacks,
        "在线探测：供电状态准备完成；进度保持0%，直到收到有效诊断响应。");
    log(callbacks, probe_start_description(plan));
    check_stop(stop);

    auto bus = bus_factory_(plan.request);
    if (!bus) throw std::runtime_error("probe bus factory returned null");
    if (!plan.request.trace_file.empty()) {
      auto trace = std::make_shared<AscTraceWriter>(
          plan.request.trace_file, plan.request.channel);
      if (trace->is_open()) {
        log(callbacks,
            "ASC原始总线日志：" +
                utf8(plan.request.trace_file.wstring()));
      } else {
        log(callbacks,
            "WARN：ASC日志创建失败，在线探测继续执行：" +
                utf8(plan.request.trace_file.wstring()));
      }
      bus = std::make_unique<TracingCanBus>(std::move(bus), std::move(trace));
    }
    bus->open();
    log(callbacks,
        "PASS：CAN硬件物理CH" + std::to_string(plan.request.channel) +
            " 已打开（后端：" +
            std::string(can_vendor_name(default_can_vendor())) + "）");
    log(callbacks, "在线探测：CAN通道已打开；尚不能据此判定ECU在线。");

    ProbePreconditions preconditions(*bus, plan, callbacks, stop);
    preconditions.start();
    const auto response_summary =
        execute_probe_session(plan, *bus, preconditions, callbacks, stop);

    result.success = true;
    result.message = plan.expected_profile_ids ? "设备在线：响应 "
                                               : "自定义端点在线：响应 ";
    result.message += response_summary;
    progress(callbacks, 100,
             "在线探测完成：已收到并核验物理诊断响应");
  } catch (const std::exception& error) {
    result.cancelled =
        stop.stop_requested() || error.what() == probe_detail::kCancelled;
    if (!result.cancelled) {
      log(callbacks, "ERROR：在线探测异常：" + std::string(error.what()));
    }
    result.message = result.cancelled
                         ? "在线探测已停止"
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
