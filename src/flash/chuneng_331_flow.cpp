#include "flash/chuneng_331_flow.hpp"
#include "core/chuneng_arc331_protocol.hpp"
#include "core/hex.hpp"
#include "core/high_resolution_timer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <stop_token>

namespace uds {
namespace {
using namespace std::chrono_literals;
constexpr std::uint32_t kAppAddress = 0x000C0000;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24U));
  out.push_back(static_cast<std::uint8_t>(value >> 16U));
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

std::uint8_t bcd(unsigned value) {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}
} // namespace

Chuneng331EntryPlan resolve_chuneng_331_entry_plan(
    std::wstring_view entry_mode) {
  if (entry_mode == L"app") return {false, true, false};
  if (entry_mode == L"boot") return {false, false, true};
  if (entry_mode == L"ft") return {true, false, false};
  throw std::invalid_argument(
      "Chuneng 331 entry mode must be 'app', 'boot', or 'ft'");
}

std::vector<std::uint8_t> chuneng_331_fingerprint_f184(
    const std::tm& local_time) {
  return {
      bcd(static_cast<unsigned>((local_time.tm_year + 1900) % 100)),
      bcd(static_cast<unsigned>(local_time.tm_mon + 1)),
      bcd(static_cast<unsigned>(local_time.tm_mday)),
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

std::size_t chuneng_331_transfer_block_length(
    std::span<const std::uint8_t> request_download_response) {
  if (request_download_response.size() < 3 ||
      request_download_response[0] != 0x74) {
    throw std::runtime_error("invalid 34 response");
  }
  const auto bytes = static_cast<std::size_t>(
      (request_download_response[1] >> 4U) & 0x0FU);
  if (bytes == 0 || request_download_response.size() < 2U + bytes) {
    throw std::runtime_error("missing max block length");
  }
  std::size_t advertised = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    advertised = (advertised << 8U) | request_download_response[2U + i];
  }
  if (advertised < kChuneng331BlockLength) {
    throw std::runtime_error(
        "ECU max block length is smaller than required 0x802");
  }
  return kChuneng331BlockLength;
}

Chuneng331Flow::Chuneng331Flow(UdsClient& physical, UdsClient& functional,
                               IsoTpSession& physical_transport, IsoTpSession& functional_transport,
                               Log log,
                               KeyGenerator key_generator)
    : physical_(physical), functional_(functional), physical_transport_(physical_transport),
      functional_transport_(functional_transport),
      log_(std::move(log)), key_generator_(std::move(key_generator)) {}

Chuneng331Flow::Chuneng331Flow(
    UdsClient& physical, UdsClient& functional, UdsClient& ft_physical,
    IsoTpSession& physical_transport, IsoTpSession& functional_transport,
    Log log, KeyGenerator key_generator)
    : Chuneng331Flow(physical, functional, physical_transport,
                     functional_transport, std::move(log),
                     std::move(key_generator)) {
  ft_physical_ = &ft_physical;
}

UdsResponse Chuneng331Flow::expect(UdsClient& client, std::span<const std::uint8_t> request,
                                   std::span<const std::uint8_t> prefix, int percent,
                                   const std::string& name) {
  if (log_) log_(percent, name);
  auto result = client.request(request);
  if (!result.success) throw std::runtime_error(name + ": NRC/timeout " + result.detail);
  if (result.response.size() < prefix.size() ||
      !std::equal(prefix.begin(), prefix.end(), result.response.begin())) {
    throw std::runtime_error(name + ": response mismatch " + to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  return result;
}

UdsResponse Chuneng331Flow::expect_routine(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::uint16_t routine_id, int percent, const std::string& name) {
  const auto prefix = chuneng_331_routine_success_prefix(routine_id);
  return expect(client, request, prefix, percent, name);
}

void Chuneng331Flow::send_functional_no_response(
    std::span<const std::uint8_t> request,
    std::chrono::milliseconds delay, int percent,
    std::string_view name) {
  check_cancelled();
  if (log_) log_(percent, std::string(name));
  functional_.send_only(request, stop_);
  std::this_thread::sleep_for(delay);
  check_cancelled();
}

void Chuneng331Flow::enter_programming_session(
    const Chuneng331EntryPlan& entry) {
  const std::array<std::uint8_t, 2> extended_response{0x50, 0x03};
  const std::array<std::uint8_t, 2> programming_response{0x50, 0x02};

  if (entry.boot_only_to_app) {
    // The project specification covers the normal APP entry. The Boot-only
    // recovery entry is bench-evidenced on this ECU as functional 10 01/10 03
    // with the configured physical response ID, then it rejoins the normative
    // flow at physical 10 02. It cannot satisfy APP-only 0203/85/28.
    expect(functional_, std::array<std::uint8_t, 2>{0x10, 0x01},
           std::array<std::uint8_t, 2>{0x50, 0x01}, 2,
           "BOOT 10 01 Functional DefaultSession");
    expect(functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
           extended_response, 4, "BOOT 10 03 Functional ExtendedSession");
    expect(physical_, kChuneng331ProgrammingSession,
           programming_response, 12,
           "BOOT 10 02 ProgrammingSession (skip 0203/85/28)");
    return;
  }

  if (entry.run_standard_preprogramming) {
    // Q/CN A201-2025 appendix C pre-programming order: physical 10 03 with
    // awaited response, physical 31 01 02 03 precondition check, then
    // functional suppressed 10 83 / 85 82 / 28 83 03, then physical 10 02.
    expect(physical_, kChuneng331ExtendedSessionRequest,
           extended_response, 1, "10 03 ExtendedSession");
    // Q/CN A201-2025 appendix B: 31 01 02 03 returns one status byte,
    // 0x04 = conditions met, 0x05 = conditions not met.  The reference
    // CANoe flow performs no 0203 gate, and bench/flash-line ECUs without
    // vehicle signals (speed/gear/EPB) report 0x05.  Accept 0x05 as a WARN
    // and continue so the remaining programming sequence can be exercised;
    // the execution log and HTML report keep the raw routine status.
    {
      const std::array<std::uint8_t, 4> precondition{0x31, 0x01, 0x02, 0x03};
      const std::array<std::uint8_t, 5> passed{0x71, 0x01, 0x02, 0x03, 0x04};
      const std::array<std::uint8_t, 5> not_met{0x71, 0x01, 0x02, 0x03, 0x05};
      if (log_) log_(4, "31 01 02 03 ProgrammingPrecondition");
      auto result = physical_.request(precondition);
      if (!result.success) {
        if (result.nrc == 0x31) {
          throw std::runtime_error(
              "31 01 02 03 ProgrammingPrecondition: NRC 0x31；ECU很可能"
              "在擦除中断后处于Boot/SBL恢复态。不要重复使用APP入口，"
              "请切换“BOOT→APP（仅Boot）”完成恢复。");
        }
        throw std::runtime_error(
            "31 01 02 03 ProgrammingPrecondition: NRC/timeout " +
            result.detail);
      }
      const auto is_passed =
          result.response.size() >= passed.size() &&
          std::equal(passed.begin(), passed.end(), result.response.begin());
      const auto is_not_met =
          result.response.size() >= not_met.size() &&
          std::equal(not_met.begin(), not_met.end(), result.response.begin());
      if (is_passed) {
        if (log_) {
          log_(4, "31 01 02 03 ProgrammingPrecondition PASS: " +
                      to_hex(result.response));
        }
      } else if (is_not_met) {
        if (log_) {
          log_(4, "31 01 02 03 ProgrammingPrecondition WARN: " +
                      to_hex(result.response) +
                      " (刷新条件未满足 0x05，按参考流程继续)");
        }
      } else {
        throw std::runtime_error(
            "31 01 02 03 ProgrammingPrecondition: response mismatch " +
            to_hex(result.response));
      }
    }
    send_functional_no_response(
        kChuneng331FunctionalExtendedSession,
        kChuneng331SessionControlDelay, 2,
        "10 83 Functional ExtendedSession (10 03 suppressed)");
    send_functional_no_response(
        kChuneng331DisableDtc,
        kChuneng331FunctionalControlDelay, 6,
        "85 82 DisableDTC");
    send_functional_no_response(
        kChuneng331DisableCommunication,
        kChuneng331FunctionalControlDelay, 8,
        "28 83 03 DisableCommunication");
    expect(physical_, kChuneng331ProgrammingSession,
           programming_response, 10,
           "APP 10 02 ProgrammingSession");
    return;
  }

  expect(*ft_physical_, kChuneng331ProgrammingSession,
         programming_response, 12,
         "FT 10 02 ProgrammingSession (0x701/0x761)");
}

void Chuneng331Flow::restore_after_reset() {
  send_functional_no_response(
      kChuneng331FunctionalExtendedSession,
      kChuneng331SessionControlDelay, 96,
      "Post-reset 10 83 Functional ExtendedSession");
  send_functional_no_response(
      kChuneng331EnableDtc,
      kChuneng331FunctionalControlDelay, 97,
      "85 81 EnableDTC");
  send_functional_no_response(
      kChuneng331EnableCommunication,
      kChuneng331FunctionalControlDelay, 98,
      "28 80 03 EnableCommunication");
  send_functional_no_response(
      kChuneng331FunctionalDefaultSession,
      kChuneng331SessionControlDelay, 99,
      "10 81 Functional DefaultSession");
  expect(functional_, kChuneng331ClearDtc,
         std::array<std::uint8_t, 1>{0x54}, 100, "ClearDTC");
}

void Chuneng331Flow::check_cancelled() const {
  if (stop_.stop_requested()) throw std::runtime_error("operation cancelled by user");
}

std::vector<std::uint8_t> Chuneng331Flow::fingerprint_f184() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  return chuneng_331_fingerprint_f184(local);
}

void Chuneng331Flow::transfer_image(std::uint32_t address, std::span<const std::uint8_t> image,
                                    int begin_percent, int end_percent, const std::string& label) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, static_cast<std::uint32_t>(image.size()));
  const std::array<std::uint8_t, 1> p74{0x74};
  auto r34 = expect(physical_, request, p74, begin_percent, "34 " + label);
  const auto block_length =
      chuneng_331_transfer_block_length(r34.response);
  const auto chunk_size = block_length - 2U;
  if (log_) {
    log_(begin_percent,
         "34 " + label + " block_length=0x802, TransferData payload=0x800");
  }
  std::size_t offset = 0;
  const auto total_blocks =
      (image.size() + chunk_size - 1U) / chunk_size;
  std::size_t block_index = 0;
  std::uint8_t sequence = 1;
  while (offset < image.size()) {
    check_cancelled();
    ++block_index;
    const auto count = std::min(chunk_size, image.size() - offset);
    std::vector<std::uint8_t> transfer{0x36, sequence};
    transfer.insert(transfer.end(), image.begin() + static_cast<std::ptrdiff_t>(offset),
                    image.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const std::array<std::uint8_t, 2> expected{0x76, sequence};
    const auto percent = begin_percent + static_cast<int>((end_percent - begin_percent) *
                         static_cast<double>(offset + count) / static_cast<double>(image.size()));
    expect(physical_, transfer, expected, percent,
           "36 " + label + " block " + std::to_string(block_index) +
               "/" + std::to_string(total_blocks));
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }
  const std::array<std::uint8_t, 1> req37{0x37};
  const std::array<std::uint8_t, 1> res77{0x77};
  expect(physical_, req37, res77, end_percent, "37 " + label);
}

void Chuneng331Flow::run(const Chuneng331Images& images, std::stop_token stop) {
  run(images, L"app", stop);
}

void Chuneng331Flow::run(const Chuneng331Images& images,
                         std::wstring_view entry_mode,
                         std::stop_token stop) {
  using namespace std::chrono_literals;
  const auto entry = resolve_chuneng_331_entry_plan(entry_mode);
  if (entry.use_ft_endpoint && ft_physical_ == nullptr) {
    throw std::runtime_error("Chuneng 331 FT entry endpoint is not configured");
  }
  stop_ = stop;
  check_cancelled();
  if (images.driver.size() != 0x4000) throw std::runtime_error("Driver must be exactly 16 KiB");
  if (images.app.size() != 0x180000) throw std::runtime_error("APP must be exactly 1536 KiB");
  if (images.driver_verification.size() != 256 || images.app_verification.size() != 256) {
    throw std::runtime_error("verification payload must be 256 bytes");
  }

  std::atomic_bool wake_failed{};
  const std::array<std::uint8_t, 8> wake_frame{};
  physical_transport_.send_raw(kChunengArc331WakeupId, wake_frame);
  if (log_) log_(0, "TX [0x520] 00 00 00 00 00 00 00 00 (initial wake-up)");
  std::jthread wake_sender([this, wake_frame, &wake_failed](std::stop_token stop) {
    ScopedHighResolutionTimer timer_resolution;
    auto next = std::chrono::steady_clock::now() +
                kChunengArc331WakeupPeriod;
    while (!stop.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next) {
        try {
          physical_transport_.send_raw(kChunengArc331WakeupId, wake_frame);
        } catch (...) {
          wake_failed.store(true);
          return;
        }
        do {
          next += kChunengArc331WakeupPeriod;
        } while (next <= now);
      }
      std::this_thread::sleep_for(1ms);
    }
  });
  if (log_) {
    log_(0, "ChuNeng ARC331 periodic 0x520 wake-up active (" +
                std::to_string(kChunengArc331WakeupPeriod.count()) +
                " ms)");
  }

  const auto check_wakeup = [&wake_failed] {
    if (wake_failed.load()) {
      throw std::runtime_error("periodic 0x520 wake-up transmission failed");
    }
  };

  std::jthread tester_present([this](std::stop_token stop) {
    const std::array<std::uint8_t, 8> frame{0x02,0x3E,0x80,0,0,0,0,0};
    auto next = std::chrono::steady_clock::now() +
                kChuneng331TesterPresentPeriod;
    while (!stop.stop_requested()) {
      if (std::chrono::steady_clock::now() >= next) {
        try {
          functional_transport_.send_raw(functional_transport_.tx_id(), frame);
          if (log_) log_(0, "TX [functional] 02 3E 80 00 00 00 00 00 (raw TesterPresent)");
        }
        catch (...) { if (log_) log_(0, "TesterPresent sender failed"); }
        next += kChuneng331TesterPresentPeriod;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  std::this_thread::sleep_for(1s);
  check_cancelled();

  enter_programming_session(entry);
  check_wakeup();

  const std::array<std::uint8_t, 2> seed_req{0x27, 0x11}, seed_prefix{0x67, 0x11};
  auto seed_result = expect(physical_, seed_req, seed_prefix, 15, "27 11 RequestSeed");
  check_cancelled();
  if (seed_result.response.size() != 18) throw std::runtime_error("331 seed must be 16 bytes");
  const auto seed = std::span(seed_result.response).subspan(2);
  auto key = key_generator_(seed);
  if (key.size() != 16) throw std::runtime_error("331 key must be 16 bytes");
  std::vector<std::uint8_t> key_request{0x27, 0x12};
  key_request.insert(key_request.end(), key.begin(), key.end());
  const std::array<std::uint8_t, 2> key_prefix{0x67, 0x12};
  expect(physical_, key_request, key_prefix, 18, "27 12 SendKey");
  // Flash20230727.can::DownloadFlow waits 500 ms after security unlock before
  // continuing the Driver download path.
  std::this_thread::sleep_for(500ms);
  check_cancelled();

  const auto write_fingerprint = [this] {
    auto fingerprint = fingerprint_f184();
    std::vector<std::uint8_t> f184{0x2E, 0xF1, 0x84};
    f184.insert(f184.end(), fingerprint.begin(), fingerprint.end());
    const std::array<std::uint8_t, 3> f184_prefix{0x6E, 0xF1, 0x84};
    expect(physical_, f184, f184_prefix, 35,
           "2E F184 Fingerprint (9 bytes)");
  };

  transfer_image(images.driver_address, images.driver, 22, 30, "Driver");
  check_cancelled();
  // Reference ChuNeng flow (Flash2944_CN_ARC_V1.2): download the 44-byte
  // Driver ABT block right after the main image, before CheckMemory 0202.
  if (!images.driver_abt.empty()) {
    transfer_image(images.driver_abt_address, images.driver_abt, 31, 31,
                   "DriverABT");
    check_cancelled();
  }
  std::vector<std::uint8_t> driver_verify{0x31,0x01,0x02,0x02};
  driver_verify.insert(driver_verify.end(), images.driver_verification.begin(), images.driver_verification.end());
  expect_routine(physical_, driver_verify, 0x0202, 32,
                 "DriverVerification");
  expect_routine(physical_,
                  std::array<std::uint8_t,4>{0x31,0x01,0x03,0x01},
                  0x0301, 34, "ActivateSBL");

  // Q/CN A201-2025 5.4.5: after SBL activation the diagnostic tool must
  // write the fingerprint into non-volatile storage before any erase or
  // download step; the erase routine returns NRC 0x22 when no valid
  // fingerprint is present. Written here for both CBF and S-record inputs.
  write_fingerprint();
  check_wakeup();

  // Driver verification uses only the 256-byte device signature extracted
  // from CBF (or selected explicitly for S-record mode). A 1322-byte LP
  // certificate must never reach RoutineControl 0202.
  check_wakeup();

  std::vector<std::uint8_t> erase{0x31,0x01,0xFF,0x00,0x44};
  append_u32(erase, kAppAddress); append_u32(erase, static_cast<std::uint32_t>(images.app.size()));
  expect_routine(physical_, erase, 0xFF00, 36, "EraseAPP");
  check_wakeup();
  transfer_image(kAppAddress, images.app, 38, 88, "APP");
  check_cancelled();
  // Reference ChuNeng flow: download the 44-byte APP ABT block right after
  // the APP image, before CheckMemory 0202.
  if (!images.app_abt.empty()) {
    transfer_image(images.app_abt_address, images.app_abt, 89, 90,
                   "APPABT");
    check_cancelled();
  }

  std::vector<std::uint8_t> app_verify{0x31,0x01,0x02,0x02};
  app_verify.insert(app_verify.end(), images.app_verification.begin(), images.app_verification.end());
  expect_routine(physical_, app_verify, 0x0202, 91,
                 "AppVerification");
  expect_routine(physical_,
                 std::array<std::uint8_t,4>{0x31,0x01,0xFF,0x01},
                 0xFF01, 93, "DependencyCheck");
  expect(physical_, std::array<std::uint8_t,2>{0x11,0x01},
         std::array<std::uint8_t,2>{0x51,0x01}, 95, "ECUReset");
  std::this_thread::sleep_for(6s);
  restore_after_reset();
  check_wakeup();
}

} // namespace uds
