#include "flash/shidaixinan_hjzj_fmr_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/high_resolution_timer.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/shidaixinan_hjzj_fmr_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace uds {
namespace {
using namespace std::chrono_literals;

struct PeriodicSenderState {
  mutable std::mutex mutex;
  std::string error;
};

void set_periodic_error(PeriodicSenderState& state,
                        const std::string& detail) {
  std::scoped_lock lock(state.mutex);
  state.error = detail;
}

void check_periodic_sender(const PeriodicSenderState& state) {
  std::scoped_lock lock(state.mutex);
  if (!state.error.empty()) {
    throw std::runtime_error(
        "时代新安 0x425/TesterPresent periodic sender failed: " +
        state.error);
  }
}

void report(const FlashWorkflowCallbacks& callbacks, std::string step,
            std::string verdict, std::string detail) {
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict),
                     std::move(detail));
  }
}

std::string hex_u32(std::uint32_t value) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << value;
  return output.str();
}

std::filesystem::path resolved_path(
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& selected) {
  if (selected.empty() || selected.is_absolute()) return selected;
  return executable_directory / selected;
}

bool same_bytes(std::span<const std::uint8_t> left,
                std::span<const std::uint8_t> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}
} // namespace

std::wstring_view ShidaixinanHjzjFmrWorkflow::id() const noexcept {
  return L"shidaixinan_hjzj_fmr";
}

std::string ShidaixinanHjzjFmrWorkflow::report_title(
    const FlashProfile&) const {
  return "Shidaixinan HJZJ FMR Download Report";
}

void ShidaixinanHjzjFmrWorkflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (!job.profile.can_fd || job.profile.extended_id ||
      !job.profile.uds_fd || job.profile.uds_brs) {
    throw std::runtime_error(
        "时代新安成功ASC要求11位CAN FD诊断且UDS请求BRS关闭");
  }
  if (job.profile.functional_id != 0x7DF) {
    throw std::runtime_error(
        "时代新安 HJZJ_FMR 功能诊断ID必须为7DF");
  }
  if (job.profile.nominal_bitrate != 500000 ||
      job.profile.data_bitrate != 2000000 ||
      job.profile.padding != 0x00 ||
      job.profile.isotp_st_min != 0) {
    throw std::runtime_error(
        "时代新安成功ASC要求500k/2M、padding=00、BS=0、STmin=0");
  }
  if (job.profile.security_level != 0x01 ||
      !job.profile.security_variant.empty()) {
    throw std::runtime_error(
        "时代新安 FMR GenerateKeyEx要求基础level=01且variant为空");
  }
  const auto entry_mode =
      job.entry_mode.empty() ? std::wstring_view{L"app"}
                             : std::wstring_view{job.entry_mode};
  if (entry_mode != L"app" && entry_mode != L"ft") {
    throw std::runtime_error(
        "时代新安 HJZJ_FMR 只支持APP或FT入口");
  }
  const bool ft_entry = entry_mode == L"ft";
  if (ft_entry &&
      (!job.profile.supports_ft_entry ||
       job.profile.ft_tx_id !=
           kShidaixinanHjzjFtFunctionalTxId ||
       job.profile.ft_rx_id !=
           kShidaixinanHjzjFtFunctionalRxId ||
       job.profile.ft_extended_id ||
       !job.profile.ft_uds_fd ||
       job.profile.ft_uds_brs ||
       job.profile.ft_padding != 0x00)) {
    throw std::runtime_error(
        "时代新安FT入口必须为功能寻址7DF->761、11位CAN FD、"
        "BRS关闭、padding=00");
  }

  const auto driver_path =
      resolved_path(job.executable_directory, job.driver_file);
  const auto app_path =
      resolved_path(job.executable_directory, job.app_file);
  const auto security_dll =
      resolved_path(job.executable_directory, job.security_dll);
  const auto broker = job.executable_directory / L"keygen_broker.exe";

  ShidaixinanHjzjFmrImages images;
  try {
    images.driver = load_single_srecord_segment(driver_path);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("时代新安 Driver S19自动分析失败，尚未访问总线: ") +
        error.what());
  }
  try {
    images.app = load_single_srecord_segment(app_path);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("时代新安 APP S19自动分析失败，尚未访问总线: ") +
        error.what());
  }
  if (images.driver.data.size() >
          static_cast<std::size_t>(
              std::numeric_limits<std::uint32_t>::max()) ||
      images.app.data.size() >
          static_cast<std::size_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error(
        "时代新安 S19自动分析窗口超过32位UDS地址/长度范围");
  }

  const auto driver_crc =
      shidaixinan_hjzj_crc32(images.driver.data);
  const auto app_crc = shidaixinan_hjzj_crc32(images.app.data);
  const auto layout_detail =
      "Driver=" + hex_u32(images.driver.address) + "/" +
      hex_u32(static_cast<std::uint32_t>(images.driver.data.size())) +
      ", CRC32=" + hex_u32(driver_crc) + "; APP=" +
      hex_u32(images.app.address) + "/" +
      hex_u32(static_cast<std::uint32_t>(images.app.data.size())) +
      ", CRC32=" + hex_u32(app_crc);
  if (callbacks.log) {
    callbacks.log(
        "时代新安S19自动分析完成（仅S1/S2/S3数据记录，不采用S7/S8/S9入口地址）："
        + layout_detail);
  }
  report(callbacks, "S19 auto layout", "PASS", layout_detail);

  const auto keygen =
      [broker, security_dll](
          std::span<const std::uint8_t> seed, unsigned level) {
        return generate_key_x86(broker, security_dll, seed, level,
                                L"");
      };
  constexpr std::array<std::uint8_t, 4> kLevel1Seed{
      0x7A, 0x45, 0xFA, 0x55};
  constexpr std::array<std::uint8_t, 4> kLevel1Key{
      0xB0, 0x16, 0xE1, 0x3B};
  constexpr std::array<std::uint8_t, 4> kLevel3Seed{
      0xE9, 0x3F, 0xDF, 0xD0};
  constexpr std::array<std::uint8_t, 4> kLevel3Key{
      0x80, 0x7A, 0xA7, 0xFA};
  try {
    const auto level1 = keygen(kLevel1Seed, 0x01);
    const auto level3 = keygen(kLevel3Seed, 0x03);
    if (!same_bytes(level1, kLevel1Key) ||
        !same_bytes(level3, kLevel3Key)) {
      throw std::runtime_error(
          "captured SeedKey vectors do not match");
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string(
            "时代新安FMR.dll/x86 broker预检失败，尚未访问总线: ") +
        error.what());
  }
  report(callbacks, "SeedKey preflight", "PASS",
         "GenerateKeyEx level01 7A45FA55->B016E13B; "
         "level03 E93FDFD0->807AA7FA");

  const bool reference_images =
      driver_crc == kShidaixinanReferenceDriverCrc32 &&
      app_crc == kShidaixinanReferenceAppCrc32;
  if (!reference_images) {
    const auto warning =
        "所选S19与2026-07-30成功基线CRC不同；地址、长度和CRC仍按所选文件自动生成。";
    if (callbacks.log) callbacks.log(std::string("WARN：") + warning);
    report(callbacks, "Reference identity", "WARN", warning);
  } else {
    report(callbacks, "Reference identity", "PASS",
           "Packaged Driver/App CRC32 match the complete 2026-07-30 "
           "CANoe/legacy success trace");
  }

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, true, L"UDSToolCpp"});

  IsoTpConfig physical_config{
      job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
      job.profile.isotp_st_min, 1000ms, 1000ms, false, false, true,
      false};
  physical_config.tx_data_length = 64;
  physical_config.batch_consecutive_frames = false;
  IsoTpSession physical_transport(*bus, physical_config);
  auto functional_config = physical_config;
  functional_config.tx_id = job.profile.functional_id;
  IsoTpSession functional_transport(*bus, functional_config);
  auto ft_functional_config = physical_config;
  ft_functional_config.tx_id = job.profile.ft_tx_id;
  ft_functional_config.rx_id = job.profile.ft_rx_id;
  ft_functional_config.padding = job.profile.ft_padding;
  ft_functional_config.tx_extended =
      job.profile.ft_extended_id;
  ft_functional_config.rx_extended =
      job.profile.ft_extended_id;
  ft_functional_config.tx_fd = job.profile.ft_uds_fd;
  ft_functional_config.tx_brs = job.profile.ft_uds_brs;
  ft_functional_config.tx_data_length =
      job.profile.ft_uds_fd ? 64U : 8U;
  IsoTpSession ft_functional_transport(
      *bus, ft_functional_config);
  UdsClient physical(
      physical_transport,
      [&](const std::string& line) {
        if (callbacks.log) callbacks.log(line);
      },
      stop);
  UdsClient functional(
      functional_transport,
      [&](const std::string& line) {
        if (callbacks.log) callbacks.log(line);
      },
      stop);
  UdsClient ft_functional(
      ft_functional_transport,
      [&](const std::string& line) {
        if (callbacks.log) callbacks.log(line);
      },
      stop);

  auto flow_log = [&](int percent, const std::string& line) {
    if (callbacks.log) callbacks.log(line);
    const auto transfer_progress =
        line.starts_with("36 ") &&
        line.find(" progress:") != std::string::npos;
    if (callbacks.progress && !transfer_progress) {
      callbacks.progress(percent, line);
    }
    if (callbacks.report && !line.starts_with("36 ") &&
        (line.find("PASS") != std::string::npos ||
         line.find("WARN") != std::string::npos)) {
      callbacks.report(
          line,
          line.find("WARN") != std::string::npos ? "WARN" : "PASS",
          line);
    }
  };
  auto flow_progress = [&](int percent, const std::string& line) {
    if (callbacks.progress) callbacks.progress(percent, line);
  };

  PeriodicSenderState periodic_state;
  const auto wakeup = shidaixinan_hjzj_wakeup_frame();
  const auto tester_present =
      shidaixinan_hjzj_tester_present_frame();
  std::jthread periodic_sender(
      [&bus, &periodic_state, wakeup,
       tester_present](std::stop_token sender_stop) {
        ScopedHighResolutionTimer timer_resolution;
        const auto started = std::chrono::steady_clock::now();
        auto next_wakeup = started;
        auto next_tester =
            started + kShidaixinanHjzjTesterPresentInitialDelay;
        std::size_t consecutive_wakeup_failures = 0;
        while (!sender_stop.stop_requested()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= next_wakeup) {
            try {
              bus->send(wakeup);
              consecutive_wakeup_failures = 0;
            } catch (const std::exception& error) {
              ++consecutive_wakeup_failures;
              if (consecutive_wakeup_failures >=
                  kShidaixinanHjzjMaximumWakeFailures) {
                set_periodic_error(periodic_state, error.what());
                return;
              }
            } catch (...) {
              ++consecutive_wakeup_failures;
              if (consecutive_wakeup_failures >=
                  kShidaixinanHjzjMaximumWakeFailures) {
                set_periodic_error(
                    periodic_state,
                    "unknown 0x425 transmit error");
                return;
              }
            }
            do {
              next_wakeup += kShidaixinanHjzjWakeupPeriod;
            } while (next_wakeup <= now);
          }
          if (now >= next_tester) {
            try {
              bus->send(tester_present);
            } catch (const std::exception& error) {
              set_periodic_error(periodic_state, error.what());
              return;
            } catch (...) {
              set_periodic_error(
                  periodic_state,
                  "unknown TesterPresent transmit error");
              return;
            }
            do {
              next_tester +=
                  kShidaixinanHjzjTesterPresentPeriod;
            } while (next_tester <= now);
          }
          std::this_thread::sleep_for(1ms);
        }
      });
  const auto stop_periodic = [&periodic_sender]() {
    periodic_sender.request_stop();
    if (periodic_sender.joinable()) periodic_sender.join();
  };

  if (callbacks.log) {
    callbacks.log(
        "时代新安网络保持：按完整ASC启动0x425全零标准CAN FD+BRS，"
        "周期10ms；0x7DF 02 3E 80标准CAN FD无BRS，首次600ms、后续3s。");
  }
  report(callbacks, "Network wake-up", "INFO",
         "0x425 DLC8 all-zero CAN FD+BRS every 10 ms; "
         "functional 0x7DF TesterPresent every 3 s");
  for (auto elapsed = 0ms;
       elapsed < kShidaixinanHjzjWakeupSettle;
       elapsed += 10ms) {
    if (stop.stop_requested()) {
      stop_periodic();
      throw std::runtime_error("operation cancelled by user");
    }
    check_periodic_sender(periodic_state);
    std::this_thread::sleep_for(10ms);
  }

  ShidaixinanHjzjFmrFlow flow(
      physical, functional, ft_functional, flow_log, flow_progress,
      keygen,
      [&periodic_state]() {
        check_periodic_sender(periodic_state);
      });
  try {
    flow.run(
        images,
        ft_entry ? ShidaixinanHjzjFmrEntryMode::ft
                 : ShidaixinanHjzjFmrEntryMode::app,
        stop);
    stop_periodic();
    check_periodic_sender(periodic_state);
  } catch (...) {
    stop_periodic();
    const auto warning =
        flow.core_programming_completed()
            ? "时代新安Driver/APP下载、CRC、依赖检查和ECU Reset已全部通过，"
              "但FT刷后收尾或最终网络保持检查未完成；不要自动重复刷写，"
              "应先确认APP在线并单独处理收尾。"
            : "时代新安流程在最终依赖检查和ECU Reset全部通过前退出；"
              "当前擦除/传输/会话状态未知，工具不会擅自发送额外恢复命令。";
    if (callbacks.log) callbacks.log(std::string("WARN：") + warning);
    report(callbacks,
           flow.core_programming_completed()
               ? "Post-programming cleanup"
               : "Failure state",
           "WARN", warning);
    throw;
  }
  report(callbacks, "Download", "PASS",
         ft_entry
             ? "时代新安 HJZJ_FMR FT（PLS->APP）流程完成"
             : "时代新安 HJZJ_FMR APP流程完成");
}

} // namespace uds
