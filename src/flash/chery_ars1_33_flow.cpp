#include "flash/chery_ars1_33_flow.hpp"

#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {
using namespace std::chrono_literals;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24U));
  out.push_back(static_cast<std::uint8_t>(value >> 16U));
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

std::uint8_t bcd(unsigned value) {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

void require_image_size(std::span<const std::uint8_t> image,
                        std::size_t expected, const char* label) {
  if (image.size() != expected) {
    throw std::runtime_error(std::string("ARS1.33 ") + label +
                             " length does not match configured flash layout");
  }
}

void require_verification_size(std::span<const std::uint8_t> verification,
                               const char* label) {
  if (verification.size() != 512U) {
    throw std::runtime_error(std::string("ARS1.33 ") + label +
                             " RSA verification payload must be exactly 512 bytes");
  }
}
} // namespace

std::array<CheryArs133PreconditionFrame, 3>
chery_ars133_precondition_frames() {
  return {{
      {CanFrame{0x600, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                false, false, false},
       std::chrono::milliseconds{100}},
      {CanFrame{0x25B, {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00},
                false, false, false},
       std::chrono::milliseconds{20}},
      {CanFrame{0x4B4, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10},
                false, false, false},
       std::chrono::milliseconds{100}},
  }};
}

CanFrame chery_ars133_app_tester_present_frame(
    std::uint32_t functional_id) {
  return {functional_id,
          {0x02, 0x3E, 0x80, 0x55, 0x55, 0x55, 0x55, 0x55},
          false, false, false};
}

CheryArs133DownloadPlan resolve_chery_ars133_download_plan(
    std::wstring_view entry_mode) {
  if (entry_mode.empty() || entry_mode == L"app_cal") {
    return {CheryArs133FlashMode::AppCal, true, true, false};
  }
  if (entry_mode == L"app") {
    return {CheryArs133FlashMode::AppOnly, true, false, true};
  }
  if (entry_mode == L"cal") {
    return {CheryArs133FlashMode::CalOnly, false, true, false};
  }
  throw std::invalid_argument("unsupported Chery ARS1.33 flashing mode");
}

std::vector<std::uint8_t> chery_ars133_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> chery_ars133_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::size_t chery_ars133_max_block_length(
    std::span<const std::uint8_t> response) {
  if (response.size() < 2 || response[0] != 0x74) {
    throw std::runtime_error("invalid RequestDownload response");
  }
  const auto length_bytes =
      static_cast<std::size_t>((response[1] >> 4U) & 0x0FU);
  if (length_bytes == 0 || response.size() < 2U + length_bytes) {
    throw std::runtime_error(
        "RequestDownload response has no max block length");
  }
  std::size_t value = 0;
  for (std::size_t i = 0; i < length_bytes; ++i) {
    value = (value << 8U) | response[2 + i];
  }
  // Flash_ARS1.33.can falls back to 0x400 when the ECU reports zero. Its
  // manual ISO-TP sender is limited to a 12-bit payload, so retain that cap.
  if (value == 0) value = 0x400;
  value = std::min<std::size_t>(value, 0x0FFF);
  if (value <= 2) {
    throw std::runtime_error("ECU max block length is too small");
  }
  return value;
}

CheryArs133Flow::CheryArs133Flow(UdsClient& physical,
                                 UdsClient& functional,
                                 CheryArs133Layout layout, Log log,
                                 KeyGenerator key_generator,
                                 HealthCheck health_check)
    : physical_(physical), functional_(functional), layout_(layout),
      log_(std::move(log)), key_generator_(std::move(key_generator)),
      health_check_(std::move(health_check)) {}

void CheryArs133Flow::check_cancelled() const {
  if (health_check_) health_check_();
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

void CheryArs133Flow::wait_cancellable(
    std::chrono::milliseconds duration) const {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 50ms));
  }
}

