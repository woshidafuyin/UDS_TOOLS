#include "app/flash_controller.hpp"
#include "app/flash_request.hpp"
#include "app/operation_state.hpp"
#include "app/probe_controller.hpp"
#include "app/probe_plan.hpp"
#include "app/probe_service.hpp"
#include "app/version_check_service.hpp"
#include "app/version_value_decoder.hpp"
#include "app/diagnostic_request_service.hpp"
#include "core/profile.hpp"
#include "flash/chuneng_331_flow.hpp"
#include "flash/geely_p416_flow.hpp"
#include "flash/shidaixinan_hjzj_fmr_flow.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void test_flash_request_defaults() {
  const uds::app::FlashRequest request;
  check(request.entry_mode == L"app", "flash request entry mode default mismatch");
  check(uds::app::kMinFlashRepeatCount == 1 &&
            uds::app::kMaxFlashRepeatCount == 10000 &&
            request.repeat_count == uds::app::kMinFlashRepeatCount &&
            request.channel == 0 &&
            request.tx_id == 0 && request.rx_id == 0,
        "flash request numeric defaults mismatch");
  check(request.driver_file.empty() && request.app_file.empty() &&
            request.security_dll.empty(),
        "flash request path defaults mismatch");
}

void test_chuneng_ft_entry_configuration() {
  const auto app = uds::resolve_chuneng_331_entry_plan(L"app");
  check(!app.use_ft_endpoint && app.run_standard_preprogramming &&
            !app.boot_only_to_app,
        "Chuneng APP entry plan mismatch");
  const auto boot = uds::resolve_chuneng_331_entry_plan(L"boot");
  check(!boot.use_ft_endpoint && !boot.run_standard_preprogramming &&
            boot.boot_only_to_app,
        "Chuneng BOOT-to-APP entry plan mismatch");
  const auto ft = uds::resolve_chuneng_331_entry_plan(L"ft");
  check(ft.use_ft_endpoint && !ft.run_standard_preprogramming &&
            !ft.boot_only_to_app,
        "Chuneng FT entry plan mismatch");

  bool rejected_invalid_mode{};
  try {
    static_cast<void>(uds::resolve_chuneng_331_entry_plan(L"auto"));
  } catch (const std::invalid_argument&) {
    rejected_invalid_mode = true;
  }
  check(rejected_invalid_mode,
        "Chuneng entry plan accepted an unsupported mode");

  const auto source = std::filesystem::path(UDS_SOURCE_DIR) / "profiles" /
                      "chuneng_331_left_rear.ini";
  const auto profile = uds::load_profile_ini(source);
  check(profile.project_name == L"ARC331" &&
            profile.device_name == L"右后雷达" &&
            profile.supports_ft_entry && profile.default_entry_mode == L"app" &&
            profile.tx_id == 0x72C && profile.rx_id == 0x72D &&
            profile.ft_tx_id == 0x701 && profile.ft_rx_id == 0x761 &&
            profile.ft_padding == 0x55 && !profile.ft_extended_id &&
            !profile.ft_uds_fd && !profile.ft_uds_brs &&
            profile.targets.size() == 2 &&
            profile.targets[0].id == L"right_rear" &&
            profile.targets[0].display_name == L"右后雷达" &&
            profile.targets[0].tx_id == 0x72C &&
            profile.targets[0].rx_id == 0x72D &&
            profile.targets[0].pending_validation &&
            profile.targets[1].id == L"left_rear" &&
            profile.targets[1].display_name == L"左后雷达" &&
            profile.targets[1].tx_id == 0x72E &&
            profile.targets[1].rx_id == 0x72F &&
            profile.targets[1].pending_validation,
        "Chuneng FTOrAPP profile configuration mismatch");
}

void test_operation_state_transitions() {
  uds::app::OperationState state;
  uds::app::OperationId operation_id{};
  auto value = state.snapshot();
  check(value.kind == uds::app::OperationKind::none &&
            value.phase == uds::app::OperationPhase::idle,
        "operation state did not start idle");
  check(!state.try_start(uds::app::OperationKind::none),
        "operation state accepted an empty operation");
  check(state.try_start(uds::app::OperationKind::flash, &operation_id) &&
            operation_id != 0,
        "operation state rejected the first operation");
  check(!state.try_start(uds::app::OperationKind::probe),
        "operation state allowed concurrent operations");
  check(state.is_active() && state.is_running(),
        "operation state did not report a running operation");
  check(state.request_stop(), "operation state rejected a stop request");
  value = state.snapshot();
  check(value.kind == uds::app::OperationKind::flash &&
            value.phase == uds::app::OperationPhase::stopping,
        "operation state stopping snapshot mismatch");
  check(state.request_stop(), "operation state stop was not idempotent");
  check(state.finish(operation_id),
        "operation state rejected the matching completion token");
  check(!state.is_active() && !state.request_stop(),
        "operation state did not return to idle");
}

void test_operation_state_concurrent_start() {
  uds::app::OperationState state;
  std::atomic_int accepted{};
  std::vector<std::thread> workers;
  for (int i = 0; i < 16; ++i) {
    workers.emplace_back([&] {
      if (state.try_start(uds::app::OperationKind::probe)) ++accepted;
    });
  }
  for (auto& worker : workers) worker.join();
  check(accepted == 1, "operation state accepted multiple concurrent starts");
  check(state.finish(state.snapshot().id),
        "operation state rejected concurrent-start winner completion");
}

void test_operation_state_rejects_stale_completion() {
  uds::app::OperationState state;
  uds::app::OperationId first{};
  uds::app::OperationId second{};
  check(state.try_start(uds::app::OperationKind::probe, &first) && first != 0,
        "operation state did not issue the first operation ID");
  check(state.finish(first), "first operation did not finish");
  check(state.try_start(uds::app::OperationKind::probe, &second) &&
            second != 0 && second != first,
        "operation state did not issue a distinct second operation ID");
  check(!state.finish(0), "operation state accepted the reserved zero ID");
  check(!state.finish(first),
        "stale completion was allowed to finish the next operation");
  const auto active = state.snapshot();
  check(active.kind == uds::app::OperationKind::probe &&
            active.phase == uds::app::OperationPhase::running &&
            active.id == second,
        "stale completion corrupted the active operation snapshot");
  check(state.is_latest(second) && !state.is_latest(first) &&
            !state.is_latest(0),
        "operation state latest-ID predicate is incorrect");
  check(state.finish(second), "second operation did not finish");
}

void test_operation_state_id_wrap_skips_reserved_zero() {
  uds::app::OperationState state(
      std::numeric_limits<uds::app::OperationId>::max() - 1);
  uds::app::OperationId maximum{};
  uds::app::OperationId wrapped{};
  check(state.try_start(uds::app::OperationKind::flash, &maximum) &&
            maximum == std::numeric_limits<uds::app::OperationId>::max(),
        "operation ID did not reach the maximum boundary");
  check(state.finish(maximum), "maximum operation ID did not finish");
  check(state.try_start(uds::app::OperationKind::version_check, &wrapped) &&
            wrapped == 1,
        "operation ID wrap did not skip the reserved zero value");
  check(state.finish(wrapped), "wrapped operation ID did not finish");
}

void test_operation_callbacks() {
  int log_count = 0;
  int progress = -1;
  bool finished = false;
  uds::app::OperationCallbacks callbacks;
  callbacks.onLog = [&](const std::string& line) {
    if (line == "log") ++log_count;
  };
  callbacks.onProgress = [&](int value, const std::string& line) {
    if (line == "progress") progress = value;
  };
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    finished = result.success && result.message == L"done";
  };
  callbacks.onLog("log");
  callbacks.onProgress(42, "progress");
  callbacks.onFinished({true, false, L"done", {}});
  check(log_count == 1 && progress == 42 && finished,
        "operation callbacks did not forward values");
}

struct WorkflowCapture {
  uds::FlashJob job;
  std::atomic_bool ran{};
  std::atomic_int run_count{};
};

class FakeWorkflow final : public uds::FlashWorkflow {
public:
  enum class Behavior { success, failure, wait_for_stop };

  FakeWorkflow(std::shared_ptr<WorkflowCapture> capture, Behavior behavior)
      : capture_(std::move(capture)), behavior_(behavior) {}

  std::wstring_view id() const noexcept override { return L"fake"; }
  std::string report_title(const uds::FlashProfile&) const override {
    return "FlashController Test Report";
  }

