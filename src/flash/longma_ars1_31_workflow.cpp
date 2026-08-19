#include "flash/longma_ars1_31_workflow.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/longma_ars1_31_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {

void record(const FlashWorkflowCallbacks& callbacks, int percent,
            std::string step, std::string verdict, std::string detail) {
  static_cast<void>(percent);
  if (callbacks.report) {
    callbacks.report(std::move(step), std::move(verdict), std::move(detail));
  }
}

void log(const FlashWorkflowCallbacks& callbacks, const std::string& line) {
  if (callbacks.log) callbacks.log(line);
}

std::string crc_text(std::uint16_t value) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setfill('0')
      << std::setw(4) << value;
  return out.str();
}

struct PeriodicSenderErrors {
  mutable std::mutex mutex;
  std::string precondition;
  std::string tester_present;
};

void set_sender_error(PeriodicSenderErrors& errors, std::string& destination,
                      const std::string& detail) {
  std::scoped_lock lock(errors.mutex);
  destination = detail;
}

void check_sender_errors(const PeriodicSenderErrors& errors,
                         const std::string& project_label) {
  std::scoped_lock lock(errors.mutex);
  if (!errors.precondition.empty()) {
    throw std::runtime_error(project_label + " 0x400 sender failed: " +
                             errors.precondition);
  }
  if (!errors.tester_present.empty()) {
    throw std::runtime_error(project_label + " TesterPresent sender failed: " +
                             errors.tester_present);
  }
}

const FlashTargetProfile* selected_target(const FlashProfile& profile) {
  const auto item = std::find_if(
      profile.targets.cbegin(), profile.targets.cend(),
      [&profile](const FlashTargetProfile& target) {
        return target.tx_id == profile.tx_id && target.rx_id == profile.rx_id;
      });
  return item == profile.targets.cend() ? nullptr : &*item;
}

} // namespace

Ars131AppWorkflow::Ars131AppWorkflow(std::wstring workflow_id,
                                     std::string project_label,
                                     std::string report_prefix)
    : workflow_id_(std::move(workflow_id)),
      project_label_(std::move(project_label)),
      report_prefix_(std::move(report_prefix)) {}

LongmaArs131Workflow::LongmaArs131Workflow()
    : Ars131AppWorkflow(L"longma_ars1_31", "Longma ARS1.31",
                        "Longma ARS1.31") {}

std::wstring_view Ars131AppWorkflow::id() const noexcept {
  return workflow_id_;
}

std::string Ars131AppWorkflow::report_title(
    const FlashProfile& profile) const {
  return longma_ars131_secondary_endpoint(profile.tx_id, profile.rx_id)
             ? report_prefix_ + " Secondary Radar Download Report"
             : report_prefix_ + " Main Radar Download Report";
}