UdsResponse CheryArs133Flow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> prefix, int percent,
    const std::string& name, std::chrono::milliseconds p2,
    std::chrono::milliseconds p2_star) {
  check_cancelled();
  if (log_) log_(percent, name);
  auto result = client.request(request, p2, p2_star);
  check_cancelled();
  if (!result.success) {
    throw std::runtime_error(name + ": NRC/timeout " + result.detail);
  }
  if (result.response.size() < prefix.size() ||
      !std::equal(prefix.begin(), prefix.end(), result.response.begin())) {
    throw std::runtime_error(name + ": response mismatch " +
                             to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  // TxMsgSrever() in the CAPL reference leaves 50 ms between services.
  wait_cancellable(50ms);
  return result;
}

UdsResponse CheryArs133Flow::expect_routine(
    std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> prefix, int percent,
    const std::string& name) {
  auto result = expect(physical_, request, prefix, percent, name);
  if (result.response.size() < 5 || result.response[4] != 0x00) {
    throw std::runtime_error(name + ": routine status is not 0x00: " +
                             to_hex(result.response));
  }
  return result;
}

std::vector<std::uint8_t> CheryArs133Flow::fingerprint_f184() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::vector<std::uint8_t> result(19, 0xFF);
  result[0] = bcd(static_cast<unsigned>((local.tm_year + 1900) % 100));
  result[1] = bcd(static_cast<unsigned>(local.tm_mon + 1));
  result[2] = bcd(static_cast<unsigned>(local.tm_mday));
  return result;
}

void CheryArs133Flow::transfer_image(
    std::uint32_t address, std::span<const std::uint8_t> image,
    int begin_percent, int end_percent, const std::string& label) {
  if (image.empty()) {
    throw std::runtime_error(label + " image is empty");
  }
  const std::array<std::uint8_t, 1> p74{0x74};
  auto download = chery_ars133_request_download(
      address, static_cast<std::uint32_t>(image.size()));
  const auto response =
      expect(physical_, download, p74, begin_percent, "34 " + label);
  const auto chunk_size =
      chery_ars133_max_block_length(response.response) - 2U;
  std::size_t offset = 0;
  std::uint8_t sequence = 1;
  while (offset < image.size()) {
    check_cancelled();
    const auto count = std::min(chunk_size, image.size() - offset);
    std::vector<std::uint8_t> transfer{0x36, sequence};
    transfer.insert(
        transfer.end(), image.begin() + static_cast<std::ptrdiff_t>(offset),
        image.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const std::array<std::uint8_t, 2> expected{0x76, sequence};
    const auto percent =
        begin_percent +
        static_cast<int>((end_percent - begin_percent) *
                         static_cast<double>(offset + count) /
                         static_cast<double>(image.size()));
    expect(physical_, transfer, expected, percent, "36 " + label);
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }
  const std::array<std::uint8_t, 1> request_exit{0x37};
  const std::array<std::uint8_t, 1> response_exit{0x77};
  expect(physical_, request_exit, response_exit, end_percent,
         "37 " + label);
}

void CheryArs133Flow::unlock_security(int begin_percent) {
  const std::array<std::uint8_t, 2> seed_request{0x27, 0x11};
  const std::array<std::uint8_t, 2> seed_prefix{0x67, 0x11};
  auto seed_result = expect(physical_, seed_request, seed_prefix,
                            begin_percent, "27 11 RequestSeed");
  if (seed_result.response.size() != 18) {
    throw std::runtime_error("ARS1.33 seed must be exactly 16 bytes");
  }
  const auto seed = std::span(seed_result.response).subspan(2);
  auto key = key_generator_(seed);
  if (key.size() != 16) {
    throw std::runtime_error("ARS1.33 key must be exactly 16 bytes");
  }
  std::vector<std::uint8_t> key_request{0x27, 0x12};
  key_request.insert(key_request.end(), key.begin(), key.end());
  expect(physical_, key_request,
         std::array<std::uint8_t, 2>{0x67, 0x12}, begin_percent + 3,
         "27 12 SendKey");
}

void CheryArs133Flow::write_fingerprint(int percent) {
  auto fingerprint = fingerprint_f184();
  std::vector<std::uint8_t> request{0x2E, 0xF1, 0x84};
  request.insert(request.end(), fingerprint.begin(), fingerprint.end());
  expect(physical_, request,
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x84}, percent,
         "2E F184 Fingerprint");
}

void CheryArs133Flow::transfer_driver(const CheryArs133Images& images,
                                      int begin_percent,
                                      int end_percent) {
  const auto split = begin_percent + (end_percent - begin_percent) / 5;
  transfer_image(layout_.driver0_start, images.driver0, begin_percent, split,
                 "FLD block0");
  transfer_image(layout_.driver_start, images.driver, split + 1, end_percent,
                 "FLD");
}

void CheryArs133Flow::verify_driver(const CheryArs133Images& images,
                                    int percent) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xDD, 0x02};
  request.insert(request.end(), images.driver_verification.begin(),
                 images.driver_verification.end());
  expect_routine(request,
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xDD, 0x02},
                 percent, "31 01 DD02 FLD RSA verification");
}