  void run(const uds::FlashJob& job,
           const uds::FlashWorkflowCallbacks& callbacks,
           std::stop_token stop) override {
    capture_->job = job;
    capture_->ran = true;
    ++capture_->run_count;
    if (callbacks.log) callbacks.log("fake log");
    if (callbacks.progress) callbacks.progress(25, "fake progress");
    if (callbacks.event) {
      callbacks.event({{}, 0, uds::FlashStage::app_transfer,
                       static_cast<std::uint8_t>(0x36),
                       uds::FlashImageRole::app,
                       "27 SecurityAccess misleading event", "PASS",
                       "STRUCTURED_FLASH_EVENT"});
    } else if (callbacks.report) {
      callbacks.report("Fake", "INFO", "fake report row");
    }
    if (behavior_ == Behavior::failure)
      throw std::runtime_error("synthetic workflow failure");
    if (behavior_ == Behavior::wait_for_stop) {
      while (!stop.stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      throw std::runtime_error("operation cancelled by user");
    }
  }

private:
  std::shared_ptr<WorkflowCapture> capture_;
  Behavior behavior_;
};

std::filesystem::path controller_test_directory(const wchar_t* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

uds::app::FlashRequest make_flash_request(
    const std::filesystem::path& directory) {
  uds::app::FlashRequest request;
  request.profile.id = L"fake";
  request.profile.flow = L"fake";
  request.profile.name = L"Fake ECU";
  request.entry_mode = L"ft";
  request.executable_directory = directory;
  request.hardware_backend = "ZLG / ZCANPRO (ZCAN)";
  request.target_description =
      "Chuneng / ARC331 / Left rear; Profile=chuneng_arc331; Target=left_rear; "
      "Flow=chuneng_arc331; Entry=APP; Repetitions=1";
  request.qualification_status = "PASS";
  request.qualification_detail = "设备在线：响应 50 03";
  request.qualification_completed_at = "2026-08-21T10:05:55.489+08:00";
  request.channel = 3;
  request.tx_id = 0x701;
  request.rx_id = 0x761;
  request.functional_id = 0x7DF;
  request.nominal_bitrate = 500000;
  request.data_bitrate = 2000000;
  request.padding = 0x55;
  request.driver_file = L"driver.s19";
  request.app_file = L"app.s19";
  request.cal_file = L"cal.s19";
  request.driver_verify_file = L"driver.asc";
  request.app_verify_file = L"app.asc";
  request.cal_verify_file = L"cal.asc";
  request.security_dll = L"security.dll";
  return request;
}

void test_flash_controller_success() {
  const auto directory = controller_test_directory(L"uds_flash_controller_success");
  auto capture = std::make_shared<WorkflowCapture>();
  uds::app::OperationState state;
  uds::app::FlashController controller(
      state, [capture](std::wstring_view flow_id) {
        check(flow_id == L"fake", "flash controller passed wrong flow id");
        return std::make_unique<FakeWorkflow>(capture,
                                               FakeWorkflow::Behavior::success);
      });

  std::promise<uds::app::OperationResult> completion;
  auto completed = completion.get_future();
  int log_count = 0;
  std::vector<std::string> audit_logs;
  int progress = -1;
  uds::app::OperationCallbacks callbacks;
  callbacks.onLog = [&](const std::string& line) {
    if (line == "fake log") ++log_count;
    if (line.find("Flash target:") == 0 ||
        line.find("Pre-flash qualification:") == 0 ||
        line.find("CAN configuration:") == 0 ||
        line.find("Flash file:") == 0) {
      audit_logs.push_back(line);
    }
  };
  callbacks.onProgress = [&](int value, const std::string&) {
    progress = value;
  };
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    completion.set_value(std::move(result));
  };

  auto request = make_flash_request(directory);
  const auto trace_path = directory / "single_flash.asc";
  request.trace_file = trace_path;
  check(controller.start(std::move(request), std::move(callbacks)),
        "flash controller rejected a valid start");
  const auto result = completed.get();
  controller.wait();
  capture->job.can_bus_provider.reset();
  auto blf_trace_path = trace_path;
  blf_trace_path.replace_extension(L".blf");

  check(result.success && !result.cancelled,
        "flash controller did not report success");
  check(capture->ran.load() && capture->job.profile.channel == 3 &&
            capture->job.profile.tx_id == 0x701 &&
            capture->job.profile.rx_id == 0x761 &&
            capture->job.entry_mode == L"ft" &&
            capture->job.security_dll ==
                std::filesystem::path(L"security.dll") &&
            std::filesystem::is_regular_file(trace_path) &&
            std::filesystem::is_regular_file(blf_trace_path) &&
            trace_path.stem() == blf_trace_path.stem(),
        "flash controller did not assemble FlashJob correctly");
  check(log_count == 1 && progress == 25 && audit_logs.size() == 10,
        "flash controller did not adapt workflow callbacks");
  check(!result.report_path.empty() &&
             std::filesystem::is_regular_file(result.report_path),
         "flash controller did not write a report");
  std::ifstream report_file(result.report_path, std::ios::binary);
  const std::string report_text{std::istreambuf_iterator<char>(report_file),
                                std::istreambuf_iterator<char>()};
  report_file.close();
  check(report_text.find("Diagnostic IDs") != std::string::npos &&
            report_text.find("Physical request TX (Tester -&gt; ECU)=0x701") !=
                std::string::npos &&
            report_text.find("Physical response RX (ECU -&gt; Tester)=0x761") !=
                std::string::npos &&
            report_text.find("Functional request TX (Tester -&gt; ECUs)=0x7DF") !=
                std::string::npos,
        "flash report did not label diagnostic ID directions");
  check(report_text.find("Pre-flash qualification") != std::string::npos &&
            report_text.find("Profile=chuneng_arc331") != std::string::npos &&
            report_text.find("Status=PASS") != std::string::npos &&
            report_text.find("Hardware backend=ZLG / ZCANPRO (ZCAN)") !=
                std::string::npos &&
            report_text.find("Nominal bitrate=500000 bit/s") !=
                std::string::npos &&
            report_text.find("Data bitrate=2000000 bit/s") !=
                std::string::npos &&
            report_text.find("driver.s19; exists=no") != std::string::npos &&
            report_text.find("security.dll; exists=no") != std::string::npos,
        "flash report is missing qualification, CAN, or file configuration");
  check(report_text.find("本次流程结果：成功（PASS）") != std::string::npos &&
            report_text.find("完成时间：") != std::string::npos &&
            report_text.find("C++") == std::string::npos &&
            report_text.find("<th>%</th>") == std::string::npos &&
            report_text.find("id='report-navigation'") !=
                std::string::npos &&
            report_text.find("id='configuration'") != std::string::npos &&
            report_text.find("id='raw-log'") != std::string::npos &&
            report_text.find("<th>时间</th>") != std::string::npos &&
            std::regex_search(
                report_text,
                std::regex(R"(<td>\d{2}:\d{2}:\d{2}\.\d{3}</td>)")),
        "flash report is missing the top result/time summary or exposes C++");
  const auto app_transfer_anchor =
      report_text.find("id='cycle-1-app-transfer'");
  const auto security_anchor =
      report_text.find("id='cycle-1-security-access'");
  const auto app_transfer_heading_end =
      report_text.find("</h5>", app_transfer_anchor);
  const auto security_heading_end =
      report_text.find("</h4>", security_anchor);
  check(app_transfer_anchor != std::string::npos &&
            app_transfer_heading_end != std::string::npos &&
            report_text.substr(app_transfer_anchor,
                               app_transfer_heading_end - app_transfer_anchor)
                    .find("status PASS") != std::string::npos &&
            security_anchor != std::string::npos &&
            security_heading_end != std::string::npos &&
            report_text.substr(security_anchor,
                               security_heading_end - security_anchor)
                    .find("status NOT_RUN") != std::string::npos,
        "FlashController did not route the structured FlashEvent by stage");
  std::size_t html_count{};
  for (const auto& entry :
       std::filesystem::directory_iterator(directory / "logs" / "reports")) {
    if (entry.path().extension() == ".html") ++html_count;
  }
  check(html_count == 2,
        "flash controller did not retain latest and historical reports");
  check(!state.is_active(), "flash controller did not release operation state");
  std::filesystem::remove_all(directory);
}

void test_flash_controller_exception_conversion() {
  const auto directory = controller_test_directory(L"uds_flash_controller_failure");
  auto capture = std::make_shared<WorkflowCapture>();
  uds::app::OperationState state;
  uds::app::FlashController controller(
      state, [capture](std::wstring_view) {
        return std::make_unique<FakeWorkflow>(capture,
                                               FakeWorkflow::Behavior::failure);
      });
  std::promise<uds::app::OperationResult> completion;
  auto completed = completion.get_future();
  uds::app::OperationCallbacks callbacks;
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(make_flash_request(directory), std::move(callbacks)),
        "flash controller rejected failure-path start");
  const auto result = completed.get();
  controller.wait();
  check(!result.success && !result.cancelled &&
            result.message.find(L"synthetic workflow failure") !=
                std::wstring::npos,
        "flash controller did not convert workflow exception");
  check(std::filesystem::is_regular_file(result.report_path),
        "flash controller failure path did not write a report");
  std::ifstream report_file(result.report_path, std::ios::binary);
  const std::string report_text{std::istreambuf_iterator<char>(report_file),
                                std::istreambuf_iterator<char>()};
  check(report_text.find("本次流程结果：失败（FAIL）") != std::string::npos &&
            report_text.find("完成时间：") != std::string::npos,
        "failure report is missing the top failure/time summary");
  report_file.close();
  std::filesystem::remove_all(directory);
}

