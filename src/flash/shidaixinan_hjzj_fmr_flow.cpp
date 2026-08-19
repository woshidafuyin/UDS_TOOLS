#include "flash/shidaixinan_hjzj_fmr_flow.hpp"

#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uds {
namespace {
using namespace std::chrono_literals;

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> crc_routine(std::uint32_t crc) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xF1, 0xA0};
  append_u32(request, crc);
  return request;
}
} // namespace

CanFrame shidaixinan_hjzj_wakeup_frame() {
  return {0x425,
          {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
          false, true, true};
}

CanFrame shidaixinan_hjzj_tester_present_frame() {
  return {0x7DF,
          {0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00},
          false, true, false};
}

std::uint32_t shidaixinan_hjzj_crc32(
    std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const auto byte : data) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U
                             : crc >> 1U;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> shidaixinan_hjzj_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> shidaixinan_hjzj_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::size_t shidaixinan_hjzj_max_block_length(
    std::span<const std::uint8_t> response) {
  if (response.size() < 2U || response[0] != 0x74) {
    throw std::runtime_error("时代新安 34 response is invalid");
  }
  const auto length_bytes =
      static_cast<std::size_t>((response[1] >> 4U) & 0x0FU);
  if (length_bytes == 0U || response.size() < 2U + length_bytes) {
    throw std::runtime_error(
        "时代新安 34 response has no maxNumberOfBlockLength");
  }
  std::size_t value = 0;
  for (std::size_t index = 0; index < length_bytes; ++index) {
    value = (value << 8U) | response[2U + index];
  }
  if (value <= 2U || value > 4095U) {
    throw std::runtime_error(
        "时代新安 34 maxNumberOfBlockLength is out of range");
  }
  return value;
}

ShidaixinanHjzjFmrFlow::ShidaixinanHjzjFmrFlow(
    UdsClient& physical, UdsClient& functional,
    UdsClient& ft_functional, Log log, Log progress,
    KeyGenerator key_generator, HealthCheck health_check,
    ShidaixinanHjzjFmrTiming timing)
    : physical_(physical), functional_(functional),
      ft_functional_(ft_functional), log_(std::move(log)),
      progress_(std::move(progress)),
      key_generator_(std::move(key_generator)),
      health_check_(std::move(health_check)), timing_(timing) {}

bool ShidaixinanHjzjFmrFlow::core_programming_completed() const noexcept {
  return core_programming_completed_;
}

void ShidaixinanHjzjFmrFlow::check_cancelled() const {
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
  if (health_check_) health_check_();
}

void ShidaixinanHjzjFmrFlow::settle_for(
    std::chrono::milliseconds duration, int percent,
    const std::string& name) const {
  if (log_) {
    log_(percent, name + ": fixed settle " +
                      std::to_string(duration.count()) + " ms");
  }
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 10ms));
  }
  check_cancelled();
}

UdsResponse ShidaixinanHjzjFmrFlow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> expected, int percent,
    const std::string& name, bool exact, bool settle_after,
    bool emit_log) {
  check_cancelled();
  if (log_ && emit_log) log_(percent, name);
  UdsResponse response;
  try {
    response = client.request(request, timing_.p2, timing_.p2_star);
  } catch (const std::exception& error) {
    throw std::runtime_error(name + ": " + error.what());
  }
  check_cancelled();
  if (!response.success) {
    throw std::runtime_error(name + ": NRC/timeout " +
                             to_hex(response.response));
  }
  const auto prefix =
      response.response.size() >= expected.size() &&
      std::equal(expected.begin(), expected.end(),
                 response.response.begin());
  if (!prefix || (exact && response.response.size() != expected.size())) {
    throw std::runtime_error(name + ": response mismatch, got " +
                             to_hex(response.response));
  }
  if (log_ && emit_log) {
    log_(percent, name + " PASS: " + to_hex(response.response));
  }
  if (settle_after) {
    settle_for(step_delay(), percent,
               name + " -> next step");
  }
  return response;
}

std::chrono::milliseconds
ShidaixinanHjzjFmrFlow::step_delay() const noexcept {
  return entry_mode_ == ShidaixinanHjzjFmrEntryMode::ft
             ? timing_.ft_step_delay
             : timing_.app_step_delay;
}