void CheryArs133Flow::verify_app(const CheryArs133Images& images,
                                 int percent) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xDD, 0x02};
  request.insert(request.end(), images.app_verification.begin(),
                 images.app_verification.end());
  expect_routine(request,
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xDD, 0x02},
                 percent, "31 01 DD02 APP RSA verification");
}

void CheryArs133Flow::verify_cal(const CheryArs133Images& images,
                                 int percent) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xDD, 0x02};
  request.insert(request.end(), images.cal_verification.begin(),
                 images.cal_verification.end());
  expect_routine(request,
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xDD, 0x02},
                 percent, "31 01 DD02 CAL RSA verification");
}

void CheryArs133Flow::check_programming_dependencies(int percent) {
  expect_routine(
      std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
      std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x01}, percent,
      "31 01 FF01 CheckProgrammingDependencies");
}

void CheryArs133Flow::hard_reset(
    int percent, std::chrono::milliseconds post_reset_wait) {
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, percent,
         "11 01 HardReset");
  wait_cancellable(post_reset_wait);
}

void CheryArs133Flow::clear_dtc(int percent) {
  expect(functional_,
         std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, percent,
         "14 FFFFFF ClearDTC");
  // CAPL server_14() waits another two seconds after the positive response.
  wait_cancellable(2000ms);
}

void CheryArs133Flow::run_app_cal(const CheryArs133Images& images) {
  // Flash_ARS1.33.can::maintest() APPAndCAL -> TC_2.
  if (log_) log_(0, "APPAndCAL/TC_2 initial delay");
  wait_cancellable(kCheryArs133WakeupSettle);

  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession");
  expect_routine(
      std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
      std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03}, 5,
      "31 01 0203 CheckProgrammingPrecondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 8,
         "10 02 ProgrammingSession");
  unlock_security(12);
  write_fingerprint(17);

  wait_cancellable(2000ms);
  transfer_driver(images, 18, 35);
  wait_cancellable(2000ms);
  verify_driver(images, 38);

  wait_cancellable(2000ms);
  const auto erase_app =
      chery_ars133_erase_memory(layout_.app_start, layout_.app_length);
  expect_routine(
      erase_app, std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
      42, "31 01 FF00 EraseAPP");
  transfer_image(layout_.app_start, images.app, 45, 70, "APP");
  wait_cancellable(2000ms);
  verify_app(images, 73);

  const auto erase_cal =
      chery_ars133_erase_memory(layout_.cal_start, layout_.cal_length);
  expect_routine(
      erase_cal, std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
      76, "31 01 FF00 EraseCAL");
  transfer_image(layout_.cal_start, images.cal, 78, 88, "CAL");
  wait_cancellable(2000ms);
  verify_cal(images, 92);

  wait_cancellable(2000ms);
  check_programming_dependencies(95);
  hard_reset(97, 2000ms);
  clear_dtc(99);
  if (log_) log_(100, "APPAndCAL/TC_2 completed");
}