void test_flash_controller_repeat_count() {
  const auto directory =
      controller_test_directory(L"uds_flash_controller_repeat_count");
  auto capture = std::make_shared<WorkflowCapture>();
  uds::app::OperationState state;
  uds::app::FlashController controller(
      state, [capture](std::wstring_view) {
        return std::make_unique<FakeWorkflow>(
            capture, FakeWorkflow::Behavior::success);
      });
  auto request = make_flash_request(directory);
  request.repeat_count = 3;
  request.trace_file = directory / "complete_cycle_trace.asc";
  std::promise<uds::app::OperationResult> completion;
  auto completed = completion.get_future();
  int progress = -1;
  unsigned trace_log_count{};
  uds::app::OperationCallbacks callbacks;
  callbacks.onLog = [&](const std::string& line) {
    if (line.find("raw ASC PASS:") != std::string::npos &&
        line.find("raw BLF PASS:") != std::string::npos) {
      ++trace_log_count;
    }
  };
  callbacks.onProgress =
      [&](int value, const std::string&) { progress = value; };
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(std::move(request), std::move(callbacks)),
        "flash controller rejected repeated flash start");
  const auto result = completed.get();
  controller.wait();
  capture->job.can_bus_provider.reset();
  const auto trace1 = directory / "complete_cycle_trace_cycle_0001_of_0003.asc";
  const auto trace2 = directory / "complete_cycle_trace_cycle_0002_of_0003.asc";
  const auto trace3 = directory / "complete_cycle_trace_cycle_0003_of_0003.asc";
  auto blf1 = trace1;
  auto blf2 = trace2;
  auto blf3 = trace3;
  blf1.replace_extension(L".blf");
  blf2.replace_extension(L".blf");
  blf3.replace_extension(L".blf");
  check(result.success && capture->run_count == 3 && progress == 100 &&
            trace_log_count == 3 && std::filesystem::is_regular_file(trace1) &&
            std::filesystem::is_regular_file(trace2) &&
            std::filesystem::is_regular_file(trace3) &&
            std::filesystem::is_regular_file(blf1) &&
            std::filesystem::is_regular_file(blf2) &&
            std::filesystem::is_regular_file(blf3) &&
            trace1.stem() == blf1.stem() && trace2.stem() == blf2.stem() &&
            trace3.stem() == blf3.stem() &&
            result.message.find(L"3/3次") != std::wstring::npos,
        "repeated flashing did not retain one same-name ASC/BLF pair per complete cycle");
  std::ifstream report_file(result.report_path, std::ios::binary);
  const std::string report_text{std::istreambuf_iterator<char>(report_file),
                                std::istreambuf_iterator<char>()};
  check(report_text.find("Configured repetitions=3") != std::string::npos &&
            report_text.find("Flash cycle 3/3") != std::string::npos &&
            report_text.find("complete_cycle_trace_cycle_0001_of_0003.asc") !=
                std::string::npos &&
            report_text.find("complete_cycle_trace_cycle_0002_of_0003.asc") !=
                std::string::npos &&
            report_text.find("complete_cycle_trace_cycle_0003_of_0003.asc") !=
                std::string::npos &&
            report_text.find("complete_cycle_trace_cycle_0001_of_0003.blf") !=
                std::string::npos &&
            report_text.find("complete_cycle_trace_cycle_0002_of_0003.blf") !=
                std::string::npos &&
            report_text.find("complete_cycle_trace_cycle_0003_of_0003.blf") !=
                std::string::npos &&
            report_text.find("完整原始日志") != std::string::npos,
        "repeated flash report did not index every cycle trace or transcript");
  report_file.close();
  std::filesystem::remove_all(directory);
}

void test_flash_controller_repeat_count_maximum() {
  const auto directory =
      controller_test_directory(L"uds_flash_controller_repeat_count_maximum");
  auto capture = std::make_shared<WorkflowCapture>();
  uds::app::OperationState state;
  uds::app::FlashController controller(
      state, [capture](std::wstring_view) {
        return std::make_unique<FakeWorkflow>(
            capture, FakeWorkflow::Behavior::success);
      });
  auto request = make_flash_request(directory);
  request.repeat_count = uds::app::kMaxFlashRepeatCount;
  std::promise<uds::app::OperationResult> completion;
  auto completed = completion.get_future();
  int progress = -1;
  bool progress_in_range = true;
  bool progress_monotonic = true;
  uds::app::OperationCallbacks callbacks;
  callbacks.onProgress = [&](int value, const std::string&) {
    progress_in_range = progress_in_range && value >= 0 && value <= 100;
    progress_monotonic = progress_monotonic && value >= progress;
    progress = value;
  };
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(std::move(request), std::move(callbacks)),
        "flash controller rejected 10000-repeat mock start");
  const auto result = completed.get();
  controller.wait();
  check(result.success &&
            capture->run_count == static_cast<int>(
                                      uds::app::kMaxFlashRepeatCount) &&
            progress == 100 && progress_in_range && progress_monotonic &&
            result.message.find(L"10000/10000次") != std::wstring::npos,
        "10000-repeat mock did not complete every workflow safely");
  std::ifstream report_file(result.report_path, std::ios::binary);
  const std::string report_text{std::istreambuf_iterator<char>(report_file),
                                std::istreambuf_iterator<char>()};
  check(report_text.find("Configured repetitions=10000") !=
                std::string::npos &&
            report_text.find("Flash cycle 10000/10000") !=
                std::string::npos,
        "10000-repeat report did not preserve the final cycle");
  report_file.close();
  std::filesystem::remove_all(directory);
}

void test_flash_controller_repeat_count_boundaries() {
  for (const auto invalid :
       {0U, uds::app::kMaxFlashRepeatCount + 1U}) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("uds_flash_controller_invalid_repeat_" + std::to_string(invalid));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto capture = std::make_shared<WorkflowCapture>();
    uds::app::OperationState state;
    uds::app::FlashController controller(
        state, [capture](std::wstring_view) {
          return std::make_unique<FakeWorkflow>(
              capture, FakeWorkflow::Behavior::success);
        });
    auto request = make_flash_request(directory);
    request.repeat_count = invalid;
    std::promise<uds::app::OperationResult> completion;
    auto completed = completion.get_future();
    uds::app::OperationCallbacks callbacks;
    callbacks.onFinished = [&](uds::app::OperationResult result) {
      completion.set_value(std::move(result));
    };
    check(controller.start(std::move(request), std::move(callbacks)),
          "flash controller did not enter invalid-boundary validation");
    const auto result = completed.get();
    controller.wait();
    check(!result.success && !result.cancelled && capture->run_count == 0 &&
              result.message.find(L"range 1..10000") != std::wstring::npos,
          "flash controller accepted an out-of-range repeat count");
    std::filesystem::remove_all(directory);
  }
}

void test_flash_controller_repeat_stops_on_first_failure() {
  const auto directory =
      controller_test_directory(L"uds_flash_controller_repeat_first_failure");
  auto capture = std::make_shared<WorkflowCapture>();
  uds::app::OperationState state;
  uds::app::FlashController controller(
      state, [capture](std::wstring_view) {
        return std::make_unique<FakeWorkflow>(
            capture, FakeWorkflow::Behavior::failure);
      });
  auto request = make_flash_request(directory);
  request.repeat_count = uds::app::kMaxFlashRepeatCount;
  std::promise<uds::app::OperationResult> completion;
  auto completed = completion.get_future();
  uds::app::OperationCallbacks callbacks;
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(std::move(request), std::move(callbacks)),
        "flash controller rejected first-failure repeat test");
  const auto result = completed.get();
  controller.wait();
  check(!result.success && !result.cancelled && capture->run_count == 1 &&
            result.message.find(L"第1/10000次") != std::wstring::npos,
        "repeated flash did not stop immediately on the first failure");
  std::ifstream report_file(result.report_path, std::ios::binary);
  const std::string report_text{std::istreambuf_iterator<char>(report_file),
                                std::istreambuf_iterator<char>()};
  check(report_text.find("Flash cycle 1/10000") != std::string::npos &&
            report_text.find("Flash cycle 2/10000") == std::string::npos,
        "first-failure report contains a later flash cycle");
  report_file.close();
  std::filesystem::remove_all(directory);
}

void test_flash_controller_stop() {
  const auto directory = controller_test_directory(L"uds_flash_controller_stop");
  auto capture = std::make_shared<WorkflowCapture>();
  uds::app::OperationState state;
  uds::app::FlashController controller(
      state, [capture](std::wstring_view) {
        return std::make_unique<FakeWorkflow>(
            capture, FakeWorkflow::Behavior::wait_for_stop);
      });
  std::promise<uds::app::OperationResult> completion;
  auto completed = completion.get_future();
  uds::app::OperationCallbacks callbacks;
  callbacks.onFinished = [&](uds::app::OperationResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(make_flash_request(directory), std::move(callbacks)),
        "flash controller rejected stop-path start");
  while (!capture->ran.load()) std::this_thread::yield();
  check(controller.request_stop(), "flash controller rejected stop request");
  const auto result = completed.get();
  controller.wait();
  check(!result.success && result.cancelled,
        "flash controller did not report cancellation");
  check(!state.is_active(),
        "flash controller did not release state after cancellation");
  std::filesystem::remove_all(directory);
}

struct ProbeBusCapture {
  std::atomic_bool opened{};
  std::vector<uds::CanFrame> sent;
};

class FakeProbeBus final : public uds::ICanBus {
public:
  FakeProbeBus(std::shared_ptr<ProbeBusCapture> capture, std::uint32_t response_id,
               bool respond, bool wrong_session = false,
               int respond_after_uds_request = 1,
               int nm_send_failures = 0)
      : capture_(std::move(capture)), response_id_(response_id),
        respond_(respond), wrong_session_(wrong_session),
        respond_after_uds_request_(respond_after_uds_request),
        nm_send_failures_(nm_send_failures) {}

