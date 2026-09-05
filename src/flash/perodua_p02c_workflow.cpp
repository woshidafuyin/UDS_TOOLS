#include "flash/perodua_p02c_workflow.hpp"

#include "core/aes_cmac.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "flash/perodua_p02c_flow.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {
using namespace std::chrono_literals;
std::filesystem::path resolve(const FlashJob& job, const std::filesystem::path& path) {
  return path.empty() || path.is_absolute() ? path : job.executable_directory / path;
}
void record(const FlashWorkflowCallbacks& cb, const std::string& step,
            const std::string& verdict, const std::string& detail) {
  if (cb.log) cb.log(step + ": " + detail);
  if (cb.report) cb.report(step, verdict, detail);
}
struct Secret {
  Aes128Block bytes{};
  ~Secret() { SecureZeroMemory(bytes.data(), bytes.size()); }
};
} // namespace

void PeroduaP02cWorkflow::run(const FlashJob& job,
                             const FlashWorkflowCallbacks& cb,
                             std::stop_token stop) {
  const auto& p = job.profile;
  if (p.tx_id != 0x714 || p.rx_id != 0x794 || p.functional_id != 0x7DF ||
      p.gateway_tx_id != 0x701 || p.gateway_rx_id != 0x781 || p.can_fd ||
      p.extended_id || p.uds_fd || p.uds_brs || p.nominal_bitrate != 500000 ||
      p.padding != 0xFF || p.isotp_st_min != 20 || p.power_control || p.security_level != 7 ||
      p.security_algorithm != L"aes128_cmac") {
    throw std::runtime_error("Perodua CES004/006/009/010 requires CPD 714/794, gateway 701/781, functional 7DF, Classic CAN 500k, padding FF, native AES-CMAC Level 4 and external power");
  }
  if (job.update_public_key) throw std::runtime_error("Perodua public-key update is not defined by this flash job");
  const auto mode = perodua_mode(job.entry_mode);
  if (p.programming_crc_variant != L"reflected" && p.programming_crc_variant != L"non_reflected")
    throw std::runtime_error("Set programming_crc_variant to reflected or non_reflected from the ECU CRC definition; CES009 does not specify reflection");
  const auto load = [&](std::string name, std::uint32_t address, std::uint32_t length,
                         const std::filesystem::path& file) {
    return PeroduaMemoryBlock{std::move(name), address, length,
                              load_perodua_image(resolve(job, file), address)};
  };
  // Reuse the common S-record parser. Only the selected mode's files are loaded.
  PeroduaImages images;
  images.driver = load("Driver", p.driver_start, p.driver_length, job.driver_file);
  if (mode != PeroduaMode::cal)
    images.modules.push_back(load("APP", p.app_start, p.app_length, job.app_file));
  if (mode != PeroduaMode::app)
    images.modules.push_back(load("CAL", p.cal_start, p.cal_length, job.cal_file));
  const auto now = std::time(nullptr);
  std::tm date{};
  if (localtime_s(&date, &now) != 0) throw std::runtime_error("cannot determine programming date");
  const auto fingerprint = perodua_fingerprint(date, p.programming_tester_identity);
  auto key_path = job.security_key_file.empty() ? p.security_key_file : job.security_key_file;
  key_path = key_path.empty() ? default_oem_key_path() : resolve(job, key_path);
  Secret secret{load_protected_aes128_key(key_path)};
  const auto log_image = [&](const PeroduaMemoryBlock& block) {
    for (const auto& segment : block.image.segments) {
      record(cb, block.name + " input segment", "PASS",
             "address=" + std::to_string(segment.address) + "; bytes=" +
             std::to_string(segment.data.size()));
    }
  };
  log_image(images.driver);
  for (const auto& block : images.modules) log_image(block);
  record(cb, "Specification", "INFO",
         "CES012 V0.2 / CES009 V0.1; DFI00; CRC32=" +
         std::string(p.programming_crc_variant == L"reflected" ? "reflected" : "non_reflected") +
         "; per-segment erase/download/check; vehicle gateway required");
  if (stop.stop_requested()) throw std::runtime_error("Perodua flashing cancelled before CAN access");
  if (!job.can_bus_provider) throw std::runtime_error("CAN bus provider is not configured");
  auto bus = job.can_bus_provider->create(
      {"", p.channel, p.nominal_bitrate, p.data_bitrate, false, L"UDSToolCpp"});

  IsoTpConfig app_config{p.tx_id, p.rx_id, 0xFF, 8, 20, 150ms, 150ms};
  app_config.batch_consecutive_frames = false;
  auto boot_config = app_config;
  boot_config.block_size = 0;
  boot_config.st_min = 0;
  auto gateway_config = app_config;
  gateway_config.tx_id = p.gateway_tx_id;
  gateway_config.rx_id = p.gateway_rx_id;
  auto functional_config = app_config;
  functional_config.tx_id = p.functional_id;
  IsoTpSession app_transport(*bus, app_config), boot_transport(*bus, boot_config),
      gateway_transport(*bus, gateway_config), functional_transport(*bus, functional_config);
  const auto log = [&](const std::string& text) { if (cb.log) cb.log(text); };
  std::stop_source operation_stop;
  std::stop_callback relay_stop(stop, [&] { operation_stop.request_stop(); });
  UdsClient app(app_transport, log, operation_stop.get_token());
  UdsClient boot(boot_transport, log, operation_stop.get_token());
  UdsClient gateway(gateway_transport, log, operation_stop.get_token());
  UdsClient functional_client(functional_transport, log, operation_stop.get_token());
  bool in_boot = false;
  std::mutex error_mutex;
  std::string heartbeat_error;
  // Use a raw functional SingleFrame; no competing receive loop and no
  // fragmented payload. Keep gateway and ECU sessions alive at S3client=2s.
  std::jthread heartbeat([&](std::stop_token sender_stop) {
    auto next = std::chrono::steady_clock::now() + 2s;
    while (!sender_stop.stop_requested() && !operation_stop.stop_requested()) {
      if (std::chrono::steady_clock::now() >= next) {
        try {
          bus->send({p.functional_id, {2, 0x3E, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, false, false, false});
        } catch (const std::exception& error) {
          { std::lock_guard lock(error_mutex); heartbeat_error = error.what(); }
          operation_stop.request_stop();
          return;
        }
        next = std::chrono::steady_clock::now() + 2s;
      }
      std::this_thread::sleep_for(10ms);
    }
  });
  const auto health = [&] {
    std::lock_guard lock(error_mutex);
    if (!heartbeat_error.empty()) throw std::runtime_error("TesterPresent failed: " + heartbeat_error);
    if (operation_stop.stop_requested()) throw std::runtime_error("Perodua flashing cancelled");
  };
  PeroduaIo io;
  io.health_check = health;
  io.request = [&](PeroduaEndpoint endpoint, std::span<const std::uint8_t> request,
                    std::chrono::milliseconds p2, std::chrono::milliseconds p2_star) {
    health();
    auto& client = endpoint == PeroduaEndpoint::gateway ? gateway : (in_boot ? boot : app);
    auto result = client.request(request, p2, p2_star);
    if (endpoint == PeroduaEndpoint::ecu && result.success && request.size() == 2) {
      if (request[0] == 0x10 && request[1] == 2) in_boot = true;
      if (request[0] == 0x11 && request[1] == 1) in_boot = false;
    }
    return result;
  };
  io.functional_send = [&](std::span<const std::uint8_t> request) { functional_client.send_only(request); };
  io.wait = [&](std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
      health();
      std::this_thread::sleep_for(std::min(10ms,
          std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now())));
    }
  };
  io.key = [&](std::span<const std::uint8_t> seed) {
    const auto key = aes128_cmac(secret.bytes, seed);
    return std::vector<std::uint8_t>(key.begin(), key.end());
  };
  io.progress = [&](int percent, const std::string& message) {
    if (percent >= 0 && cb.progress) cb.progress(percent, message);
    if (percent < 0 || message.find("TransferData") == std::string::npos)
      record(cb, "Perodua sequence", "INFO", message);
  };
  PeroduaP02cFlow flow(std::move(io), p.programming_crc_variant == L"reflected");
  try {
    flow.run(images, fingerprint, operation_stop.get_token());
    heartbeat.request_stop();
    heartbeat.join();
    health();
  } catch (...) {
    heartbeat.request_stop();
    heartbeat.join();
    record(cb, "Failure state", "FAIL", flow.programming_completed()
        ? "Programming/dependencies/reset completed; communication restoration did not complete."
        : "Sequence did not complete. ECU/session/network state must be checked; no automatic reset or erase retry was issued.");
    throw;
  }
  record(cb, "Perodua download", "PASS", "Programming, CRC32, dependencies, reset and communication restoration completed");
}
} // namespace uds
