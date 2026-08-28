#include "app/flash_controller.hpp"

#include "core/html_report.hpp"
#include "drivers/can/tracing_can_bus_provider.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace uds::app {
namespace {

bool is_cancelled(const std::exception& error, std::stop_token stop) {
  return stop.stop_requested() ||
         std::string_view(error.what()) == "operation cancelled by user";
}

std::string utf8_path(const std::filesystem::path& path) {
  const auto encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path cycle_trace_path(const std::filesystem::path& base,
                                       unsigned cycle,
                                       unsigned repeat_count) {
  if (repeat_count <= 1) return base;
  const auto width = static_cast<int>(
      std::max<std::size_t>(4, std::to_string(repeat_count).size()));
  std::ostringstream suffix;
  suffix << "_cycle_" << std::setfill('0') << std::setw(width) << cycle
         << "_of_" << std::setw(width) << repeat_count;
  const auto suffix_text = suffix.str();
  return base.parent_path() /
         (base.stem().wstring() +
          std::wstring(suffix_text.begin(), suffix_text.end()) +
          base.extension().wstring());
}

std::string transcript_type(const std::string& line) {
  if (line.starts_with("TX [")) return "TX request";
  if (line.starts_with("RX [")) return "RX response";
  if (line.find("timeout") != std::string::npos ||
      line.find("no response") != std::string::npos) {
    return "Response / timeout";
  }
  return "Workflow";
}

std::string transcript_verdict(const std::string& line) {
  if (line.starts_with("ERROR:") || line.find(" FAIL") != std::string::npos ||
      line.ends_with("FAIL")) {
    return "FAIL";
  }
  if (line.starts_with("WARN:")) return "WARN";
  if (line.find(" PASS") != std::string::npos || line.ends_with("PASS")) {
    return "PASS";
  }
  return "INFO";
}

std::string diagnostic_endpoint_detail(const FlashRequest& request) {
  std::ostringstream detail;
  const auto append_id = [&](std::uint32_t id) {
    const auto width = id > 0x7FFU ? 8 : 3;
    detail << "0x" << std::uppercase << std::hex << std::setfill('0')
           << std::setw(width) << id << std::dec;
  };
  detail << "Channel " << request.channel
         << "; Physical request TX (Tester -> ECU)=";
  append_id(request.tx_id);
  detail << "; Physical response RX (ECU -> Tester)=";
  append_id(request.rx_id);
  if (request.functional_id != 0) {
    detail << "; Functional request TX (Tester -> ECUs)=";
    append_id(request.functional_id);
  }
  detail << "; Entry=";
  const auto entry = request.entry_mode.empty() ? L"app" : request.entry_mode;
  for (const auto character : entry) {
    detail << static_cast<char>(character <= 0x7F ? character : '?');
  }
  if (request.profile.supports_ft_entry && request.profile.ft_tx_id != 0 &&
      request.profile.ft_rx_id != 0) {
    detail << "; FT recovery request/response=";
    append_id(request.profile.ft_tx_id);
    detail << "/";
    append_id(request.profile.ft_rx_id);
    if (entry != L"ft") detail << " (not used by this entry)";
  }
  return detail.str();
}

std::string can_configuration_detail(const FlashRequest& request) {
  std::ostringstream detail;
  detail << "Hardware backend=" << request.hardware_backend
         << "; Channel=" << request.channel
         << "; Nominal bitrate=" << request.nominal_bitrate << " bit/s";
  if (request.profile.can_fd) {
    detail << "; Data bitrate=" << request.data_bitrate << " bit/s; CAN FD=yes";
  } else {
    detail << "; CAN FD=no (Classic CAN)";
  }
  detail << "; Padding=0x" << std::uppercase << std::hex
         << std::setfill('0') << std::setw(2)
         << static_cast<unsigned>(request.padding) << std::dec;
  return detail.str();
}

std::string qualification_detail(const FlashRequest& request) {
  std::string detail = "Status=" + request.qualification_status;
  if (!request.qualification_completed_at.empty()) {
    detail += "; Completed at=" + request.qualification_completed_at;
  }
  detail += "; Detail=" + request.qualification_detail;
  detail += "; This record is audit evidence and does not bypass workflow preflight";
  return detail;
}

std::string file_configuration_detail(std::string_view role,
                                      const std::filesystem::path& path) {
  std::ostringstream detail;
  detail << role << '=';
  if (path.empty()) {
    detail << "<not configured>";
    return detail.str();
  }
  std::error_code error;
  auto resolved = path;
  if (!resolved.is_absolute()) resolved = std::filesystem::absolute(path, error);
  if (error) resolved = path;
  detail << utf8_path(resolved.lexically_normal());
  error.clear();
  const auto exists = std::filesystem::is_regular_file(resolved, error);
  detail << "; exists=" << (exists && !error ? "yes" : "no");
  if (exists && !error) {
    const auto size = std::filesystem::file_size(resolved, error);
    if (!error) detail << "; size=" << size << " bytes";
  }
  return detail.str();
}

std::wstring widen_utf8(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<std::uint8_t>(text[index]);
    std::uint32_t code_point{};
    std::size_t length{};
    if (first < 0x80U) {
      code_point = first;
      length = 1;
    } else if ((first & 0xE0U) == 0xC0U) {
      code_point = first & 0x1FU;
      length = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
      code_point = first & 0x0FU;
      length = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
      code_point = first & 0x07U;
      length = 4;
    } else {
      result.push_back(L'\uFFFD');
      ++index;
      continue;
    }

    if (index + length > text.size()) {
      result.push_back(L'\uFFFD');
      break;
    }
    bool valid = true;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation =
          static_cast<std::uint8_t>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        valid = false;
        break;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (!valid || (length == 2 && code_point < 0x80U) ||
        (length == 3 && code_point < 0x800U) ||
        (length == 4 && code_point < 0x10000U) ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU) ||
        code_point > 0x10FFFFU) {
      result.push_back(L'\uFFFD');
      ++index;
      continue;
    }

    if constexpr (sizeof(wchar_t) == 2) {
      if (code_point <= 0xFFFFU) {
        result.push_back(static_cast<wchar_t>(code_point));
      } else {
        code_point -= 0x10000U;
        result.push_back(static_cast<wchar_t>(0xD800U + (code_point >> 10U)));
        result.push_back(static_cast<wchar_t>(0xDC00U + (code_point & 0x3FFU)));
      }
    } else {
      result.push_back(static_cast<wchar_t>(code_point));
    }
    index += length;
  }
  return result;
}

} // namespace