  void open() override {
    opened_ = true;
    capture_->opened = true;
  }
  void close() noexcept override { opened_ = false; }
  bool is_open() const noexcept override { return opened_; }
  void send(const uds::CanFrame& frame) override {
    std::scoped_lock lock(send_mutex_);
    capture_->sent.push_back(frame);
    if ((frame.id == 0x18FFA025 || frame.id == 0x18FFA0B6) &&
        nm_send_failures_ > 0) {
      --nm_send_failures_;
      throw std::runtime_error("simulated first NM frame without ACK");
    }
    if (frame.data.size() >= 3 && frame.data[0] == 0x02 &&
        frame.data[1] == 0x10) {
      requested_session_ = frame.data[2];
      ++uds_request_count_;
    }
  }
  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds timeout) override {
    std::scoped_lock lock(send_mutex_);
    if (respond_ && !response_sent_ &&
        uds_request_count_ >= respond_after_uds_request_) {
      response_sent_ = true;
      const auto session = wrong_session_
                               ? static_cast<std::uint8_t>(
                                     requested_session_ == 0x01 ? 0x03 : 0x01)
                               : requested_session_;
      return uds::CanFrame{
           response_id_,
           {0x02, 0x50, session, 0x00, 0x00, 0x00, 0x00, 0x00},
           response_id_ > 0x7FFU, false, false};
    }
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(5)));
    return std::nullopt;
  }

private:
  std::shared_ptr<ProbeBusCapture> capture_;
  std::uint32_t response_id_{};
  bool respond_{};
  bool wrong_session_{};
  int respond_after_uds_request_{1};
  int nm_send_failures_{};
  int uds_request_count_{};
  std::uint8_t requested_session_{0x01};
  bool response_sent_{};
  bool opened_{};
  std::mutex send_mutex_;
};

class FakeVersionBus final : public uds::ICanBus {
public:
  void open() override { opened_ = true; }
  void close() noexcept override { opened_ = false; }
  bool is_open() const noexcept override { return opened_; }
  void send(const uds::CanFrame& frame) override {
    if (frame.data.size() >= 4 && frame.data[0] == 0x03 &&
        frame.data[1] == 0x22 && frame.data[2] == 0xF1 &&
        frame.data[3] == 0x89) {
      request_seen_ = true;
    }
  }
  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds timeout) override {
    if (request_seen_ && !response_sent_) {
      response_sent_ = true;
      return uds::CanFrame{
          0x708, {0x05, 0x62, 0xF1, 0x89, 'O', 'K', 0x00, 0x00},
          false, false, false};
    }
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(2)));
    return std::nullopt;
  }

private:
  bool opened_{};
  bool request_seen_{};
  bool response_sent_{};
};

struct ChunengVersionCapture {
  std::vector<uds::CanFrame> sent;
  std::mutex mutex;
};

class FakeChunengVersionBus final : public uds::ICanBus {
public:
  explicit FakeChunengVersionBus(
      std::shared_ptr<ChunengVersionCapture> capture)
      : capture_(std::move(capture)) {}
  void open() override { opened_ = true; }
  void close() noexcept override { opened_ = false; }
  bool is_open() const noexcept override { return opened_; }
  void send(const uds::CanFrame& frame) override {
    std::scoped_lock lock(capture_->mutex);
    capture_->sent.push_back(frame);
    if (frame.id == 0x72E && frame.data.size() >= 3 &&
        frame.data[1] == 0x10 && frame.data[2] == 0x01) {
      pending_ = 1;
    } else if (frame.id == 0x72E && frame.data.size() >= 4 &&
               frame.data[1] == 0x22 && frame.data[2] == 0xF1 &&
               frame.data[3] == 0x89) {
      pending_ = 2;
    }
  }
  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds timeout) override {
    if (pending_ == 1) {
      pending_ = 0;
      return uds::CanFrame{0x72F,
                           {0x06, 0x50, 0x01, 0x00, 0x32, 0x01, 0xF4, 0x00},
                           false, false, false};
    }
    if (pending_ == 2) {
      pending_ = 0;
      return uds::CanFrame{0x72F,
                           {0x05, 0x62, 0xF1, 0x89, 'O', 'K', 0x00, 0x00},
                           false, false, false};
    }
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(2)));
    return std::nullopt;
  }

private:
  std::shared_ptr<ChunengVersionCapture> capture_;
  bool opened_{};
  int pending_{};
};

struct XizhongVersionCapture {
  std::vector<uds::CanFrame> sent;
  std::mutex mutex;
};

class FakeXizhongVersionBus final : public uds::ICanBus {
public:
  FakeXizhongVersionBus(std::shared_ptr<XizhongVersionCapture> capture,
                        std::uint32_t response_id)
      : capture_(std::move(capture)), response_id_(response_id) {}

  void open() override { opened_ = true; }
  void close() noexcept override { opened_ = false; }
  bool is_open() const noexcept override { return opened_; }
  void send(const uds::CanFrame& frame) override {
    std::scoped_lock lock(capture_->mutex);
    capture_->sent.push_back(frame);
    if (frame.data.size() >= 4 && frame.data[0] == 0x03 &&
        frame.data[1] == 0x22 && frame.data[2] == 0xF1 &&
        frame.data[3] == 0x89) {
      request_seen_ = true;
    }
  }
  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds timeout) override {
    std::scoped_lock lock(capture_->mutex);
    if (request_seen_ && !response_sent_) {
      response_sent_ = true;
      return uds::CanFrame{response_id_,
                           {0x05, 0x62, 0xF1, 0x89, 'O', 'K', 0x00, 0x00},
                           true, true, true};
    }
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(2)));
    return std::nullopt;
  }

private:
  std::shared_ptr<XizhongVersionCapture> capture_;
  std::uint32_t response_id_{};
  bool opened_{};
  bool request_seen_{};
  bool response_sent_{};
};

void test_version_check_service_success() {
  const auto directory =
      std::filesystem::temp_directory_path() / "uds_version_check_test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto profile_path = directory / "profile.ini";
  {
    std::ofstream output(profile_path);
    output << "[version_check]\n"
              "session=0\n"
               "item_count=1\n"
               "item_0_name=SoftwareVersion\n"
               "item_0_request=22 F1 89\n"
               "item_0_decoder=ascii_trim\n"
              "item_0_required=true\n";
  }
  uds::app::VersionCheckRequest request;
  request.profile_path = profile_path;
  request.profile.can_fd = false;
  request.profile.padding = 0;
  request.profile.isotp_st_min = 0;
  request.profile.nominal_bitrate = 500000;
  request.profile.vendor_name = L"测试厂商";
  request.profile.project_name = L"测试项目";
  request.profile.device_name = L"测试设备";
  request.channel = 1;
  request.tx_id = 0x700;
  request.rx_id = 0x708;
  uds::app::VersionCheckService service(
      [](const uds::app::VersionCheckRequest&) {
        return std::make_unique<FakeVersionBus>();
      });
  std::vector<std::string> logs;
  uds::app::VersionCheckCallbacks callbacks;
  callbacks.onLog = [&](const std::string& line) { logs.push_back(line); };
  const auto result = service.run(request, callbacks, {});
  const auto logged = [&logs](std::string_view expected) {
    return std::any_of(logs.cbegin(), logs.cend(), [&](const auto& line) {
      return line.find(expected) != std::string::npos;
    });
  };
  const auto raw_uds_leaked = std::any_of(
      logs.cbegin(), logs.cend(), [](const auto& line) {
        return line.starts_with("TX [") || line.starts_with("RX [") ||
               line.find("请求=") != std::string::npos ||
               line.find("响应/原因=") != std::string::npos;
      });
  check(result.success && !result.cancelled && result.items.size() == 1 &&
             result.items[0].status ==
                 uds::app::VersionCheckStatus::pass &&
             result.items[0].actual == L"OK" &&
             result.items[0].expected.empty() &&
             result.items[0].response_hex == "62 F1 89 4F 4B" &&
             result.message.starts_with(
                 "版本读取完成：成功 1，失败 0，耗时 ") &&
             logged("项目：测试厂商 / 测试项目 / 测试设备") &&
             logged("通道：CH1 | TX 0x700 | RX 0x708") &&
             logged("F189 SoftwareVersion：OK") && !raw_uds_leaked,
        "version-check service did not produce concise decoded logs while retaining raw result evidence");
  std::filesystem::remove_all(directory);
}

void test_diagnostic_request_service_success() {
  uds::app::DiagnosticRequest request;
  request.profile.can_fd = false;
  request.profile.padding = 0;
  request.profile.isotp_st_min = 0;
  request.profile.nominal_bitrate = 500000;
  request.channel = 1;
  request.tx_id = 0x700;
  request.rx_id = 0x708;
  request.payload = {0x22, 0xF1, 0x89};
  request.timeout_ms = 500;
  uds::app::DiagnosticRequestService service(
      [](const uds::app::DiagnosticRequest&) {
        return std::make_unique<FakeVersionBus>();
      });
  const auto result = service.run(request, {});
  check(result.success && !result.cancelled && result.nrc == 0 &&
            result.request_hex == "22 F1 89" &&
            result.response_hex == "62 F1 89 4F 4B" &&
            result.elapsed_ms <= 500,
        "manual diagnostic request did not preserve UDS request/response");
}