void ShidaixinanHjzjFmrFlow::unlock(
    std::uint8_t seed_subfunction, std::uint8_t key_subfunction,
    unsigned algorithm_level, int percent) {
  const std::array<std::uint8_t, 2> seed_request{
      0x27, seed_subfunction};
  const std::array<std::uint8_t, 2> seed_expected{
      0x67, seed_subfunction};
  const auto seed =
      expect(physical_, seed_request, seed_expected, percent,
             "27 " + to_hex(std::span(&seed_subfunction, 1), false) +
                 " RequestSeed");
  if (seed.response.size() != 6U) {
    throw std::runtime_error(
        "时代新安 SecurityAccess seed must be 4 bytes");
  }
  const auto key = key_generator_(
      std::span(seed.response).subspan(2, 4), algorithm_level);
  if (key.size() != 4U) {
    throw std::runtime_error(
        "时代新安 SecurityAccess key must be 4 bytes");
  }
  std::vector<std::uint8_t> key_request{0x27, key_subfunction};
  key_request.insert(key_request.end(), key.begin(), key.end());
  const std::array<std::uint8_t, 2> key_expected{
      0x67, key_subfunction};
  expect(physical_, key_request, key_expected, percent + 2,
         "27 " + to_hex(std::span(&key_subfunction, 1), false) +
             " SendKey",
         true);
}

void ShidaixinanHjzjFmrFlow::transfer(
    const SRecordSegment& image, int begin_percent, int end_percent,
    const std::string& name) {
  if (image.data.empty() ||
      image.data.size() >
          static_cast<std::size_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error(name + " S19 data length is invalid");
  }
  const auto length = static_cast<std::uint32_t>(image.data.size());
  const auto download =
      shidaixinan_hjzj_request_download(image.address, length);
  const auto response =
      expect(physical_, download, std::array<std::uint8_t, 1>{0x74},
             begin_percent, "34 " + name);
  const auto max_block =
      shidaixinan_hjzj_max_block_length(response.response);
  const auto chunk_size = max_block - 2U;
  const auto block_count =
      (image.data.size() + chunk_size - 1U) / chunk_size;

  std::size_t offset = 0;
  std::size_t block_index = 0;
  std::uint8_t sequence = 1;
  int last_logged_percent = begin_percent;
  while (offset < image.data.size()) {
    check_cancelled();
    const auto count =
        std::min(chunk_size, image.data.size() - offset);
    std::vector<std::uint8_t> block{0x36, sequence};
    block.insert(
        block.end(),
        image.data.begin() + static_cast<std::ptrdiff_t>(offset),
        image.data.begin() +
            static_cast<std::ptrdiff_t>(offset + count));
    ++block_index;
    const auto percent =
        begin_percent +
        static_cast<int>(
            (end_percent - begin_percent) *
            static_cast<double>(offset + count) /
            static_cast<double>(image.data.size()));
    expect(physical_, block,
           std::array<std::uint8_t, 2>{0x76, sequence}, percent,
           "36 " + name + " block " +
               std::to_string(block_index) + "/" +
               std::to_string(block_count),
           true, false, false);
    offset += count;
    const auto progress_line =
        "36 " + name + " progress: " +
        std::to_string(block_index) + "/" +
        std::to_string(block_count) + " blocks, " +
        std::to_string(offset) + "/" +
        std::to_string(image.data.size()) + " bytes";
    if (progress_) progress_(percent, progress_line);
    if (log_ &&
        (percent > last_logged_percent ||
         offset == image.data.size())) {
      log_(percent, progress_line);
      last_logged_percent = percent;
    }
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }
  if (log_) {
    log_(end_percent,
         "TransferData (0x36) " + name + " PASS: blocks=" +
             std::to_string(block_count) + ", bytes=" +
             std::to_string(image.data.size()) +
             ", max-data-per-block=" +
             std::to_string(chunk_size));
  }
  settle_for(step_delay(), end_percent,
             "last 36 " + name + " -> 37");
  expect(physical_, std::array<std::uint8_t, 1>{0x37},
         std::array<std::uint8_t, 1>{0x77}, end_percent,
         "37 " + name, true);
}