void CheryArs133Flow::run_app_only(const CheryArs133Images& images) {
  // Flash_ARS1.33.can::maintest() APP -> Download().
  if (log_) log_(0, "APP-only/Download wake-up settle (1 s)");
  wait_cancellable(kCheryArs133WakeupSettle);
  functional_.send_only(std::array<std::uint8_t, 2>{0x10, 0x83});
  if (log_) {
    log_(1, "10 83 Functional ExtendedSession (suppressed response)");
  }
  wait_cancellable(kCheryArs133SuppressedSessionSettle);

  expect_routine(
      std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
      std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03}, 4,
      "31 01 0203 CheckProgrammingPrecondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 7,
         "10 02 ProgrammingSession");
  wait_cancellable(2200ms);
  unlock_security(10);
  write_fingerprint(15);

  transfer_driver(images, 17, 34);
  wait_cancellable(1000ms);
  verify_driver(images, 37);
  wait_cancellable(1000ms);

  const auto erase_app =
      chery_ars133_erase_memory(layout_.app_start, layout_.app_length);
  expect_routine(
      erase_app, std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
      41, "31 01 FF00 EraseAPP");
  transfer_image(layout_.app_start, images.app, 44, 82, "APP");
  wait_cancellable(1000ms);
  verify_app(images, 85);

  wait_cancellable(1800ms);
  check_programming_dependencies(90);
  wait_cancellable(1800ms);
  // CAPL server_11() waits 2 s, then Download() waits another 7 s.
  hard_reset(94, 9000ms);

  functional_.send_only(std::array<std::uint8_t, 2>{0x10, 0x81});
  if (log_) {
    log_(97, "10 81 Functional DefaultSession (suppressed response)");
  }
  wait_cancellable(kCheryArs133SuppressedSessionSettle);
  clear_dtc(99);
  if (log_) log_(100, "APP-only/Download completed");
}

void CheryArs133Flow::run_cal_only(const CheryArs133Images& images) {
  // Flash_ARS1.33.can::maintest() CAL -> TC_7().
  if (log_) log_(0, "CAL-only/TC_7 initial delay");
  wait_cancellable(kCheryArs133WakeupSettle);

  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession");
  expect_routine(
      std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
      std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03}, 5,
      "31 01 0203 CheckProgrammingPrecondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 8,
         "10 02 ProgrammingSession");
  unlock_security(12);
  write_fingerprint(17);

  wait_cancellable(2000ms);
  transfer_driver(images, 20, 42);
  wait_cancellable(2000ms);
  verify_driver(images, 46);
  wait_cancellable(2000ms);

  const auto erase_cal =
      chery_ars133_erase_memory(layout_.cal_start, layout_.cal_length);
  expect_routine(
      erase_cal, std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
      52, "31 01 FF00 EraseCAL");
  transfer_image(layout_.cal_start, images.cal, 56, 78, "CAL");
  wait_cancellable(2000ms);
  verify_cal(images, 82);

  wait_cancellable(2000ms);
  check_programming_dependencies(88);
  hard_reset(94, 2000ms);
  clear_dtc(99);
  if (log_) log_(100, "CAL-only/TC_7 completed");
}

void CheryArs133Flow::run(const CheryArs133Images& images,
                          CheryArs133FlashMode mode,
                          std::stop_token stop) {
  stop_ = stop;
  check_cancelled();

  require_image_size(images.driver0, layout_.driver0_length, "FLD block0");
  require_image_size(images.driver, layout_.driver_length, "FLD");
  require_verification_size(images.driver_verification, "FLD");

  if (mode == CheryArs133FlashMode::AppCal ||
      mode == CheryArs133FlashMode::AppOnly) {
    require_image_size(images.app, layout_.app_length, "APP");
    require_verification_size(images.app_verification, "APP");
  }
  if (mode == CheryArs133FlashMode::AppCal ||
      mode == CheryArs133FlashMode::CalOnly) {
    require_image_size(images.cal, layout_.cal_length, "CAL");
    require_verification_size(images.cal_verification, "CAL");
  }

  switch (mode) {
    case CheryArs133FlashMode::AppCal:
      run_app_cal(images);
      return;
    case CheryArs133FlashMode::AppOnly:
      run_app_only(images);
      return;
    case CheryArs133FlashMode::CalOnly:
      run_cal_only(images);
      return;
  }
  throw std::invalid_argument("unsupported Chery ARS1.33 flashing mode");
}

} // namespace uds