void test_version_check_service_lsmr_nm_wakeup() {
  const auto directory =
      std::filesystem::temp_directory_path() / "uds_lsmr_version_check_test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto profile_path = directory / "profile.ini";
  {
    std::ofstream output(profile_path);
    output << "[version_check]\n"
              "session=0\n"
              "precondition=xizhong_nm\n"
              "item_count=1\n"
              "item_0_name=SoftwareVersion\n"
              "item_0_request=22 F1 89\n"
              "item_0_decoder=ascii_trim\n"
              "item_0_required=true\n";
  }
  auto capture = std::make_shared<XizhongVersionCapture>();
  uds::app::VersionCheckService service(
      [capture](const uds::app::VersionCheckRequest& request) {
        return std::make_unique<FakeXizhongVersionBus>(capture, request.rx_id);
      });
  uds::app::VersionCheckRequest request;
  request.profile_path = profile_path;
  request.profile.flow = L"xizhong_lsmr";
  request.profile.can_fd = true;
  request.profile.extended_id = true;
  request.profile.uds_fd = true;
  request.profile.uds_brs = true;
  request.profile.padding = 0xCC;
  request.profile.isotp_st_min = 0;
  request.profile.nominal_bitrate = 500000;
  request.profile.data_bitrate = 2000000;
  request.channel = 1;
  request.tx_id = 0x18DAB6F1;
  request.rx_id = 0x18DAF1B6;
  const auto result = service.run(request, {}, {});
  std::scoped_lock lock(capture->mutex);
  const auto nm_count = std::count_if(
      capture->sent.begin(), capture->sent.end(), [](const uds::CanFrame& frame) {
        return frame.id == 0x18FFA0B6 && frame.extended && !frame.fd &&
               !frame.brs && frame.data == std::vector<std::uint8_t>(
                                           {0, 0, 0, 0, 0, 0, 0, 0});
      });
  const auto did_request = std::find_if(
      capture->sent.begin(), capture->sent.end(), [](const uds::CanFrame& frame) {
        return frame.id == 0x18DAB6F1 && frame.extended && frame.fd && frame.brs &&
               frame.data == std::vector<std::uint8_t>(
                                 {0x03, 0x22, 0xF1, 0x89, 0xCC, 0xCC, 0xCC, 0xCC});
      });
  check(result.success && result.items.size() == 1 && nm_count >= 5 &&
            did_request != capture->sent.end(),
        "LSMR version check did not keep the LSMR NM wakeup and FD DID request");
  std::filesystem::remove_all(directory);
}

void test_version_check_service_chuneng_periodic_wakeup() {
  const auto directory = std::filesystem::temp_directory_path() /
                         "uds_chuneng_version_check_test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto profile_path = directory / "profile.ini";
  {
    std::ofstream output(profile_path);
    output << "[version_check]\n"
              "session=0x01\n"
              "precondition=chuneng_520\n"
              "item_count=1\n"
              "item_0_name=SoftwareVersion\n"
              "item_0_request=22 F1 89\n"
              "item_0_decoder=ascii_trim\n"
              "item_0_required=true\n";
  }
  auto capture = std::make_shared<ChunengVersionCapture>();
  uds::app::VersionCheckService service(
      [capture](const uds::app::VersionCheckRequest&) {
        return std::make_unique<FakeChunengVersionBus>(capture);
      });
  uds::app::VersionCheckRequest request;
  request.profile_path = profile_path;
  request.profile.can_fd = true;
  request.profile.padding = 0x55;
  request.profile.nominal_bitrate = 500000;
  request.profile.data_bitrate = 2000000;
  request.channel = 2;
  request.tx_id = 0x72E;
  request.rx_id = 0x72F;
  const auto result = service.run(request, {}, {});
  std::scoped_lock lock(capture->mutex);
  const auto wake_count = std::count_if(
      capture->sent.begin(), capture->sent.end(),
      [](const uds::CanFrame& frame) {
        return frame.id == 0x520 && !frame.extended && !frame.fd &&
               frame.data == std::vector<std::uint8_t>(8, 0);
      });
  check(result.success && wake_count >= 2,
        "Chuneng version read did not maintain periodic 0x520 wake-up");
  std::filesystem::remove_all(directory);
}

void test_version_value_decoders() {
  const std::vector<std::uint8_t> ordinary_ascii{
      'S', 'W', 'D', '.', '0', '0', '.', '7', 0x00};
  check(uds::app::decode_version_value(ordinary_ascii, L"ascii_trim") ==
             L"SWD.00.7",
        "ordinary ASCII version decoding regressed");

  const std::vector<std::uint8_t> xizhong_f180{
      'R', 'S', 'M', 'R', '_', 'A', 'A', '_', 'P', 'B', 'L', 'x', '_',
      0x09, 0x00};
  check(uds::app::decode_version_value(xizhong_f180, L"xizhong_f180") ==
            L"RSMR_AA_PBLx_ V09.00",
        "xizhong F180 structured version decoding is incorrect");

  const std::vector<std::uint8_t> xizhong_f189{
      0x03,
      'R', 'S', 'M', 'R', '_', 'A', 'A', '_', 'P', 'B', 'L', 'x', '_',
      0x09, 0x00,
      'R', 'S', 'M', 'R', '_', 'A', 'A', '_', 'S', 'B', 'L', 'x', '_',
      0x00, 0x00,
      'R', 'S', 'M', 'R', '_', 'A', 'A', '_', 'A', 'P', 'P', '1', '_',
      0x09, 0x13};
  check(uds::app::decode_version_value(xizhong_f189, L"xizhong_f189") ==
            L"PBL: RSMR_AA_PBLx_ V09.00 | "
            L"SBL: RSMR_AA_SBLx_ V00.00 | "
            L"APP: RSMR_AA_APP1_ V09.13",
        "xizhong F189 structured version decoding is incorrect");

  const std::vector<std::uint8_t> counted_ascii_24{
      0x01, 'P', '4', '1', '6', '_', 'B', 'O', 'O', 'T', '_', '0', '1',
      '.',  '0', '0', '.', '0', '7', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  check(uds::app::decode_version_value(counted_ascii_24,
                                       L"counted_ascii_24") ==
            L"模块数: 1 | Boot: P416_BOOT_01.00.07",
        "Geely F180 structured software identification is incomplete");

  const std::vector<std::uint8_t> bcd_ascii_part_number{
      0x66, 0x08, 0x46, 0x01, '3', '6', 'A'};
  check(uds::app::decode_version_value(
            bcd_ascii_part_number, L"bcd_ascii_part_7") == L"66084601 36A",
        "Geely BCD plus ASCII part number decoding is incorrect");

  const std::vector<std::uint8_t> counted_bcd_ascii_part_numbers{
      0x02, 0x66, 0x08, 0x46, 0x01, '3', '6', 'A',
      0x66, 0x08, 0x33, 0x58, '3', '1', 'B'};
  check(uds::app::decode_version_value(
            counted_bcd_ascii_part_numbers,
            L"counted_bcd_ascii_part_7") ==
            L"模块数: 2 | #1: 66084601 36A | #2: 66083358 31B",
        "Geely software part-number list decoding is incomplete");
}

class FakeChunengProbeBus final : public uds::ICanBus {
public:
  explicit FakeChunengProbeBus(std::shared_ptr<ProbeBusCapture> capture,
                               std::uint32_t response_id,
                               std::uint8_t precondition_status = 0x04)
      : capture_(std::move(capture)), response_id_(response_id),
        precondition_status_(precondition_status) {}

  void open() override {
    opened_ = true;
    capture_->opened = true;
  }
  void close() noexcept override { opened_ = false; }
  bool is_open() const noexcept override { return opened_; }
  void send(const uds::CanFrame& frame) override {
    std::scoped_lock lock(mutex_);
    capture_->sent.push_back(frame);
    if (frame.data.size() >= 3 && frame.data[0] == 0x02 &&
        frame.data[1] == 0x10) {
      pending_session_ = frame.data[2];
    } else if (frame.data.size() >= 5 && frame.data[0] == 0x04 &&
               frame.data[1] == 0x31 && frame.data[2] == 0x01 &&
               frame.data[3] == 0x02 && frame.data[4] == 0x03) {
      pending_precondition_ = true;
    }
  }
  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds timeout) override {
    {
      std::scoped_lock lock(mutex_);
      if (pending_session_) {
        const auto session = *pending_session_;
        pending_session_.reset();
        return uds::CanFrame{
            response_id_,
            {0x02, 0x50, session, 0x00, 0x00, 0x00, 0x00, 0x00},
            false, false, false};
      }
      if (pending_precondition_) {
        pending_precondition_ = false;
        if (precondition_status_ == 0x31 || precondition_status_ == 0x22) {
          return uds::CanFrame{
              response_id_,
              {0x03, 0x7F, 0x31, precondition_status_, 0x00, 0x00, 0x00,
               0x00},
              false, false, false};
        }
        return uds::CanFrame{
            response_id_,
            {0x05, 0x71, 0x01, 0x02, 0x03,
             precondition_status_,
             0x00, 0x00},
            false, false, false};
      }
    }
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(5)));
    return std::nullopt;
  }