void ShidaixinanHjzjFmrFlow::run_app_preamble() {
  expect(functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 5,
         "FUN 10 03 ExtendedSession");
  unlock(0x01, 0x02, 0x01, 8);
  expect(physical_,
         std::array<std::uint8_t, 4>{0x31, 0x01, 0xF0, 0x02},
         std::array<std::uint8_t, 5>{
             0x71, 0x01, 0xF0, 0x02, 0x00},
         13, "31 F002 CheckProgrammingPreconditions", true);
  expect(functional_, std::array<std::uint8_t, 2>{0x85, 0x02},
         std::array<std::uint8_t, 2>{0xC5, 0x02}, 15,
         "FUN 85 02 DisableDTC", true);
  expect(functional_,
         std::array<std::uint8_t, 3>{0x28, 0x03, 0x03},
         std::array<std::uint8_t, 2>{0x68, 0x03}, 17,
         "FUN 28 03 03 DisableCommunication", true);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 19,
         "10 02 ProgrammingSession");
}

void ShidaixinanHjzjFmrFlow::wait_for_ft_physical_programming_session() {
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + timing_.ft_entry_window;
  std::string last_error{"no physical 50 02 response"};
  unsigned attempt{};
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    ++attempt;
    if (log_) {
      log_(19, "FT 10 02 physical gate attempt " +
                   std::to_string(attempt));
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const auto timeout =
        std::max(1ms, std::min(timing_.p2, remaining));
    try {
      const auto response = physical_.request(
          std::array<std::uint8_t, 2>{0x10, 0x02}, timeout,
          timing_.p2_star, stop_);
      const auto valid =
          response.success && response.response.size() >= 2U &&
          response.response[0] == 0x50 &&
          response.response[1] == 0x02;
      if (valid) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        if (log_) {
          log_(19, "FT physical 10 02 PASS: " +
                       to_hex(response.response) + ", elapsed=" +
                       std::to_string(elapsed.count()) + " ms");
        }
        settle_for(step_delay(), 19,
                   "FT physical 10 02 -> SecurityAccess");
        return;
      }
      last_error =
          response.success
              ? "response mismatch " + to_hex(response.response)
              : "NRC/timeout " + to_hex(response.response);
    } catch (const std::exception& error) {
      last_error = error.what();
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    if (log_) {
      log_(19, "FT physical 10 02 not ready: " + last_error);
    }
    const auto retry_delay = std::min(
        timing_.ft_retry_delay,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()));
    settle_for(retry_delay, 19, "FT physical 10 02 retry");
  }
  throw std::runtime_error(
      "FT entry failed: physical 10 02 did not receive 50 02 "
      "within " +
      std::to_string(timing_.ft_entry_window.count()) +
      " ms; last=" + last_error);
}

void ShidaixinanHjzjFmrFlow::run_ft_preamble() {
  expect(ft_functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 5,
         "FT FUN 7DF->761 10 03 ExtendedSession");
  check_cancelled();
  if (log_) {
    log_(13,
         "FT FUN 7DF 10 02: send-only; positive response is not expected");
  }
  try {
    ft_functional_.send_only(
        std::array<std::uint8_t, 2>{0x10, 0x02}, stop_);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("FT FUN 7DF 10 02 send failed: ") +
        error.what());
  }
  wait_for_ft_physical_programming_session();
}

void ShidaixinanHjzjFmrFlow::run_programming_body(
    const ShidaixinanHjzjFmrImages& images) {
  unlock(0x03, 0x04, 0x03, 21);
  expect(
      physical_,
      std::array<std::uint8_t, 13>{
          0x2E, 0xF1, 0x84, 0x11, 0x22, 0x33, 0x44,
          0x55, 0x66, 0x77, 0x88, 0x99, 0xAA},
      std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x84}, 25,
      "2E F184 WriteFingerprint", true);

  transfer(images.driver, 28, 38, "Driver");
  const auto driver_crc =
      shidaixinan_hjzj_crc32(images.driver.data);
  expect(physical_, crc_routine(driver_crc),
         std::array<std::uint8_t, 5>{
             0x71, 0x01, 0xF1, 0xA0, 0x00},
         40, "31 F1A0 Driver CRC32", true);

  if (images.app.data.size() >
      static_cast<std::size_t>(
          std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("时代新安 APP S19 data length is invalid");
  }
  const auto app_length =
      static_cast<std::uint32_t>(images.app.data.size());
  expect(physical_,
         shidaixinan_hjzj_erase_memory(images.app.address,
                                       app_length),
         std::array<std::uint8_t, 5>{
             0x71, 0x01, 0xFF, 0x00, 0x00},
         43, "31 FF00 Erase APP", true);
  transfer(images.app, 45, 91, "APP");
  const auto app_crc = shidaixinan_hjzj_crc32(images.app.data);
  expect(physical_, crc_routine(app_crc),
         std::array<std::uint8_t, 5>{
             0x71, 0x01, 0xF1, 0xA0, 0x00},
         94, "31 F1A0 APP CRC32", true);
  expect(physical_,
         std::array<std::uint8_t, 4>{
             0x31, 0x01, 0xFF, 0x01},
         std::array<std::uint8_t, 5>{
             0x71, 0x01, 0xFF, 0x01, 0x00},
         97, "31 FF01 CheckDependencies", true);
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 99,
         "11 01 ECUReset", true);
  core_programming_completed_ = true;
}

