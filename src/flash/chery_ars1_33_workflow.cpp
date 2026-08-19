#include "flash/chery_ars1_33_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/chery_ars1_33_flow.hpp"

#include <array>
#include <chrono>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace uds {
namespace {

void record(const FlashWorkflowCallbacks& callbacks, int percent, std::string step,
            std::string verdict, std::string detail) {
  static_cast<void>(percent);
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

void log(const FlashWorkflowCallbacks& callbacks, const std::string& line) {
  if (callbacks.log) callbacks.log(line);
}

std::string layout_detail(const CheryArs133Layout& layout) {
  std::ostringstream out;
  out << std::hex << std::uppercase
      << "FLD0=0x" << layout.driver0_start << "/0x" << layout.driver0_length
      << ", FLD=0x" << layout.driver_start << "/0x" << layout.driver_length
      << ", APP=0x" << layout.app_start << "/0x" << layout.app_length
      << ", CAL=0x" << layout.cal_start << "/0x" << layout.cal_length;
  return out.str();
}

std::string_view mode_name(CheryArs133FlashMode mode) {
  switch (mode) {
    case CheryArs133FlashMode::AppCal:
      return "APP+CAL";
    case CheryArs133FlashMode::AppOnly:
      return "APP-only";
    case CheryArs133FlashMode::CalOnly:
      return "CAL-only";
  }
  return "unknown";
}

std::string_view capl_entry(CheryArs133FlashMode mode) {
  switch (mode) {
    case CheryArs133FlashMode::AppCal:
      return "APPAndCAL/TC_2";
    case CheryArs133FlashMode::AppOnly:
      return "APP/Download";
    case CheryArs133FlashMode::CalOnly:
      return "CAL/TC_7";
  }
  return "unknown";
}

} // namespace

std::wstring_view CheryArs133Workflow::id() const noexcept {
  return L"chery_ars1_33";
}

std::string CheryArs133Workflow::report_title(const FlashProfile&) const {
  return "奇瑞 ARS1.33 刷写报告";
}

void CheryArs133Workflow::run(const FlashJob& job,
                              const FlashWorkflowCallbacks& callbacks,
                              std::stop_token stop) {
  if (job.profile.can_fd) {
    throw std::runtime_error("Chery ARS1.33 CAPL reference requires Classic CAN");
  }
  if (job.profile.security_level != 0x11 || !job.profile.security_variant.empty()) {
    throw std::runtime_error("Chery ARS1.33 security must use level 0x11 and an empty variant");
  }
  const auto plan = resolve_chery_ars133_download_plan(job.entry_mode);
  CheryArs133Layout layout{job.profile.driver0_start, job.profile.driver0_length,
                           job.profile.driver_start, job.profile.driver_length,
                           job.profile.app_start, job.profile.app_length,
                           job.profile.cal_start, job.profile.cal_length};
  if (layout.driver0_length == 0 || layout.driver_length == 0 ||
      (plan.download_app && layout.app_length == 0) ||
      (plan.download_cal && layout.cal_length == 0)) {
    throw std::runtime_error("Chery ARS1.33 flash layout is incomplete in the profile");
  }

  const auto selected_mode = std::string(mode_name(plan.mode));
  record(callbacks, 0, "Preflight", "INFO",
         "Loading ARS1.33 resources for " + selected_mode + "; " +
             layout_detail(layout));
  log(callbacks, "预检查：加载奇瑞 ARS1.33 " + selected_mode +
                     " 固件及 RSA 校验文件……");

  CheryArs133Images images;
  // The FLD S19 contains two disjoint windows. CAPL FileInit() reads the same
  // file twice and filters records for each configured address window.
  images.driver0 = load_srecord_window_filtered(job.driver_file,
                                                 layout.driver0_start,
                                                 layout.driver0_length);
  images.driver = load_srecord_window_filtered(job.driver_file,
                                                layout.driver_start,
                                                layout.driver_length);
  images.driver_verification = load_hex_bytes(job.driver_verify_file, 512, 512);
  if (plan.download_app) {
    images.app =
        load_srecord_window(job.app_file, layout.app_start, layout.app_length);
    images.app_verification =
        load_hex_bytes(job.app_verify_file, 512, 512);
  }
  if (plan.download_cal) {
    images.cal =
        load_srecord_window(job.cal_file, layout.cal_start, layout.cal_length);
    images.cal_verification =
        load_hex_bytes(job.cal_verify_file, 512, 512);
  }
  record(callbacks, 0, "Preflight", "PASS",
         "Files validated for " + selected_mode + "; " +
             layout_detail(layout));
  if (stop.stop_requested()) throw std::runtime_error("operation cancelled by user");

  log(callbacks,
      "供电：本项目不调用 CANoe DOUT，保持台架现有外部供电状态。");
  record(callbacks, 0, "Power", "INFO",
         "External power unchanged (CAPL " +
             std::string(capl_entry(plan.mode)) + " parity)");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, false, L"UDSToolCpp"});

  // Send the requested 0x600 wake-up frame and the two vehicle-state messages.
  // Send each one synchronously before the first UDS request, then maintain its
  // configured period on the same CAN port for the complete selected sequence.
  // This common setup is deliberately outside the mode switch so APP+CAL,
  // APP-only and CAL-only cannot bypass wake-up.
  const auto precondition_frames = chery_ars133_precondition_frames();
  for (const auto& item : precondition_frames) bus->send(item.frame);
  const auto tester_present = chery_ars133_app_tester_present_frame(
      job.profile.functional_id);
  if (plan.periodic_tester_present) bus->send(tester_present);
  log(callbacks,
      "刷写前置报文已启动：0x600 全零唤醒(100ms)，"
      "0x25B PowerMode=ON(20ms)，0x4B4 Gear=P(100ms)。");
  record(callbacks, 0, "Precondition frames", "PASS",
         "0x600 00 00 00 00 00 00 00 00 @100ms; "
         "0x25B 00 00 02 00 00 00 00 00 @20ms; "
         "0x4B4 00 00 00 00 00 00 00 10 @100ms");
  if (plan.periodic_tester_present) {
    log(callbacks,
        "APP-only TesterPresent 已启动：功能寻址 3E 80，周期4000ms。");
    record(callbacks, 0, "TesterPresent", "PASS",
           "0x7DF 02 3E 80 55 55 55 55 55 @4000ms");
  }

  std::mutex precondition_error_mutex;
  std::string precondition_error;
  std::jthread precondition_sender(
      [&bus, &precondition_frames, &precondition_error_mutex,
       &precondition_error, tester_present,
       send_tester_present = plan.periodic_tester_present](
          std::stop_token sender_stop) {
        auto now = std::chrono::steady_clock::now();
        std::array<std::chrono::steady_clock::time_point, 3> next{
            now + precondition_frames[0].period,
            now + precondition_frames[1].period,
            now + precondition_frames[2].period};
        auto next_tester_present = now + std::chrono::milliseconds(4000);
        const auto set_error = [&](std::string detail) {
          std::scoped_lock lock(precondition_error_mutex);
          precondition_error = std::move(detail);
        };
        while (!sender_stop.stop_requested()) {
          now = std::chrono::steady_clock::now();
          for (std::size_t i = 0; i < precondition_frames.size(); ++i) {
            if (now < next[i]) continue;
            try {
              bus->send(precondition_frames[i].frame);
            } catch (const std::exception& e) {
              set_error(e.what());
              return;
            } catch (...) {
              set_error("unknown CAN transmit error");
              return;
            }
            do {
              next[i] += precondition_frames[i].period;
            } while (next[i] <= now);
          }
          if (send_tester_present && now >= next_tester_present) {
            try {
              bus->send(tester_present);
            } catch (const std::exception& e) {
              set_error(e.what());
              return;
            } catch (...) {
              set_error("unknown CAN transmit error");
              return;
            }
            do {
              next_tester_present += std::chrono::milliseconds(4000);
            } while (next_tester_present <= now);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });

  const auto check_precondition_sender = [&]() {
    std::scoped_lock lock(precondition_error_mutex);
    if (!precondition_error.empty()) {
      throw std::runtime_error("ARS1.33 precondition frame sender failed: " +
                               precondition_error);
    }
  };

  IsoTpSession physical_transport(
      *bus, {job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
            job.profile.isotp_st_min});
  IsoTpSession functional_transport(
      *bus, {job.profile.functional_id, job.profile.rx_id, job.profile.padding, 0,
            job.profile.isotp_st_min});
  auto uds_log = [&](const std::string& line) {
    log(callbacks, line);
    record(callbacks, 0, "UDS", "INFO", line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);

  const auto broker = job.executable_directory / L"keygen_broker.exe";
  auto keygen = [broker, dll = job.security_dll,
                 level = job.profile.security_level,
                 variant = job.profile.security_variant](std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };

  CheryArs133Flow flow(
      physical, functional, layout,
      [&](int percent, const std::string& line) {
        if (callbacks.progress) callbacks.progress(percent, line);
        const auto pass = line.find(" PASS:") != std::string::npos;
        record(callbacks, percent, line, pass ? "PASS" : "INFO", line);
      },
      keygen, check_precondition_sender);
  flow.run(images, plan.mode, stop);
  precondition_sender.request_stop();
  precondition_sender.join();
  check_precondition_sender();
  record(callbacks, 100, selected_mode, "PASS",
         "Flash_ARS1.33.can::maintest " +
             std::string(capl_entry(plan.mode)) +
             "-equivalent sequence completed");
}

} // namespace uds