private:
  std::shared_ptr<ProbeBusCapture> capture_;
  std::uint32_t response_id_{};
  std::optional<std::uint8_t> pending_session_;
  bool pending_precondition_{};
  std::uint8_t precondition_status_{0x04};
  bool opened_{};
  std::mutex mutex_;
};

uds::app::ProbeRequest make_probe_request() {
  uds::app::ProbeRequest request;
  request.profile.id = L"probe_test";
  request.profile.flow = L"probe_test";
  request.profile.name = L"Probe Test ECU";
  request.profile.power_control = false;
  request.profile.can_fd = false;
  request.profile.tx_id = 0x701;
  request.profile.rx_id = 0x761;
  request.channel = 2;
  request.tx_id = 0x701;
  request.rx_id = 0x761;
  request.nominal_bitrate = 500000;
  request.data_bitrate = 2000000;
  request.padding = 0x00;
  return request;
}

void test_probe_service_success() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        check(request.channel == 2 && request.nominal_bitrate == 500000,
              "probe service passed wrong bus configuration");
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, true);
      });
  std::vector<int> progress_values;
  uds::app::ProbeServiceCallbacks callbacks;
  callbacks.onProgress = [&](int value, const std::string&) {
    progress_values.push_back(value);
  };
  const auto result = service.run(make_probe_request(), callbacks, {});
  check(result.success && !result.cancelled,
        "probe service did not accept 50 01 response");
  check(capture->opened && capture->sent.size() == 1 &&
            capture->sent.front().id == 0x701 &&
            capture->sent.front().data[0] == 0x02 &&
            capture->sent.front().data[1] == 0x10 &&
            capture->sent.front().data[2] == 0x01,
        "probe service did not send the expected 10 01 request");
  check(progress_values == std::vector<int>({0, 100}),
        "probe service progress must be the binary 0/100 verdict");
}

void test_probe_service_rejects_invalid_configuration_before_bus_access() {
  int factory_calls{};
  uds::app::ProbeService service(
      [&factory_calls](const uds::app::ProbeRequest&) {
        ++factory_calls;
        return std::unique_ptr<uds::ICanBus>{};
      });

  auto invalid_mode = make_probe_request();
  invalid_mode.entry_mode = L"factory";
  const auto invalid_result = service.run(invalid_mode, {}, {});
  check(!invalid_result.success && !invalid_result.cancelled &&
            invalid_result.message == "在线探测配置无效" &&
            factory_calls == 0,
        "probe service opened CAN for an invalid entry mode");

  auto missing_ft = make_probe_request();
  missing_ft.entry_mode = L"ft";
  const auto missing_ft_result = service.run(missing_ft, {}, {});
  check(!missing_ft_result.success && !missing_ft_result.cancelled &&
            missing_ft_result.message == "FT探测端点未配置" &&
            factory_calls == 0,
        "probe service opened CAN when FT support/endpoints were absent");
}

void test_probe_service_marks_custom_endpoint_without_changing_request() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        check(request.tx_id == 0x612 && request.rx_id == 0x61A,
              "probe plan changed a user-edited custom endpoint");
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, true);
      });
  auto request = make_probe_request();
  request.tx_id = 0x612;
  request.rx_id = 0x61A;

  const auto result = service.run(request, {}, {});
  check(result.success && !result.cancelled &&
            result.message.find("自定义端点在线：响应 50 01") == 0 &&
            capture->sent.size() == 1 &&
            capture->sent.front().id == 0x612,
        "probe service lost custom-endpoint identity or request routing");
}

void test_probe_plan_preserves_entry_specific_session_and_ft_target() {
  auto generic = uds::app::probe_detail::resolve_probe_plan(make_probe_request());
  check(generic.valid && generic.plan.session == 0x01 &&
            generic.plan.probe_tx_id == 0x701 &&
            generic.plan.attempt_count == 1,
        "generic probe plan no longer uses one safe 10 01 attempt");

  auto arc331_request = make_probe_request();
  arc331_request.profile.flow = L"chuneng_arc331";
  arc331_request.entry_mode = L"boot";
  auto arc331 = uds::app::probe_detail::resolve_probe_plan(arc331_request);
  check(arc331.valid && arc331.plan.boot_probe &&
            arc331.plan.session == 0x03 && arc331.plan.chuneng_arc331,
        "ARC331 BOOT probe plan lost its safe extended-session routing");

  auto ft_request = make_probe_request();
  ft_request.entry_mode = L"ft";
  ft_request.profile.supports_ft_entry = true;
  ft_request.profile.ft_tx_id = 0x715;
  ft_request.profile.ft_rx_id = 0x71D;
  ft_request.profile.targets = {
      {L"secondary", L"Secondary", 0x760, 0x768, false, 0, {}, {}, {}, {},
       {}, {}, {}, 0x714, 0x71C}};
  ft_request.tx_id = 0x760;
  ft_request.rx_id = 0x768;
  auto ft = uds::app::probe_detail::resolve_probe_plan(ft_request);
  check(ft.valid && ft.plan.ft_probe && ft.plan.session == 0x03 &&
            ft.plan.request.tx_id == 0x714 &&
            ft.plan.request.rx_id == 0x71C,
        "probe plan ignored the selected target's FT endpoint");
}

void test_probe_service_ft_target_endpoint() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, true);
      });
  auto request = make_probe_request();
  request.entry_mode = L"ft";
  request.profile.supports_ft_entry = true;
  request.profile.ft_tx_id = 0x715;
  request.profile.ft_rx_id = 0x71D;
  request.profile.ft_padding = 0x00;
  request.profile.targets = {
      {L"main", L"Main", 0x744, 0x74C, false, 0, {}, {}, {}, {}, {}, {}, {},
       0x715, 0x71D},
      {L"secondary", L"Secondary", 0x760, 0x768, false, 0, {}, {}, {}, {}, {},
       {}, {}, 0x714, 0x71C}};
  request.tx_id = 0x760;
  request.rx_id = 0x768;

  const auto result = service.run(request, {}, {});
  check(result.success,
        "FT probe did not accept the selected target 50 03 response");
  check(capture->sent.size() == 1 &&
            capture->sent.front().id == 0x714 &&
            capture->sent.front().data[0] == 0x02 &&
            capture->sent.front().data[1] == 0x10 &&
            capture->sent.front().data[2] == 0x03,
        "FT probe did not resolve the target FT endpoint and send 10 03");
}

void test_probe_service_shidaixinan_ft_endpoint_and_wakeup() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeProbeBus>(
            capture, request.rx_id, true);
      });
  auto request = make_probe_request();
  request.entry_mode = L"ft";
  request.profile.id = L"shidaixinan_hjzj_fmr";
  request.profile.flow = L"shidaixinan_hjzj_fmr";
  request.profile.name = L"时代新安 HJZJ_FMR";
  request.profile.can_fd = true;
  request.profile.uds_fd = true;
  request.profile.uds_brs = false;
  request.profile.supports_ft_entry = true;
  request.profile.tx_id = request.tx_id = 0x7A4;
  request.profile.rx_id = request.rx_id = 0x7AC;
  request.profile.functional_id = 0x7DF;
  request.profile.ft_tx_id =
      uds::kShidaixinanHjzjFtFunctionalTxId;
  request.profile.ft_rx_id =
      uds::kShidaixinanHjzjFtFunctionalRxId;
  request.profile.ft_uds_fd = true;
  request.profile.ft_uds_brs = false;
  request.profile.ft_padding = 0x00;

  const auto result = service.run(request, {}, {});
  const auto wakeup = std::find_if(
      capture->sent.begin(), capture->sent.end(),
      [](const auto& frame) {
        return frame.id == 0x425 && frame.fd && frame.brs &&
               frame.data == std::vector<std::uint8_t>(8, 0x00);
      });
  const auto ft_session = std::find_if(
      capture->sent.begin(), capture->sent.end(),
      [](const auto& frame) {
        return frame.id == 0x7DF && frame.fd && !frame.brs &&
               frame.data.size() >= 3U &&
               frame.data[0] == 0x02 &&
               frame.data[1] == 0x10 &&
               frame.data[2] == 0x03;
      });
  check(result.success &&
            wakeup != capture->sent.end() &&
            ft_session != capture->sent.end(),
        "Shidaixinan FT probe did not keep 0x425 wake-up and use 7DF->761 10 03");
}