void ShidaixinanHjzjFmrFlow::wait_for_ft_post_reset_session() {
  const auto deadline =
      std::chrono::steady_clock::now() +
      timing_.ft_post_reset_ready_window;
  std::string last_error{"no functional 50 03 response"};
  unsigned attempt{};
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    ++attempt;
    if (log_) {
      log_(99, "FT post-reset 10 03 readiness attempt " +
                   std::to_string(attempt));
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const auto timeout =
        std::max(1ms, std::min(timing_.p2, remaining));
    try {
      const auto response = functional_.request(
          std::array<std::uint8_t, 2>{0x10, 0x03}, timeout,
          timing_.p2_star, stop_);
      const auto valid =
          response.success && response.response.size() >= 2U &&
          response.response[0] == 0x50 &&
          response.response[1] == 0x03;
      if (valid) {
        if (log_) {
          log_(99, "FT post-reset FUN 10 03 PASS: " +
                       to_hex(response.response));
        }
        settle_for(step_delay(), 99,
                   "FT post-reset 10 03 -> cleanup");
        return;
      }
      last_error =
          response.success
              ? "response mismatch " + to_hex(response.response)
              : "NRC/timeout " + to_hex(response.response);
    } catch (const std::exception& error) {
      last_error = error.what();
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    if (log_) {
      log_(99, "FT post-reset 10 03 not ready: " + last_error);
    }
    const auto retry_delay = std::min(
        timing_.ft_retry_delay,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()));
    settle_for(retry_delay, 99, "FT post-reset 10 03 retry");
  }
  throw std::runtime_error(
      "FT post-reset cleanup failed: functional 10 03 did not "
      "receive 50 03 within " +
      std::to_string(
          timing_.ft_post_reset_ready_window.count()) +
      " ms; last=" + last_error);
}

void ShidaixinanHjzjFmrFlow::run_ft_cleanup() {
  wait_for_ft_post_reset_session();
  expect(functional_,
         std::array<std::uint8_t, 3>{0x28, 0x00, 0x03},
         std::array<std::uint8_t, 2>{0x68, 0x00}, 99,
         "FT FUN 28 00 03 EnableCommunication", false);
  expect(physical_, std::array<std::uint8_t, 2>{0x85, 0x01},
         std::array<std::uint8_t, 2>{0xC5, 0x01}, 99,
         "FT 85 01 EnableDTC", true);
  expect(functional_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 99,
         "FT FUN 10 01 DefaultSession");
  expect(physical_,
         std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, 100,
         "FT 14 FF FF FF ClearDiagnosticInformation", true,
         false);
}

void ShidaixinanHjzjFmrFlow::run(
    const ShidaixinanHjzjFmrImages& images,
    ShidaixinanHjzjFmrEntryMode entry_mode,
    std::stop_token stop) {
  stop_ = stop;
  entry_mode_ = entry_mode;
  core_programming_completed_ = false;
  if (images.driver.data.empty() || images.app.data.empty()) {
    throw std::runtime_error(
        "时代新安 Driver/APP S19 has no data");
  }

  if (entry_mode_ == ShidaixinanHjzjFmrEntryMode::ft) {
    run_ft_preamble();
  } else {
    run_app_preamble();
  }
  run_programming_body(images);
  if (entry_mode_ == ShidaixinanHjzjFmrEntryMode::ft) {
    run_ft_cleanup();
  }
  if (progress_) {
    progress_(
        100,
        entry_mode_ == ShidaixinanHjzjFmrEntryMode::ft
            ? "时代新安 HJZJ_FMR FT流程完成"
            : "时代新安 HJZJ_FMR APP流程完成");
  }
}

} // namespace uds