FlashController::FlashController(OperationState& state,
                                 WorkflowFactory workflow_factory,
                                 std::shared_ptr<ICanBusProvider> bus_provider)
    : state_(state), workflow_factory_(std::move(workflow_factory)),
      bus_provider_(std::move(bus_provider)) {
  if (!workflow_factory_) {
    workflow_factory_ = [](std::wstring_view flow_id) {
      return create_flash_workflow(flow_id);
    };
  }
  if (!bus_provider_) bus_provider_ = default_can_bus_provider();
}

FlashController::~FlashController() {
  request_stop();
  wait();
}

bool FlashController::start(FlashRequest request,
                            OperationCallbacks callbacks) {
  std::scoped_lock lock(worker_mutex_);
  if (!state_.try_start(OperationKind::flash)) return false;

  try {
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread(
        [this, request = std::move(request), callbacks = std::move(callbacks)](
            std::stop_token stop) mutable {
          execute(std::move(request), std::move(callbacks), stop);
        });
  } catch (...) {
    state_.finish();
    throw;
  }
  return true;
}

bool FlashController::request_stop() {
  std::scoped_lock lock(worker_mutex_);
  const auto current = state_.snapshot();
  if (current.kind != OperationKind::flash ||
      current.phase == OperationPhase::idle) {
    return false;
  }
  state_.request_stop();
  if (worker_.joinable()) worker_.request_stop();
  return true;
}

void FlashController::wait() {
  std::scoped_lock lock(worker_mutex_);
  if (worker_.joinable()) worker_.join();
}

bool FlashController::is_active() const {
  const auto current = state_.snapshot();
  return current.kind == OperationKind::flash &&
         current.phase != OperationPhase::idle;
}