void test_probe_service_xizhong_nm_wakeup_and_retry() {
  const auto verify = [](std::wstring_view flow, std::uint32_t tx_id,
                         std::uint32_t rx_id, std::uint32_t nm_id) {
    auto capture = std::make_shared<ProbeBusCapture>();
    uds::app::ProbeService service(
        [capture](const uds::app::ProbeRequest& request) {
          return std::make_unique<FakeProbeBus>(capture, request.rx_id, true,
                                                 false, 2, 1);
        });
    auto request = make_probe_request();
    request.profile.id = std::wstring(flow);
    request.profile.flow = std::wstring(flow);
    request.profile.can_fd = true;
    request.profile.extended_id = true;
    request.profile.uds_fd = true;
    request.profile.uds_brs = true;
    request.profile.tx_id = request.tx_id = tx_id;
    request.profile.rx_id = request.rx_id = rx_id;
    request.profile.functional_id = 0x18DBFFF1;
    request.profile.padding = request.padding = 0xCC;

    std::vector<int> progress_values;
    uds::app::ProbeServiceCallbacks callbacks;
    callbacks.onProgress = [&](int value, const std::string&) {
      progress_values.push_back(value);
    };
    const auto result = service.run(request, callbacks, {});
    check(result.success && progress_values == std::vector<int>({0, 100}),
          "Xizhong probe did not retry to a binary successful verdict");
    const auto nm_count = std::count_if(
        capture->sent.begin(), capture->sent.end(),
        [nm_id](const uds::CanFrame& frame) { return frame.id == nm_id; });
    std::vector<uds::CanFrame> uds_requests;
    std::copy_if(capture->sent.begin(), capture->sent.end(),
                 std::back_inserter(uds_requests), [tx_id](const uds::CanFrame& frame) {
                   return frame.id == tx_id && frame.data.size() >= 3 &&
                          frame.data[1] == 0x10 && frame.data[2] == 0x01;
                 });
    check(nm_count >= 5 && uds_requests.size() == 2,
          "Xizhong probe did not establish project NM wakeup and retry 10 01");
    check(std::all_of(uds_requests.begin(), uds_requests.end(),
                      [](const uds::CanFrame& frame) {
                        return frame.extended && frame.fd && frame.brs &&
                               frame.data == std::vector<std::uint8_t>(
                                                 {0x02, 0x10, 0x01, 0xCC, 0xCC,
                                                  0xCC, 0xCC, 0xCC});
                      }),
          "Xizhong physical probe frame format is not the passing FD+BRS baseline");
  };
  verify(L"xizhong_rsmr", 0x18DAB7F1, 0x18DAF1B7, 0x18FFA025);
  verify(L"xizhong_lsmr", 0x18DAB6F1, 0x18DAF1B6, 0x18FFA0B6);
}

void test_probe_service_chery_preconditions() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, true);
      });
  auto request = make_probe_request();
  request.profile.flow = L"chery_ars1_33";
  request.profile.tx_id = request.tx_id = 0x6C4;
  request.profile.rx_id = request.rx_id = 0x6C5;
  request.padding = 0x55;

  const auto result = service.run(request, {}, {});
  check(result.success, "Chery probe with precondition frames failed");
  const auto sent_id = [&](std::uint32_t id) {
    return std::any_of(capture->sent.begin(), capture->sent.end(),
                       [id](const auto& frame) { return frame.id == id; });
  };
  const auto wakeup_count = std::count_if(
      capture->sent.begin(), capture->sent.end(),
      [](const uds::CanFrame& frame) {
        return frame.id == 0x600 && !frame.extended && !frame.fd &&
               !frame.brs &&
               frame.data == std::vector<std::uint8_t>(
                                 {0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00});
      });
  check(wakeup_count >= 9 && sent_id(0x25B) && sent_id(0x4B4) &&
            sent_id(0x6C4),
        "Chery flashability probe did not maintain 0x600 wake-up and "
        "vehicle-state frames before UDS 10 01");
}

void test_probe_service_chuneng_arc331_selected_endpoint_online() {
  struct Endpoint {
    std::uint32_t tx_id;
    std::uint32_t rx_id;
  };
  for (const auto endpoint :
       {Endpoint{0x72C, 0x72D}, Endpoint{0x72E, 0x72F}}) {
    auto capture = std::make_shared<ProbeBusCapture>();
    uds::app::ProbeService service(
        [capture](const uds::app::ProbeRequest& request) {
          return std::make_unique<FakeChunengProbeBus>(capture,
                                                       request.rx_id);
        });
    auto request = make_probe_request();
    request.profile.flow = L"chuneng_arc331";
    request.profile.tx_id = request.tx_id = endpoint.tx_id;
    request.profile.rx_id = request.rx_id = endpoint.rx_id;
    request.profile.functional_id = 0x7DF;
    request.padding = 0x55;

    const auto result = service.run(request, {}, {});
    const auto wakeup_count = std::count_if(
        capture->sent.cbegin(), capture->sent.cend(),
        [](const uds::CanFrame& frame) {
          return frame.id == 0x520 && !frame.extended && !frame.fd &&
                 !frame.brs &&
                 frame.data == std::vector<std::uint8_t>(8, 0x00);
        });
    const auto session_request = std::find_if(
        capture->sent.cbegin(), capture->sent.cend(),
        [endpoint](const uds::CanFrame& frame) {
          return frame.id == endpoint.tx_id && !frame.extended && !frame.fd &&
                 !frame.brs && frame.data.size() == 8 &&
                 frame.data[0] == 0x02 && frame.data[1] == 0x10 &&
                 frame.data[2] == 0x03;
        });
    const auto precondition_request = std::find_if(
        capture->sent.cbegin(), capture->sent.cend(),
        [](const uds::CanFrame& frame) {
          return frame.data.size() >= 5 && frame.data[1] == 0x31 &&
                 frame.data[2] == 0x01 && frame.data[3] == 0x02 &&
                 frame.data[4] == 0x03;
        });
    const auto programming_session_request = std::find_if(
        capture->sent.cbegin(), capture->sent.cend(),
        [](const uds::CanFrame& frame) {
          return frame.data.size() >= 3 && frame.data[1] == 0x10 &&
                 frame.data[2] == 0x02;
        });
    check(result.success && wakeup_count >= 1 &&
               session_request != capture->sent.cend() &&
               precondition_request != capture->sent.cend() &&
               programming_session_request == capture->sent.cend(),
          "ARC331 APP probe did not check 31 01 02 03 safely for both "
          "selected radar endpoints");
  }
}

void test_probe_service_chuneng_arc331_boot_probe_is_non_intrusive() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeChunengProbeBus>(capture, request.rx_id);
      });
  auto request = make_probe_request();
  request.profile.flow = L"chuneng_arc331";
  request.profile.tx_id = request.tx_id = 0x72E;
  request.profile.rx_id = request.rx_id = 0x72F;
  request.profile.functional_id = 0x7DF;
  request.entry_mode = L"boot";
  request.padding = 0x55;

  const auto result = service.run(request, {}, {});
  const auto unsafe_request = std::find_if(
      capture->sent.cbegin(), capture->sent.cend(),
      [](const uds::CanFrame& frame) {
        return frame.data.size() >= 3 &&
               (frame.data[1] == 0x31 ||
                (frame.data[1] == 0x10 && frame.data[2] == 0x02));
      });
  check(result.success && unsafe_request == capture->sent.cend(),
        "ARC331 Boot probe sent a precondition or programming-session request");
}

void test_probe_service_chuneng_arc331_nrc31_warns_and_continues() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeChunengProbeBus>(capture, request.rx_id,
                                                     std::uint8_t{0x31});
      });
  auto request = make_probe_request();
  request.profile.flow = L"chuneng_arc331";
  request.profile.tx_id = request.tx_id = 0x72E;
  request.profile.rx_id = request.rx_id = 0x72F;
  request.profile.functional_id = 0x7DF;
  request.entry_mode = L"app";
  request.padding = 0x55;

  std::vector<std::string> logs;
  uds::app::ProbeServiceCallbacks callbacks;
  callbacks.onLog = [&](const std::string& line) { logs.push_back(line); };
  const auto result = service.run(request, callbacks, {});
  const auto warned = std::any_of(logs.cbegin(), logs.cend(), [](const auto& line) {
    return line.find("7F 31 31") != std::string::npos &&
           line.find("容错继续") != std::string::npos;
  });
  check(result.success && warned &&
            result.message.find("7F 31 31") != std::string::npos &&
            result.message.find("容错继续") != std::string::npos,
        "ARC331 NRC 0x31 was not retained as a warning/continue result");
}

void test_probe_service_chuneng_arc331_other_nrc_still_fails() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeChunengProbeBus>(capture, request.rx_id,
                                                     std::uint8_t{0x22});
      });
  auto request = make_probe_request();
  request.profile.flow = L"chuneng_arc331";
  request.profile.tx_id = request.tx_id = 0x72E;
  request.profile.rx_id = request.rx_id = 0x72F;
  request.profile.functional_id = 0x7DF;
  request.entry_mode = L"app";
  request.padding = 0x55;

  const auto result = service.run(request, {}, {});
  check(!result.success,
        "ARC331 incorrectly tolerated a precondition NRC other than 0x31");
}

