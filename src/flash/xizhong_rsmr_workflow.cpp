#include "flash/xizhong_rsmr_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/sha256.hpp"
#include "core/uds_client.hpp"
#include "flash/xizhong_rsmr_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace uds {
using namespace std::chrono_literals;
namespace {

struct XizhongRadarSpec {
  std::wstring_view workflow_id;
  std::string_view report_target;
  std::string_view identity;
  std::uint32_t tx_id;
  std::uint32_t rx_id;
  std::uint32_t ft_tx_id;
  std::uint32_t ft_rx_id;
  std::uint32_t nm_id;
  std::array<std::uint8_t, 4> known_key;
  bool supports_ft_entry;
};

constexpr XizhongRadarSpec kRsmrSpec{
    L"xizhong_rsmr", "RSMR", "RSMR_AA", 0x18DAB7F1, 0x18DAF1B7,
    0x701, 0x761, 0x18FFA025, {0x29, 0x98, 0x42, 0x58}, true};
constexpr XizhongRadarSpec kLsmrSpec{
    L"xizhong_lsmr", "LSMR", "LSMR_AA", 0x18DAB6F1, 0x18DAF1B6,
    0x714, 0x71C, 0x18FFA0B6, {0x2A, 0x98, 0x42, 0x58}, false};

const XizhongRadarSpec& radar_spec(XizhongRadarTarget target) noexcept {
  return target == XizhongRadarTarget::lsmr ? kLsmrSpec : kRsmrSpec;
}

struct TesterPresentState {
  mutable std::mutex mutex;
  std::string error;
};

void set_tester_present_error(TesterPresentState& state,
                              const std::string& detail) {
  std::scoped_lock lock(state.mutex);
  state.error = detail;
}

void check_tester_present(const TesterPresentState& state) {
  std::scoped_lock lock(state.mutex);
  if (!state.error.empty()) {
    throw std::runtime_error("犀重 NM/TesterPresent keepalive sender failed: " +
                             state.error);
  }
}

void report(const FlashWorkflowCallbacks& callbacks, int percent,
            std::string step, std::string verdict, std::string detail) {
  static_cast<void>(percent);
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

} // namespace

bool xizhong_rsmr_report_line(std::string_view line) noexcept {
  // The report is a verdict summary, while the live execution log retains
  // requests, waits and per-percent TransferData progress.
  return !line.starts_with("36 ") &&
         (line.find("PASS") != std::string_view::npos ||
          line.find("WARN") != std::string_view::npos);
}

XizhongRadarWorkflow::XizhongRadarWorkflow(
    XizhongRadarTarget target) noexcept
    : target_(target) {}

std::wstring_view XizhongRadarWorkflow::id() const noexcept {
  return radar_spec(target_).workflow_id;
}

std::string XizhongRadarWorkflow::report_title(const FlashProfile&) const {
  return "Xizhong " + std::string(radar_spec(target_).report_target) +
         " Download Report";
}

void XizhongRadarWorkflow::run(const FlashJob& job,
                               const FlashWorkflowCallbacks& callbacks,
                               std::stop_token stop) {
  const auto& spec = radar_spec(target_);
  if (job.profile.flow != spec.workflow_id) {
    throw std::runtime_error(
        "犀重Profile与RSMR/LSMR Workflow目标不匹配，尚未访问总线");
  }
  if (!job.profile.can_fd || !job.profile.extended_id || !job.profile.uds_fd ||
      !job.profile.uds_brs) {
    throw std::runtime_error("犀重要求29位CAN FD+BRS诊断配置");
  }
  if (job.profile.tx_id != spec.tx_id || job.profile.rx_id != spec.rx_id ||
      job.profile.functional_id != 0x18DBFFF1 ||
      job.profile.ft_tx_id != spec.ft_tx_id ||
      job.profile.ft_rx_id != spec.ft_rx_id) {
    throw std::runtime_error(
        "犀重RSMR/LSMR诊断端点与CANoe目标配置不匹配，尚未访问总线");
  }
  if (job.profile.supports_ft_entry != spec.supports_ft_entry) {
    throw std::runtime_error(
        "犀重RSMR/LSMR产线入口能力与CANoe有效分支不匹配，尚未访问总线");
  }
  if (job.profile.nominal_bitrate != 500000 ||
      job.profile.data_bitrate != 2000000 || job.profile.padding != 0xCC ||
      job.profile.isotp_st_min != 0) {
    throw std::runtime_error(
        "犀重成功基线要求500k/2M、物理请求padding=0xCC、BS=0、STmin=0");
  }
  if (job.profile.security_level != 0x11 ||
      !job.profile.security_variant.empty()) {
    throw std::runtime_error(
         "犀重 LSMR/RSMR GenerateKeyEx 要求level=0x11且variant为空");
  }
  if (job.entry_mode != L"app" && job.entry_mode != L"ft") {
    throw std::runtime_error(
        "犀重只允许APP或已声明的FT入口，尚未访问总线");
  }
  if (job.entry_mode == L"ft") {
    if (!spec.supports_ft_entry) {
      throw std::runtime_error(
          "犀重CANoe的LSMR/RaderID=1下载分支为空，禁止推断执行FT刷写");
    }
    if (callbacks.log) {
      callbacks.log("警告：本次2026-07-22成功基线只验证APP入口；FT入口尚未实刷验收。");
    }
    report(callbacks, 0, "Entry mode", "WARN",
           "FT recovery entry selected; not proven by the 2026-07-22 APP pass");
  }
  if (job.driver_file.empty() || job.app_file.empty() ||
      job.app_verify_file.empty()) {
    throw std::runtime_error(
        target_ == XizhongRadarTarget::lsmr
            ? "犀重LSMR源工程没有可复刻下载分支和固件默认值；必须手动选择同一LSMR发布包的Driver、APP与Hash，尚未访问总线"
            : "犀重RSMR必须提供Driver、APP与Hash，尚未访问总线");
  }

  XizhongRsmrImages images;
  images.driver = load_srecord_window(job.driver_file, 0x00080000, 0x400);
  images.app = load_srecord_window(job.app_file, 0x000C0000, 0x300000);
  images.app_hash = load_srecord_window(job.app_verify_file, 0x00000000, 0x20);
  const auto driver_hash = sha256(images.driver);
  if (driver_hash != kXizhongDriverHash) {
    throw std::runtime_error(
        "犀重Driver数据窗口SHA-256与Flash.can固定校验值不匹配，尚未访问总线");
  }
  const auto app_hash = sha256(images.app);
  if (!std::equal(app_hash.begin(), app_hash.end(), images.app_hash.begin(),
                  images.app_hash.end())) {
    throw std::runtime_error(
        "犀重APP数据窗口SHA-256与所选Hash文件不匹配，尚未访问总线");
  }

  const auto broker = job.executable_directory / L"keygen_broker.exe";
  auto keygen = [broker, dll = job.security_dll](
                    std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll, seed, 0x11, L"");
  };
  constexpr std::array<std::uint8_t, 4> kKnownSeed{0xFD, 0xBA, 0xAF, 0x18};
  std::vector<std::uint8_t> known_key;
  try {
    known_key = keygen(kKnownSeed);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("犀重SeedKey DLL/broker预检失败，尚未访问总线: ") +
        error.what());
  }
  if (known_key.size() != spec.known_key.size() ||
      !std::equal(spec.known_key.begin(), spec.known_key.end(),
                  known_key.begin(), known_key.end())) {
    throw std::runtime_error(
        "犀重SeedKey DLL/broker已运行，但LSMR/RSMR项目对应的已知向量不匹配");
  }
  report(callbacks, 0, "Preflight", "PASS",
         "Driver/APP SHA-256 matches verification data; project-specific SeedKey KAT passed before bus access");
  if (callbacks.log) {
    callbacks.log("犀重预检通过：Driver/APP数据窗口SHA-256、Hash及SeedKey已知向量均匹配，尚未改变ECU状态。");
  }

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, true, L"UDSToolCpp"});
  IsoTpConfig app_config{
      job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
      job.profile.isotp_st_min, 1000ms, 1000ms, true, true, true, true};
  app_config.adapt_consecutive_frames_to_flow_control = true;
  app_config.adapt_flow_control_to_first_frame = true;
  app_config.flow_control_delay = kXizhongFlowControlDelay;
  app_config.drain_receive_before_send = true;
  IsoTpSession app_physical_transport(
      *bus, app_config);
  auto functional_config = app_config;
  functional_config.tx_id = job.profile.functional_id;
  // The passing BLF uses 0x00 padding for functional SF requests while the
  // physical APP endpoint uses 0xCC.
  functional_config.padding = kXizhongFunctionalPadding;
  IsoTpSession app_functional_transport(
      *bus, functional_config);
  IsoTpSession ft_transport(
      *bus, {job.profile.ft_tx_id, job.profile.ft_rx_id, job.profile.ft_padding, 0,
            job.profile.isotp_st_min, 1000ms, 1000ms, false, false, false, false});
  auto log = [&](int percent, const std::string& line) {
    if (callbacks.log) callbacks.log(line);
    const auto transfer_progress =
        line.starts_with("36 ") &&
        line.find(" progress:") != std::string::npos;
    if (callbacks.progress && !transfer_progress) {
      callbacks.progress(percent, line);
    }
    // Keep per-percent 0x36 progress in the live log, but collapse it to the
    // explicit TransferData summary row in the HTML report.
    if (callbacks.report && xizhong_rsmr_report_line(line)) {
      const auto verdict = line.find("PASS") != std::string::npos
                               ? "PASS"
                               : (line.find("WARN") != std::string::npos
                                      ? "WARN"
                                      : "INFO");
      callbacks.report(line, verdict, line);
    }
  };
  auto progress = [&](int percent, const std::string& line) {
    if (callbacks.progress) callbacks.progress(percent, line);
  };
  UdsClient app_physical(
      app_physical_transport,
      [&](const std::string& line) { if (callbacks.log) callbacks.log(line); },
      stop);
  UdsClient app_functional(
      app_functional_transport,
      [&](const std::string& line) { if (callbacks.log) callbacks.log(line); },
      stop);
  UdsClient ft_physical(
      ft_transport,
      [&](const std::string& line) { if (callbacks.log) callbacks.log(line); },
      stop);

  TesterPresentState tester_present_state;
  const auto tester_present_frames =
      xizhong_tester_present_frames(job.profile.functional_id);
  const auto nm_wakeup_frame = xizhong_nm_wakeup_frame(spec.nm_id);
  // CANoe's passing environment keeps NM_ICG active before and throughout the
  // test. Start the same stream and allow one second of wakeup settling before
  // the first DID request, matching the already-running CAN IG precondition.
  std::string last_nm_error;
  bool nm_transmitted{};
  for (std::size_t attempt = 1;
       attempt <= kXizhongNmMaxConsecutiveFailures; ++attempt) {
    if (stop.stop_requested()) throw std::runtime_error("刷写已由用户停止");
    try {
      bus->send(nm_wakeup_frame);
      nm_transmitted = true;
      if (attempt > 1 && callbacks.log) {
        callbacks.log("犀重刷写网络唤醒：首帧NM无ACK后继续发送，第" +
                      std::to_string(attempt) + "帧已获得ACK。");
      }
      break;
    } catch (const std::exception& error) {
      last_nm_error = error.what();
    } catch (...) {
      last_nm_error = "unknown CAN NM transmit error";
    }
    if (attempt < kXizhongNmMaxConsecutiveFailures) {
      if (callbacks.log) {
        callbacks.log("犀重刷写网络唤醒：NM第" +
                      std::to_string(attempt) +
                      "帧暂未获得ACK，200ms后继续发送。");
      }
      std::this_thread::sleep_for(kXizhongNmPeriod);
    }
  }
  if (!nm_transmitted) {
    throw std::runtime_error(
        "犀重NM连续" +
        std::to_string(kXizhongNmMaxConsecutiveFailures) +
        "帧发送失败: " + last_nm_error);
  }
  if (callbacks.log) {
    callbacks.log(
        "犀重刷写网络唤醒：已发送NM_ICG全零扩展帧；"
        "后续每200ms持续发送至完整恢复序列结束。");
  }
  report(callbacks, 0, "NetworkManagement", "INFO",
         "NM_ICG project-specific extended Classic CAN frame, DLC=8, data=00 00 00 00 00 00 00 00, period=200 ms, active for the complete flash flow");
  std::jthread tester_present_sender(
      [&bus, &tester_present_state,
       &tester_present_frames, nm_wakeup_frame](std::stop_token sender_stop) {
        const auto now = std::chrono::steady_clock::now();
        auto next_nm = now + kXizhongNmPeriod;
        std::size_t consecutive_nm_failures{};
        std::array<std::chrono::steady_clock::time_point, 2> next{
            now + tester_present_frames[0].period,
            now + tester_present_frames[1].period};
        while (!sender_stop.stop_requested()) {
          const auto current = std::chrono::steady_clock::now();
          if (current >= next_nm) {
            try {
              bus->send(nm_wakeup_frame);
              consecutive_nm_failures = 0;
            } catch (const std::exception& error) {
              ++consecutive_nm_failures;
              if (consecutive_nm_failures >=
                  kXizhongNmMaxConsecutiveFailures) {
                set_tester_present_error(tester_present_state, error.what());
                return;
              }
            } catch (...) {
              ++consecutive_nm_failures;
              if (consecutive_nm_failures >=
                  kXizhongNmMaxConsecutiveFailures) {
                set_tester_present_error(
                    tester_present_state, "unknown CAN NM transmit error");
                return;
              }
            }
            do {
              next_nm += kXizhongNmPeriod;
            } while (next_nm <= current);
          }
          for (std::size_t index = 0;
               index < tester_present_frames.size(); ++index) {
            if (current < next[index]) continue;
            try {
              bus->send(tester_present_frames[index].frame);
            } catch (const std::exception& error) {
              set_tester_present_error(tester_present_state, error.what());
              return;
            } catch (...) {
              set_tester_present_error(
                  tester_present_state, "unknown CAN transmit error");
              return;
            }
            do {
              next[index] += tester_present_frames[index].period;
            } while (next[index] <= current);
          }
          std::this_thread::sleep_for(10ms);
        }
      });
  report(callbacks, 0, "TesterPresent", "INFO",
         "Passed-BLF dual functional keepalive @4s: one Classic CAN and one CAN FD+BRS 0x18DBFFF1 02 3E 80 00 00 00 00 00");

  if (callbacks.log) {
    callbacks.log(
        "犀重刷写网络唤醒：按CANoe持续NM环境预热1000ms，再发送首个22 F187。");
  }
  for (auto elapsed = 0ms; elapsed < kXizhongNmWakeupSettle;
       elapsed += 10ms) {
    if (stop.stop_requested()) {
      throw std::runtime_error("operation cancelled by user");
    }
    check_tester_present(tester_present_state);
    std::this_thread::sleep_for(10ms);
  }

  XizhongRsmrFlow flow(
      app_physical, app_functional, ft_physical, log, progress, keygen,
      std::string(spec.identity),
      [&tester_present_state]() { check_tester_present(tester_present_state); });
  const auto stop_tester_present = [&tester_present_sender]() {
    tester_present_sender.request_stop();
    if (tester_present_sender.joinable()) tester_present_sender.join();
  };
  try {
    flow.run(images, job.entry_mode, stop);
    stop_tester_present();
    check_tester_present(tester_present_state);
  } catch (...) {
    stop_tester_present();
    const std::string warning =
        "流程在最终28 00 01、85 01、10 01恢复序列全部验证前退出；"
        "ECU通信、DTC及会话恢复状态未确认。请保持供电并按报告最后成功步骤处理，"
        "不要在未知擦除/传输状态下自动发送恢复命令。";
    if (callbacks.log) callbacks.log(warning);
    report(callbacks, 0, "Recovery state", "WARN", warning);
    throw;
  }
  if (callbacks.report) {
    callbacks.report("Download", "PASS",
                     "犀重 " + std::string(spec.report_target) +
                         " 刷写流程完成");
  }
}

} // namespace uds
