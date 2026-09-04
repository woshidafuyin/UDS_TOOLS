#include "flash/chery_ars1_31_project_workflows.hpp"

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/keygen_client.hpp"
#include "core/uds_client.hpp"
#include "flash/chery_ars1_31_app_flow.hpp"
#include "flash/chery_e0y_wakeup.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace uds {
namespace {
using namespace std::chrono_literals;

std::filesystem::path resolve(const FlashJob& job,
                              const std::filesystem::path& selected) {
  if (selected.empty() || selected.is_absolute()) return selected;
  return job.executable_directory / selected;
}

void report(const FlashWorkflowCallbacks& callbacks, std::string step,
            std::string verdict, std::string detail) {
  if (callbacks.report) callbacks.report(std::move(step), std::move(verdict),
                                         std::move(detail));
}

void required(const std::filesystem::path& path, const char* label) {
  if (path.empty()) throw std::runtime_error(std::string("select ") + label);
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error(std::string(label) + " does not exist: " +
                             path.string());
  }
}

void run_project(CheryArs131Project project, std::wstring_view workflow_id,
                 const FlashJob& job,
                 const FlashWorkflowCallbacks& callbacks,
                 std::stop_token stop) {
  const auto& spec = chery_ars1_31_app_spec(project);
  if (job.profile.flow != workflow_id) {
    throw std::runtime_error(spec.name + " workflow/profile mismatch");
  }
  const auto plan = resolve_chery_ars1_31_download_plan(project,
                                                        job.entry_mode);
  if (job.update_public_key && project != CheryArs131Project::e0y) {
    throw std::runtime_error(
        "Update_PublicKey is supported only by the Chery E0Y workflow");
  }
  constexpr auto supports_cal = true;
  if (job.profile.placeholder || job.profile.can_fd ||
      job.profile.extended_id || job.profile.uds_fd || job.profile.uds_brs ||
      job.profile.power_control || job.profile.supports_ft_entry ||
      job.profile.supports_cal_download != supports_cal ||
      job.profile.nominal_bitrate != 500000 || job.profile.padding != 0x55 ||
      job.profile.functional_id != 0x7DF ||
      job.profile.security_level != spec.seed_subfunction ||
      !job.profile.security_variant.empty()) {
    throw std::runtime_error(
        spec.name + " Profile conflicts with the frozen CANoe requirement contract");
  }

  CheryArs131AppLayout layout{job.profile.driver_start,
                              job.profile.driver_length,
                              job.profile.app_start,
                              job.profile.app_length,
                              job.profile.cal_start,
                              job.profile.cal_length};
  if (layout.driver_start != 0x08000000 || layout.driver_length != 0x400 ||
      layout.app_start != 0xC0080000 || layout.app_length != 0xF5000 ||
      (supports_cal &&
       (layout.cal_start != 0xC0180000 || layout.cal_length != 0xC8))) {
    throw std::runtime_error(spec.name + " Driver/APP/CAL layout mismatch");
  }
  if (project == CheryArs131Project::e0y &&
      (job.profile.tx_id != spec.tx_id || job.profile.rx_id != spec.rx_id)) {
    throw std::runtime_error(
        "Chery E0Y diagnostic endpoint must match CANoe 0x71F/0x79F");
  }

  const auto driver = resolve(job, job.driver_file);
  const auto app = resolve(job, job.app_file);
  const auto cal = resolve(job, job.cal_file);
  const auto driver_signature = resolve(job, job.driver_verify_file);
  const auto app_signature = resolve(job, job.app_verify_file);
  const auto cal_signature = resolve(job, job.cal_verify_file);
  const auto cal_needs_app_signature =
      project == CheryArs131Project::t22 &&
      plan.mode == CheryArs131FlashMode::cal_only;
  const auto dll = resolve(job, job.security_dll);
  required(driver, "Driver S19");
  required(driver_signature, "Driver 512-byte RSA");
  if (plan.download_app || cal_needs_app_signature) {
    required(app_signature, "APP 512-byte RSA");
  }
  if (plan.download_app) {
    required(app, "APP S19");
  }
  if (plan.download_cal) {
    required(cal, "CAL S19");
    required(cal_signature, "CAL 512-byte RSA");
  }
  required(dll, "SeedKey DLL");

  CheryArs131AppImages images;
  try {
    images.driver = load_srecord_window(driver, layout.driver_start,
                                        layout.driver_length);
    images.driver_signature = load_hex_bytes(driver_signature, 512, 512);
    if (plan.download_app || cal_needs_app_signature) {
      images.app_signature = load_hex_bytes(app_signature, 512, 512);
    }
    if (plan.download_app) {
      images.app = load_srecord_window(app, layout.app_start,
                                       layout.app_length);
    }
    if (plan.download_cal) {
      images.cal = load_srecord_window(cal, layout.cal_start,
                                       layout.cal_length);
      images.cal_signature = load_hex_bytes(cal_signature, 512, 512);
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(spec.name +
                             " file preflight failed before CAN access: " +
                             error.what());
  }
  std::ostringstream contract;
  contract << spec.name << "; effective TX/RX=0x" << std::hex
           << std::uppercase << job.profile.tx_id << "/0x"
           << job.profile.rx_id << "; CANoe default=0x" << spec.tx_id
           << "/0x" << spec.rx_id << "; 27 "
           << static_cast<unsigned>(spec.seed_subfunction) << "/"
           << static_cast<unsigned>(spec.seed_subfunction + 1)
           << "; seed/key=" << std::dec << spec.seed_length << "/"
           << spec.key_length << "; source=frozen public-share requirement";
  report(callbacks, "Requirement contract", "PASS", contract.str());
  report(callbacks, "Acceptance boundary", "WARN",
         "Offline preflight passed; real ECU acceptance still requires a hash-bound bench report and trace");
  report(callbacks, "Update_PublicKey",
         job.update_public_key ? "WARN" : "INFO",
         job.update_public_key
             ? "Enabled: after SecurityAccess, send 2E 6F00 plus the frozen 514-byte Panel public key before 2E F184"
             : "Disabled: matches the CANoe Panel default; no 2E 6F00 request will be sent");

  const auto broker = job.executable_directory / L"keygen_broker.exe";
  required(broker, "x86 SeedKey broker");
  auto keygen = [broker, dll, variant = job.profile.security_variant](
                    std::span<const std::uint8_t> seed, unsigned level) {
    return generate_key_x86(broker, dll, seed, level, variant);
  };
  try {
    if (spec.seed_length == 4) {
      constexpr std::array<std::uint8_t, 4> zero_seed{};
      constexpr std::array<std::uint8_t, 4> expected{
          0xFF, 0xFF, 0x93, 0xBC};
      const auto actual = keygen(zero_seed, spec.seed_subfunction);
      if (actual.size() != expected.size() ||
          !std::equal(actual.begin(), actual.end(), expected.begin())) {
        throw std::runtime_error("GenerateKeyExOpt level 0x07 vector mismatch");
      }
    } else {
      constexpr std::array<std::uint8_t, 16> zero_seed{};
      constexpr std::array<std::uint8_t, 16> expected{
          0xEB, 0x45, 0x8E, 0xD6, 0x24, 0x35, 0xF7, 0xED,
          0x59, 0xC0, 0xC0, 0x32, 0xD1, 0x6E, 0x9E, 0xDC};
      const auto actual = keygen(zero_seed, spec.seed_subfunction);
      if (actual.size() != expected.size() ||
          !std::equal(actual.begin(), actual.end(), expected.begin())) {
        throw std::runtime_error("GenerateKeyEx level 0x11 vector mismatch");
      }
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(spec.name +
                             " SeedKey preflight failed before CAN access: " +
                             error.what());
  }
  report(callbacks, "SeedKey preflight", "PASS",
         spec.seed_length == 4
             ? "GenerateKeyExOpt: 00000000 -> FFFF93BC at level 0x07"
             : "GenerateKeyEx: 16-byte zero seed vector matched at level 0x11");

  if (!job.can_bus_provider) throw std::runtime_error("CAN bus provider is not configured");
  auto bus = job.can_bus_provider->create(
      {"", job.profile.channel, job.profile.nominal_bitrate,
       job.profile.data_bitrate, false, L"UDSToolCpp"});
  std::unique_ptr<CheryE0yWakeupSession> e0y_wakeup;
  if (project == CheryArs131Project::e0y) {
    e0y_wakeup = std::make_unique<CheryE0yWakeupSession>(
        *bus, stop, [&](const std::string& line) {
          if (callbacks.log) callbacks.log(line);
        });
    e0y_wakeup->start();
    e0y_wakeup->wait_until_settled();
    report(callbacks, "E0Y wake-up", "PASS",
           "0x600 all-zero Classic CAN @1000ms; 15000ms settle before first diagnostic request");
  }
  IsoTpSession physical_transport(
      *bus, {job.profile.tx_id, job.profile.rx_id, job.profile.padding, 0,
             job.profile.isotp_st_min});
  IsoTpSession functional_transport(
      *bus, {job.profile.functional_id, job.profile.rx_id,
             job.profile.padding, 0, job.profile.isotp_st_min});
  const auto uds_log = [&](const std::string& line) {
    if (callbacks.log) callbacks.log(line);
  };
  UdsClient physical(physical_transport, uds_log, stop);
  UdsClient functional(functional_transport, uds_log, stop);
  CheryArs131AppFlow flow(
      physical, functional, spec, layout,
      [&](int percent, const std::string& line) {
        if (callbacks.log) callbacks.log(line);
        if (callbacks.progress && !line.starts_with("36 ")) {
          callbacks.progress(percent, line);
        }
        report(callbacks, line,
               line.find("PASS:") == std::string::npos ? "INFO" : "PASS",
               line);
      },
      keygen);
  flow.run(images, plan.mode, job.update_public_key, stop);
  if (e0y_wakeup) e0y_wakeup->stop_and_check();
  const auto mode = plan.download_app
                        ? (plan.download_cal ? "APP+CAL/TC_2" : "APP")
                        : "CAL/TC_7";
  report(callbacks, "Download", "PASS",
         spec.name + " normal " + mode + " flow completed");
}
} // namespace

std::wstring_view CheryT1ejWorkflow::id() const noexcept { return L"chery_t1ej"; }
std::string CheryT1ejWorkflow::report_title(const FlashProfile&) const {
  return "奇瑞 T1EJ ARS1.31 正常刷写报告";
}
void CheryT1ejWorkflow::run(const FlashJob& job,
                            const FlashWorkflowCallbacks& callbacks,
                            std::stop_token stop) {
  run_project(CheryArs131Project::t1ej, id(), job, callbacks, stop);
}

std::wstring_view CheryT22Workflow::id() const noexcept { return L"chery_t22"; }
std::string CheryT22Workflow::report_title(const FlashProfile&) const {
  return "奇瑞 T22 ARS1.31 正常刷写报告";
}
void CheryT22Workflow::run(const FlashJob& job,
                           const FlashWorkflowCallbacks& callbacks,
                           std::stop_token stop) {
  run_project(CheryArs131Project::t22, id(), job, callbacks, stop);
}

std::wstring_view CheryE0yWorkflow::id() const noexcept { return L"chery_e0y"; }
std::string CheryE0yWorkflow::report_title(const FlashProfile&) const {
  return "奇瑞 E0Y ARS1.31 正常刷写报告";
}
void CheryE0yWorkflow::run(const FlashJob& job,
                           const FlashWorkflowCallbacks& callbacks,
                           std::stop_token stop) {
  run_project(CheryArs131Project::e0y, id(), job, callbacks, stop);
}

} // namespace uds