void test_probe_service_lingpao_radar_entry_sequences() {
  struct Project {
    std::wstring id;
    std::uint32_t tx_id;
    std::uint32_t rx_id;
  };
  for (const auto& project :
       {Project{L"lp_arc", 0x772, 0x77A},
        Project{L"lp_arf", 0x751, 0x759}}) {
    for (const auto entry :
         {std::wstring{L"app"}, std::wstring{L"ft"}}) {
    auto capture = std::make_shared<ProbeBusCapture>();
    uds::app::ProbeService service(
        [capture](const uds::app::ProbeRequest& request) {
          return std::make_unique<FakeChunengProbeBus>(
              capture, request.rx_id);
        });
    auto request = make_probe_request();
    request.profile.id = project.id;
    request.profile.flow = project.id;
    request.profile.name = project.id == L"lp_arc" ? L"LP-ARC" : L"LP-ARF";
    request.profile.tx_id = request.tx_id = project.tx_id;
    request.profile.rx_id = request.rx_id = project.rx_id;
    request.profile.functional_id = 0x7DF;
    request.profile.supports_ft_entry = true;
    request.profile.ft_tx_id = 0x701;
    request.profile.ft_rx_id = 0x761;
    request.profile.ft_padding = 0x55;
    request.entry_mode = entry;
    request.padding = 0x55;

    const auto result = service.run(request, {}, {});
    check(result.success,
          "Leapmotor radar read-only APP/PLS probe sequence failed");
    check(capture->sent.size() == 2 &&
              capture->sent[0].id == 0x7DF &&
              capture->sent[0].data[1] == 0x10 &&
              capture->sent[0].data[2] == 0x01 &&
              capture->sent[1].id == 0x7DF &&
              capture->sent[1].data[1] == 0x10 &&
              capture->sent[1].data[2] == 0x03,
          "Leapmotor radar probe did not use functional 10 01 then 10 03");
    check(std::none_of(
              capture->sent.begin(), capture->sent.end(),
              [](const auto& frame) {
                return frame.data.size() >= 3 &&
                       frame.data[1] == 0x10 &&
                       frame.data[2] == 0x02;
              }),
          "Leapmotor radar read-only probe entered ProgrammingSession");
    }
  }
}

void test_probe_service_geely_p416_entry_wakeup() {
  for (const auto entry :
       {std::wstring{L"app"}, std::wstring{L"ft"}}) {
    auto capture = std::make_shared<ProbeBusCapture>();
    uds::app::ProbeService service(
        [capture](const uds::app::ProbeRequest& request) {
          return std::make_unique<FakeProbeBus>(capture, request.rx_id, true);
        });
    auto request = make_probe_request();
    request.profile.id = L"geely_p416";
    request.profile.flow = L"geely_p416";
    request.profile.name = L"Geely P416 ARS1.31L";
    request.profile.can_fd = true;
    request.profile.uds_fd = false;
    request.profile.uds_brs = false;
    request.profile.tx_id = request.tx_id = 0x716;
    request.profile.rx_id = request.rx_id = 0x616;
    request.profile.functional_id = 0x7FF;
    request.profile.supports_ft_entry = true;
    request.profile.ft_tx_id = 0x701;
    request.profile.ft_rx_id = 0x761;
    request.profile.ft_padding = 0x55;
    request.entry_mode = entry;
    request.padding = 0x55;

    const auto result = service.run(request, {}, {});
    const auto wakeup = std::find_if(
        capture->sent.begin(), capture->sent.end(),
        [](const uds::CanFrame& frame) {
          return frame.id == uds::kGeelyP416NmId && !frame.extended &&
                 !frame.fd && !frame.brs &&
                 frame.data == std::vector<std::uint8_t>(
                                   {0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF});
        });
    const auto expected_tx = entry == L"ft" ? 0x701U : 0x716U;
    const auto safe_session = std::find_if(
        capture->sent.begin(), capture->sent.end(),
        [expected_tx](const uds::CanFrame& frame) {
          return frame.id == expected_tx && frame.data.size() >= 3U &&
                 frame.data[0] == 0x02 && frame.data[1] == 0x10 &&
                 frame.data[2] == 0x01;
        });
    const auto entered_programming = std::any_of(
        capture->sent.begin(), capture->sent.end(),
        [](const uds::CanFrame& frame) {
          return frame.data.size() >= 3U && frame.data[1] == 0x10 &&
                 frame.data[2] == 0x02;
        });
    check(result.success && wakeup != capture->sent.end() &&
              safe_session != capture->sent.end() && !entered_programming,
          "Geely P416 APP/PLS probe did not keep 0x53F wake and use safe 10 01");
  }
}

void test_probe_service_unexpected_response() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, true,
                                               true);
      });
  const auto result = service.run(make_probe_request(), {}, {});
  check(!result.success && !result.cancelled &&
            result.message == "设备响应无效",
        "probe service accepted an unexpected UDS response");
}

void test_probe_controller_success() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, true);
      });
  uds::app::OperationState state;
  uds::app::ProbeController controller(state, std::move(service));
  std::promise<uds::app::ProbeResult> completion;
  auto completed = completion.get_future();
  int log_count = 0;
  int progress = -1;
  uds::app::ProbeControllerCallbacks callbacks;
  callbacks.onLog = [&](const std::string&) { ++log_count; };
  callbacks.onProgress = [&](int value, const std::string&) {
    progress = value;
  };
  callbacks.onFinished = [&](uds::app::ProbeResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(make_probe_request(), std::move(callbacks)),
        "probe controller rejected a valid start");
  const auto result = completed.get();
  controller.wait();
  check(result.success && log_count > 0 && progress == 100,
        "probe controller did not forward service callbacks");
  check(!state.is_active(), "probe controller did not release operation state");
}

void test_probe_controller_stop() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeProbeBus>(capture, request.rx_id, false);
      });
  uds::app::OperationState state;
  uds::app::ProbeController controller(state, std::move(service));
  std::promise<uds::app::ProbeResult> completion;
  auto completed = completion.get_future();
  uds::app::ProbeControllerCallbacks callbacks;
  callbacks.onFinished = [&](uds::app::ProbeResult result) {
    completion.set_value(std::move(result));
  };
  check(controller.start(make_probe_request(), std::move(callbacks)),
        "probe controller rejected stop-path start");
  while (!capture->opened.load()) std::this_thread::yield();
  check(controller.request_stop(), "probe controller rejected stop request");
  const auto result = completed.get();
  controller.wait();
  check(!result.success && result.cancelled,
        "probe controller did not report cancellation");
  check(!state.is_active(),
        "probe controller did not release state after cancellation");
}

void test_probe_controller_stop_during_periodic_wakeup() {
  auto capture = std::make_shared<ProbeBusCapture>();
  uds::app::ProbeService service(
      [capture](const uds::app::ProbeRequest& request) {
        return std::make_unique<FakeChunengProbeBus>(capture, request.rx_id);
      });
  uds::app::OperationState state;
  uds::app::ProbeController controller(state, std::move(service));
  std::promise<uds::app::ProbeResult> completion;
  auto completed = completion.get_future();
  uds::app::ProbeControllerCallbacks callbacks;
  callbacks.onFinished = [&](uds::app::ProbeResult result) {
    completion.set_value(std::move(result));
  };

  auto request = make_probe_request();
  request.profile.flow = L"chuneng_arc331";
  request.profile.tx_id = request.tx_id = 0x72E;
  request.profile.rx_id = request.rx_id = 0x72F;
  request.entry_mode = L"boot";
  request.padding = 0x55;
  check(controller.start(request, std::move(callbacks)),
        "periodic-wakeup cancellation probe did not start");
  while (!capture->opened.load()) std::this_thread::yield();
  check(controller.request_stop(),
        "periodic-wakeup cancellation request was rejected");
  const auto result = completed.get();
  controller.wait();
  const auto unsafe_request = std::find_if(
      capture->sent.cbegin(), capture->sent.cend(),
      [](const uds::CanFrame& frame) {
        return frame.data.size() >= 3 &&
               (frame.data[1] == 0x31 ||
                (frame.data[1] == 0x10 && frame.data[2] == 0x02));
      });
  check(!result.success && result.cancelled && !state.is_active() &&
            unsafe_request == capture->sent.cend(),
        "periodic wake-up cancellation leaked work or sent an unsafe request");
}

} // namespace

int main() {
  try {
    test_flash_request_defaults();
    test_chuneng_ft_entry_configuration();
    test_operation_state_transitions();
    test_operation_state_concurrent_start();
    test_operation_state_rejects_stale_completion();
    test_operation_state_id_wrap_skips_reserved_zero();
    test_operation_callbacks();
    test_flash_controller_success();
    test_flash_controller_exception_conversion();
    test_flash_controller_repeat_count();
    test_flash_controller_repeat_count_maximum();
    test_flash_controller_repeat_count_boundaries();
    test_flash_controller_repeat_stops_on_first_failure();
    test_flash_controller_stop();
    test_probe_service_success();
    test_probe_service_rejects_invalid_configuration_before_bus_access();
    test_probe_service_marks_custom_endpoint_without_changing_request();
    test_probe_plan_preserves_entry_specific_session_and_ft_target();
    test_probe_service_ft_target_endpoint();
    test_probe_service_shidaixinan_ft_endpoint_and_wakeup();
    test_probe_service_xizhong_nm_wakeup_and_retry();
    test_probe_service_chery_preconditions();
    test_probe_service_chuneng_arc331_selected_endpoint_online();
    test_probe_service_chuneng_arc331_boot_probe_is_non_intrusive();
    test_probe_service_chuneng_arc331_nrc31_warns_and_continues();
    test_probe_service_chuneng_arc331_other_nrc_still_fails();
    test_probe_service_lingpao_radar_entry_sequences();
    test_probe_service_geely_p416_entry_wakeup();
    test_probe_service_unexpected_response();
    test_probe_controller_success();
    test_probe_controller_stop();
    test_probe_controller_stop_during_periodic_wakeup();
    test_version_check_service_success();
    test_diagnostic_request_service_success();
    test_version_check_service_lsmr_nm_wakeup();
    test_version_check_service_chuneng_periodic_wakeup();
    test_version_value_decoders();
    std::cout << "app state tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "app state tests failed: " << error.what() << '\n';
    return 1;
  }
}