void Ars131AppWorkflow::run(
    const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
    std::stop_token stop) {
  if (job.profile.can_fd) {
    throw std::runtime_error(project_label_ +
                             " CAPL reference requires Classic CAN");
  }
  if (job.profile.nominal_bitrate != 500000) {
    throw std::runtime_error(project_label_ +
                             " requires Classic CAN 500 kbit/s");
  }
  if (job.profile.functional_id != 0x7DF) {
    throw std::runtime_error(
        project_label_ + " functional diagnostic endpoint must be 0x7DF");
  }
  if (job.profile.padding != 0x00) {
    throw std::runtime_error(project_label_ +
                             " ISO-TP padding must be 0x00");
  }
  if (job.profile.power_control) {
    throw std::runtime_error(
        project_label_ +
        " uses external bench power and must not control CANoe DOUT");
  }
  if (job.profile.security_level != 0x01 ||
      !job.profile.security_variant.empty()) {
    throw std::runtime_error(
        project_label_ +
        " GenerateKeyEx requires level 0x01 and an empty variant");
  }

  const LongmaArs131Layout layout{job.profile.driver_start,
                                  job.profile.driver_length,
                                  job.profile.app_start,
                                  job.profile.app_length,
                                  job.profile.cal_start,
                                  job.profile.cal_length};
  if (layout.driver_start != 0x08000000 || layout.driver_length != 0x400 ||
      layout.app_start != 0xC0080000 || layout.app_length != 0xF5000) {
    throw std::runtime_error(
        project_label_ +
        " layout must be Driver=0x08000000/0x400, "
        "APP=0xC0080000/0xF5000");
  }
  const auto* target = selected_target(job.profile);
  const auto entry_mode =
      job.entry_mode.empty() ? std::wstring_view{L"app"}
                             : std::wstring_view{job.entry_mode};
  const auto plan = resolve_longma_ars131_download_plan(entry_mode);
  if (plan.download_cal && !job.profile.supports_cal_download) {
    throw std::runtime_error(project_label_ +
                             " profile does not enable CAL download");
  }
  if (plan.download_cal &&
      (layout.cal_start != 0xC0180000 || layout.cal_length != 0x270)) {
    throw std::runtime_error(
        project_label_ + " CAL layout must be 0xC0180000/0x270");
  }
  const auto ft_tx_id =
      target != nullptr && target->ft_tx_id != 0
          ? target->ft_tx_id
          : job.profile.ft_tx_id;
  const auto ft_rx_id =
      target != nullptr && target->ft_rx_id != 0
          ? target->ft_rx_id
          : job.profile.ft_rx_id;
  if (plan.ft_entry &&
      (!job.profile.supports_ft_entry || ft_tx_id == 0 || ft_rx_id == 0)) {
    throw std::runtime_error(project_label_ +
                             " FT recovery endpoint is not configured");
  }

  const auto content_names =
      plan.download_app && plan.download_cal
          ? "Driver、APP 和 CAL"
          : plan.download_cal ? "Driver 和 CAL" : "Driver 和 APP";
  record(callbacks, 0, "Preflight", "INFO",
         "Loading " + std::string(content_names) +
             " S19 windows defined by the CANoe Protocol branch");
  log(callbacks, "预检查：加载 " + project_label_ + " " +
                     content_names + " S19 文件……");
  const auto secondary = target != nullptr && target->id == L"secondary";
  std::ostringstream endpoint_stream;
  endpoint_stream << (target == nullptr ? "Custom diagnostic endpoint"
                                         : (secondary ? "Secondary radar"
                                                      : "Main radar"))
                  << " TX=0x" << std::uppercase << std::hex
                  << job.profile.tx_id << "/RX=0x" << job.profile.rx_id;
  const auto endpoint_detail = endpoint_stream.str();
  const auto pending_validation =
      target != nullptr && target->pending_validation;
  record(callbacks, 0, "Radar target",
         pending_validation ? "INFO" : "PASS",
         endpoint_detail +
             (pending_validation ? " (pending bench validation)" : ""));
  log(callbacks,
      std::string("目标：") + (target == nullptr ? "自定义诊断地址"
                                                   : (secondary ? "从雷达" : "主雷达")) +
          "，" + endpoint_detail +
          (pending_validation ? "；该端点尚待台架/实车验证。" : "。"));

  LongmaArs131Images images;
  try {
    images.driver = load_srecord_window(job.driver_file, layout.driver_start,
                                        layout.driver_length);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("Driver S19 preflight failed: ") +
                             error.what());
  }
  if (plan.download_app) {
    try {
      images.app = load_srecord_window(job.app_file, layout.app_start,
                                       layout.app_length);
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("APP S19 preflight failed: ") +
                               error.what());
    }
  }
  if (plan.download_cal) {
    try {
      images.cal = load_srecord_window(job.cal_file, layout.cal_start,
                                       layout.cal_length);
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("CAL S19 preflight failed: ") +
                               error.what());
    }
  }

  const auto driver_crc = longma_ars131_crc16_ccitt_false(images.driver);
  const std::uint16_t app_crc =
      plan.download_app ? longma_ars131_crc16_ccitt_false(images.app) : 0;
  const std::uint16_t cal_crc =
      plan.download_cal ? longma_ars131_crc16_ccitt_false(images.cal) : 0;
  const auto expected_driver_crc = job.profile.expected_driver_crc16;
  const auto expected_app_crc =
      target == nullptr ? std::uint16_t{} : target->expected_app_crc16;
  const auto driver_matches =
      expected_driver_crc == 0 || driver_crc == expected_driver_crc;
  const auto app_matches = !plan.download_app || expected_app_crc == 0 ||
                           app_crc == expected_app_crc;
  const auto has_reference =
      expected_driver_crc != 0 &&
      (!plan.download_app || expected_app_crc != 0);
  const auto matches_reference =
      has_reference && driver_matches && app_matches;
  auto crc_detail = "Driver CRC16=" + crc_text(driver_crc);
  if (plan.download_app) crc_detail += ", APP CRC16=" + crc_text(app_crc);
  if (plan.download_cal) crc_detail += ", CAL CRC16=" + crc_text(cal_crc);
  crc_detail += " (every TransferExit verifies the selected file CRC "
                "dynamically, matching CANoe CAPL)";
  const auto reference_mismatch =
      has_reference && (!driver_matches || !app_matches);
  if (reference_mismatch) {
    auto reference_detail =
        "Selected files differ from the packaged reference CRC: Driver "
        "expected=" +
        crc_text(expected_driver_crc) + ", actual=" + crc_text(driver_crc);
    if (plan.download_app) {
      reference_detail += ", APP expected=" + crc_text(expected_app_crc) +
                          ", actual=" + crc_text(app_crc);
    }
    reference_detail +=
        ". Continue with dynamically calculated CRC, as CANoe CAPL does.";
    record(callbacks, 0, "Reference CRC", "WARN", reference_detail);
    log(callbacks, "提示：" + reference_detail);
  }
  record(callbacks, 0, "Preflight",
         matches_reference ? "PASS" : "INFO", crc_detail);
  log(callbacks, "预检查完成：" + crc_detail);
  if (stop.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }

  log(callbacks, "供电：本项目不调用 CANoe DOUT，保持台架外部供电状态。");
  record(callbacks, 0, "Power", "INFO",
         "External power unchanged; no CANoe DOUT operation");

  if (!job.can_bus_provider) {
    throw std::runtime_error("CAN bus provider is not configured");
  }
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, false, L"UDSToolCpp"});

  const CanFrame precondition{0x400,
                              {0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00},
                              false, false, false};
  bus->send(precondition);
  log(callbacks, "刷写前置报文已发送：0x400 00 00 00 00 00 00 00 00 @100ms。");
  record(callbacks, 0, "Precondition frame", "PASS",
         "Classic CAN 0x400 00 00 00 00 00 00 00 00 @100ms");

  PeriodicSenderErrors sender_errors;
  std::jthread precondition_sender(
      [&bus, &precondition, &sender_errors](std::stop_token sender_stop) {
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(100);
        while (!sender_stop.stop_requested()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= next) {
            try {
              bus->send(precondition);
            } catch (const std::exception& error) {
              set_sender_error(sender_errors, sender_errors.precondition,
                               error.what());
              return;
            } catch (...) {
              set_sender_error(sender_errors, sender_errors.precondition,
                               "unknown CAN transmit error");
              return;
            }
            do {
              next += std::chrono::milliseconds(100);
            } while (next <= now);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });

  IsoTpSession physical_transport(
      *bus, {job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
            job.profile.isotp_st_min});
  IsoTpSession functional_transport(
      *bus, {job.profile.functional_id, job.profile.rx_id, job.profile.padding,
            0, job.profile.isotp_st_min});
  std::unique_ptr<IsoTpSession> ft_transport;
  std::unique_ptr<UdsClient> ft_physical;

  // Flash.can owns one re-arming 4 s timer which emits exactly this raw,
  // suppress-positive-response functional TesterPresent frame.
  std::jthread tester_present_sender(
      [&functional_transport, &sender_errors](std::stop_token sender_stop) {
        constexpr std::array<std::uint8_t, 8> frame{
            0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::seconds(4);
        while (!sender_stop.stop_requested()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= next) {
            try {
              functional_transport.send_raw(0x7DF, frame);
            } catch (const std::exception& error) {
              set_sender_error(sender_errors, sender_errors.tester_present,
                               error.what());
              return;
            } catch (...) {
              set_sender_error(sender_errors, sender_errors.tester_present,
                               "unknown CAN transmit error");
              return;
            }
            do {
              next += std::chrono::seconds(4);
            } while (next <= now);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
  record(callbacks, 0, "TesterPresent", "INFO",
         "One functional 0x7DF 02 3E 80 00 00 00 00 00 sender @4s");

  auto uds_log = [&](const std::string& line) {
    log(callbacks, line);
    record(callbacks, 0, "UDS", "INFO", line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);
  if (plan.ft_entry) {
    ft_transport = std::make_unique<IsoTpSession>(
        *bus, IsoTpConfig{ft_tx_id, ft_rx_id, job.profile.ft_padding, 0, 0,
                          std::chrono::milliseconds(1000),
                          std::chrono::milliseconds(1000),
                          job.profile.ft_extended_id,
                          job.profile.ft_extended_id,
                          job.profile.ft_uds_fd,
                          job.profile.ft_uds_brs});
    ft_physical =
        std::make_unique<UdsClient>(*ft_transport, uds_log, stop);
    std::ostringstream endpoint;
    endpoint << "FT恢复入口：0x" << std::uppercase << std::hex << ft_tx_id
             << " -> 0x" << ft_rx_id
             << "；切换后继续使用正常物理端点。";
    log(callbacks, endpoint.str());
    record(callbacks, 0, "FT recovery endpoint", "INFO", endpoint.str());
  }

  const auto broker = job.executable_directory / L"keygen_broker.exe";
  auto keygen = [broker, dll = job.security_dll,
                 level = job.profile.security_level,
                 variant = job.profile.security_variant](
                    std::span<const std::uint8_t> seed) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };

  LongmaArs131Flow flow(
      physical, functional, layout,
      [&](int percent, const std::string& line) {
        if (callbacks.progress) callbacks.progress(percent, line);
        const auto pass = line.find(" PASS:") != std::string::npos ||
                          line.find(" completed") != std::string::npos;
        record(callbacks, percent, line, pass ? "PASS" : "INFO", line);
      },
      keygen, [&sender_errors, label = project_label_]() {
        check_sender_errors(sender_errors, label);
      }, ft_physical.get());

  flow.run(images, entry_mode, stop);
  tester_present_sender.request_stop();
  tester_present_sender.join();
  precondition_sender.request_stop();
  precondition_sender.join();
  check_sender_errors(sender_errors, project_label_);
  record(callbacks, 100, "Download", "PASS",
         "Flash.can::maintest/Download " +
             std::string(plan.display_name) +
             " sequence completed; " + crc_detail);
}

} // namespace uds