void FlashController::execute(FlashRequest request,
                              OperationCallbacks callbacks,
                              std::stop_token stop) {
  HtmlReport report;
  auto add_report = [&](int percent, std::string step, std::string verdict,
                         std::string detail) {
    static_cast<void>(percent);
    report.add({{}, std::move(step), std::move(verdict), std::move(detail)});
  };

  OperationResult result;
  std::string report_title = "UDS Download Report";
  unsigned active_cycle{};
  const auto repeat_count = request.repeat_count;
  try {
    if (repeat_count < kMinFlashRepeatCount ||
        repeat_count > kMaxFlashRepeatCount) {
      throw std::runtime_error(
          "flash repeat count must be in range " +
          std::to_string(kMinFlashRepeatCount) + ".." +
          std::to_string(kMaxFlashRepeatCount));
    }
    request.profile.channel = request.channel;
    request.profile.tx_id = request.tx_id;
    request.profile.rx_id = request.rx_id;
    request.profile.functional_id = request.functional_id;
    request.profile.nominal_bitrate = request.nominal_bitrate;
    request.profile.data_bitrate = request.data_bitrate;
    request.profile.padding = request.padding;
    const auto selected_target = std::find_if(
        request.profile.targets.cbegin(), request.profile.targets.cend(),
        [&request](const FlashTargetProfile& target) {
          return target.id == request.target_id;
        });
    if (selected_target != request.profile.targets.cend() &&
        selected_target->ft_tx_id != 0 && selected_target->ft_rx_id != 0) {
      request.profile.ft_tx_id = selected_target->ft_tx_id;
      request.profile.ft_rx_id = selected_target->ft_rx_id;
    }

    add_report(0, "Diagnostic IDs", "INFO",
               diagnostic_endpoint_detail(request));
    const auto record_snapshot = [&](std::string step, std::string detail) {
      if (callbacks.onLog) callbacks.onLog(step + ": " + detail);
      add_report(0, std::move(step), "INFO", std::move(detail));
    };
    record_snapshot("Flash target", request.target_description);
    record_snapshot("Pre-flash qualification", qualification_detail(request));
    record_snapshot("CAN configuration", can_configuration_detail(request));
    record_snapshot("Flash file", file_configuration_detail(
                                      "Boot Driver", request.driver_file));
    record_snapshot("Flash file", file_configuration_detail(
                                      "Driver Data", request.driver_verify_file));
    record_snapshot("Flash file",
                    file_configuration_detail("APP", request.app_file));
    record_snapshot("Flash file", file_configuration_detail(
                                      "APP Data", request.app_verify_file));
    record_snapshot("Flash file",
                    file_configuration_detail("CAL", request.cal_file));
    record_snapshot("Flash file", file_configuration_detail(
                                      "CAL Data", request.cal_verify_file));
    record_snapshot("Flash file", file_configuration_detail(
                                      "SeedKey", request.security_dll));
    add_report(0, "Flash count", "INFO",
               "Configured repetitions=" + std::to_string(repeat_count) +
                   "; each repetition runs the complete workflow; stop on first failure");

    FlashJob job;
    job.profile = request.profile;
    job.entry_mode = request.entry_mode;
    job.skip_signature_verification = request.skip_signature_verification;
    job.executable_directory = request.executable_directory;
    job.driver_file = request.driver_file;
    job.app_file = request.app_file;
    job.cal_file = request.cal_file;
    job.driver_verify_file = request.driver_verify_file;
    job.app_verify_file = request.app_verify_file;
    job.cal_verify_file = request.cal_verify_file;
    job.security_dll = request.security_dll;
    job.can_bus_provider = bus_provider_;

    for (active_cycle = 1; active_cycle <= repeat_count; ++active_cycle) {
      if (stop.stop_requested()) {
        throw std::runtime_error("operation cancelled by user");
      }
      auto workflow = workflow_factory_(request.profile.flow);
      if (!workflow) throw std::runtime_error("workflow factory returned null");
      if (active_cycle == 1) {
        report_title = workflow->report_title(request.profile);
      }
      const auto cycle_tag = "第" + std::to_string(active_cycle) + "/" +
                             std::to_string(repeat_count) + "次";
      std::shared_ptr<TracingCanBusProvider> tracing_provider;
      if (!request.trace_file.empty()) {
        const auto trace_path =
            cycle_trace_path(request.trace_file, active_cycle, repeat_count);
        tracing_provider = std::make_shared<TracingCanBusProvider>(
            bus_provider_, trace_path, request.channel);
        job.can_bus_provider = tracing_provider;
        const auto trace_detail =
            tracing_provider->trace_is_open()
                ? "Cycle " + std::to_string(active_cycle) + "/" +
                      std::to_string(repeat_count) + " raw ASC: " +
                      utf8_path(trace_path)
                : "WARN: failed to create cycle " +
                      std::to_string(active_cycle) + "/" +
                      std::to_string(repeat_count) + " raw ASC: " +
                      utf8_path(trace_path);
        if (callbacks.onLog) callbacks.onLog(trace_detail);
        add_report(0,
                   "ASC Trace cycle " + std::to_string(active_cycle) + "/" +
                       std::to_string(repeat_count),
                   tracing_provider->trace_is_open() ? "INFO" : "WARN",
                   trace_detail);
      }
      if (callbacks.onLog) callbacks.onLog(cycle_tag + "完整刷写开始");
      add_report(repeat_count == 1 ? 0 :
                     static_cast<int>((active_cycle - 1U) * 100U /
                                      repeat_count),
                 "Flash cycle " + std::to_string(active_cycle) + "/" +
                     std::to_string(repeat_count),
                 "INFO", "Complete workflow started");

      FlashWorkflowCallbacks workflow_callbacks;
      workflow_callbacks.log = [&](const std::string& line) {
        const auto decorated = repeat_count == 1
                                   ? line
                                   : "[" + cycle_tag + "] " + line;
        if (callbacks.onLog) callbacks.onLog(decorated);
        report.add_transcript(
            {{}, transcript_type(line), transcript_verdict(line), decorated});
      };
      workflow_callbacks.progress = [&](int percent,
                                        const std::string& line) {
        if (!callbacks.onProgress) return;
        if (repeat_count == 1) {
          callbacks.onProgress(percent, line);
          return;
        }
        const auto clamped = std::clamp(percent, 0, 100);
        const auto overall = static_cast<int>(
            ((active_cycle - 1U) * 100U +
             static_cast<unsigned>(clamped)) /
            repeat_count);
        callbacks.onProgress(overall, "[" + cycle_tag + "] " + line);
      };
      workflow_callbacks.report =
          [&](std::string step, std::string verdict,
               std::string detail) {
            if (repeat_count > 1) {
              step = "[" + cycle_tag + "] " + step;
              detail = "[" + cycle_tag + "] " + detail;
            }
            add_report(0, std::move(step), std::move(verdict),
                       std::move(detail));
          };

      try {
        workflow->run(job, workflow_callbacks, stop);
      } catch (...) {
        job.can_bus_provider = bus_provider_;
        tracing_provider.reset();
        throw;
      }
      job.can_bus_provider = bus_provider_;
      tracing_provider.reset();
      if (callbacks.onLog) callbacks.onLog(cycle_tag + "完整刷写完成");
      add_report(static_cast<int>(active_cycle * 100U / repeat_count),
                 "Flash cycle " + std::to_string(active_cycle) + "/" +
                     std::to_string(repeat_count),
                 "PASS", "Complete workflow passed");
      if (repeat_count > 1 && callbacks.onProgress) {
        callbacks.onProgress(
            static_cast<int>(active_cycle * 100U / repeat_count),
            cycle_tag + "完整刷写完成");
      }
    }
    result.success = true;
    result.message = request.profile.name + L" 刷写成功（" +
                     std::to_wstring(repeat_count) + L"/" +
                     std::to_wstring(repeat_count) + L"次）";
  } catch (const std::exception& error) {
    result.cancelled = is_cancelled(error, stop);
    const auto cycle_prefix =
        active_cycle > 0 && active_cycle <= repeat_count
            ? ("第" + std::to_string(active_cycle) + "/" +
               std::to_string(repeat_count) + "次：")
            : std::string{};
    if (result.cancelled) {
      add_report(0, "Download", "FAIL",
                 cycle_prefix + "User requested abort: " + error.what());
      report.add_transcript(
          {{}, "Controller", "FAIL",
           cycle_prefix + "User requested abort: " + error.what()});
      result.message = request.profile.name + L" 刷写已中断：" +
                       widen_utf8(cycle_prefix + error.what());
    } else {
      add_report(0, "Download", "FAIL", cycle_prefix + error.what());
      report.add_transcript(
          {{}, "Controller", "FAIL", cycle_prefix + error.what()});
      result.message = request.profile.name + L" 刷写失败：" +
                       widen_utf8(cycle_prefix + error.what());
    }
  } catch (...) {
    add_report(0, "Download", "FAIL", "unknown exception");
    report.add_transcript({{}, "Controller", "FAIL", "unknown exception"});
    result.cancelled = stop.stop_requested();
    result.message = request.profile.name +
                     (result.cancelled ? L" 刷写已中断：unknown exception"
                                       : L" 刷写失败：unknown exception");
  }

  try {
    result.report_path =
        report.write(request.executable_directory / L"logs" / L"reports",
                     report_title);
  } catch (const std::exception& error) {
    result.message += L"；报告写入失败：" +
                      widen_utf8(error.what());
  }

  state_.finish();
  if (callbacks.onFinished) {
    try {
      callbacks.onFinished(std::move(result));
    } catch (...) {
      // A presentation callback must not escape the worker thread.
    }
  }
}

} // namespace uds::app
