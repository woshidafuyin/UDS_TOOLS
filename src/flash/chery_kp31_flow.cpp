#include "flash/chery_kp31_flow.hpp"

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

void require_size(std::span<const std::uint8_t> data, std::size_t expected,
                  const char* label) {
  if (data.size() != expected) {
    throw std::runtime_error(std::string("KP31 ") + label +
                             " length does not match the configured layout");
  }
}
} // namespace

CheryKp31DownloadPlan resolve_chery_kp31_download_plan(
    std::wstring_view entry_mode) {
  if (entry_mode.empty() || entry_mode == L"app") {
    return {CheryKp31FlashMode::AppOnly, true, false};
  }
  if (entry_mode == L"cal") {
    return {CheryKp31FlashMode::CalOnly, false, true};
  }
  if (entry_mode == L"app_cal") {
    return {CheryKp31FlashMode::AppCal, true, true};
  }
  throw std::invalid_argument(
      "KP31 flashing mode must be APP, CAL or APP+CAL");
}

std::vector<std::uint8_t> chery_kp31_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> chery_kp31_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::size_t chery_kp31_max_block_length(
    std::span<const std::uint8_t> response) {
  if (response.size() < 2 || response[0] != 0x74) {
    throw std::runtime_error("invalid KP31 RequestDownload response");
  }
  const auto length_bytes =
      static_cast<std::size_t>((response[1] >> 4U) & 0x0FU);
  if (length_bytes == 0 || response.size() < 2U + length_bytes) {
    throw std::runtime_error(
        "KP31 RequestDownload response has no max block length");
  }
  std::size_t value = 0;
  for (std::size_t i = 0; i < length_bytes; ++i) {
    value = (value << 8U) | response[2U + i];
  }
  // The CAPL implementation falls back to 0x400 if the ECU reports zero and
  // uses a 12-bit manual ISO-TP length field.
  if (value == 0) value = 0x400;
  value = std::min<std::size_t>(value, 0x0FFF);
  if (value <= 2U) {
    throw std::runtime_error("KP31 ECU max block length is too small");
  }
  return value;
}

CheryKp31Flow::CheryKp31Flow(UdsClient& physical, UdsClient& functional,
                             CheryKp31Layout layout, Log log,
                             KeyGenerator key_generator)
    : physical_(physical), functional_(functional), layout_(layout),
      log_(std::move(log)), key_generator_(std::move(key_generator)) {}

void CheryKp31Flow::check_cancelled() const {
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

void CheryKp31Flow::wait_cancellable(
    std::chrono::milliseconds duration) const {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 50ms));
  }
}

UdsResponse CheryKp31Flow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> expected_prefix, int percent,
    const std::string& name, std::chrono::milliseconds p2,
    std::chrono::milliseconds p2_star) {
  check_cancelled();
  if (log_) log_(percent, name);
  auto result = client.request(request, p2, p2_star);
  check_cancelled();
  if (!result.success) {
    throw std::runtime_error(name + ": NRC/timeout " + result.detail);
  }
  if (result.response.size() < expected_prefix.size() ||
      !std::equal(expected_prefix.begin(), expected_prefix.end(),
                  result.response.begin())) {
    throw std::runtime_error(name + ": response mismatch " +
                             to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  // TxMsgSrever() leaves 50 ms after each service in the CANoe reference.
  wait_cancellable(50ms);
  return result;
}

UdsResponse CheryKp31Flow::expect_routine(
    std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> expected_prefix, int percent,
    const std::string& name) {
  auto result = expect(physical_, request, expected_prefix, percent, name);
  if (result.response.size() < 5 || result.response[4] != 0x00) {
    throw std::runtime_error(name + ": routine status is not 0x00: " +
                             to_hex(result.response));
  }
  return result;
}

void CheryKp31Flow::send_functional_suppressed(
    std::span<const std::uint8_t> request, int percent,
    const std::string& name) {
  check_cancelled();
  if (log_) log_(percent, name);
  functional_.send_only(request);
  wait_cancellable(50ms);
  if (log_) log_(percent, name + " PASS: sent without waiting for response");
}

std::vector<std::uint8_t> CheryKp31Flow::fingerprint_f184() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::vector<std::uint8_t> result(19, 0xFF);
  result[0] = bcd(static_cast<unsigned>((local.tm_year + 1900) % 100));
  result[1] = bcd(static_cast<unsigned>(local.tm_mon + 1));
  result[2] = bcd(static_cast<unsigned>(local.tm_mday));
  return result;
}

void CheryKp31Flow::unlock_security(int percent) {
  auto seed_result = expect(
      physical_, std::array<std::uint8_t, 2>{0x27, 0x11},
      std::array<std::uint8_t, 2>{0x67, 0x11}, percent,
      "27 11 RequestSeed");
  if (seed_result.response.size() != 18U) {
    throw std::runtime_error("KP31 seed must be exactly 16 bytes");
  }
  auto key = key_generator_(std::span(seed_result.response).subspan(2));
  if (key.size() != 16U) {
    throw std::runtime_error("KP31 key must be exactly 16 bytes");
  }
  std::vector<std::uint8_t> request{0x27, 0x12};
  request.insert(request.end(), key.begin(), key.end());
  expect(physical_, request, std::array<std::uint8_t, 2>{0x67, 0x12},
         percent + 2, "27 12 SendKey");
}

void CheryKp31Flow::write_fingerprint(int percent) {
  auto fingerprint = fingerprint_f184();
  std::vector<std::uint8_t> request{0x2E, 0xF1, 0x84};
  request.insert(request.end(), fingerprint.begin(), fingerprint.end());
  expect(physical_, request,
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x84}, percent,
         "2E F184 WriteFingerprint");
}

void CheryKp31Flow::verify_rsa(
    std::uint16_t routine_id,
    std::span<const std::uint8_t> verification,
    int percent, const std::string& label) {
  require_size(verification, 512U, label.c_str());
  std::vector<std::uint8_t> request{
      0x31, 0x01, static_cast<std::uint8_t>(routine_id >> 8U),
      static_cast<std::uint8_t>(routine_id)};
  request.insert(request.end(), verification.begin(), verification.end());
  expect_routine(
      request,
      std::array<std::uint8_t, 4>{
          0x71, 0x01, static_cast<std::uint8_t>(routine_id >> 8U),
          static_cast<std::uint8_t>(routine_id)},
      percent, "31 01 " + to_hex(std::array<std::uint8_t, 2>{
                                  static_cast<std::uint8_t>(routine_id >> 8U),
                                  static_cast<std::uint8_t>(routine_id)}) +
                   " " + label);
}

void CheryKp31Flow::check_dependencies(int percent) {
  expect_routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x01},
                 percent, "31 01 FF01 CheckProgrammingDependencies");
}

void CheryKp31Flow::hard_reset_and_clear_dtc(
    int percent, bool restore_default_session) {
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, percent,
         "11 01 HardReset");
  // server_11() in the CAPL reference waits two seconds after the response.
  wait_cancellable(2000ms);
  if (restore_default_session) {
    send_functional_suppressed(
        std::array<std::uint8_t, 2>{0x10, 0x81}, percent + 1,
        "FUNC 10 81 DefaultSession suppressPositiveResponse");
  }
  expect(functional_,
         std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, percent + 2,
         "FUNC 14 FF FF FF ClearDTC");
}

void CheryKp31Flow::transfer_image(
    std::uint32_t address, std::span<const std::uint8_t> image,
    int begin_percent, int end_percent, const std::string& label,
    bool send_transfer_exit, std::size_t forced_data_size) {
  const auto response = expect(
      physical_, chery_kp31_request_download(
                     address, static_cast<std::uint32_t>(image.size())),
      std::array<std::uint8_t, 1>{0x74}, begin_percent, "34 " + label);
  const auto negotiated = chery_kp31_max_block_length(response.response) - 2U;
  if (forced_data_size != 0 && negotiated < forced_data_size) {
    throw std::runtime_error(
        "KP31 " + label +
        " ECU max block length is smaller than the CAPL fixed 258-byte "
        "TransferData request");
  }
  const auto chunk_size =
      forced_data_size == 0 ? negotiated : forced_data_size;
  if (chunk_size == 0) {
    throw std::runtime_error("KP31 " + label + " transfer chunk is zero");
  }

  std::size_t offset = 0;
  std::uint8_t sequence = 1;
  const auto blocks = (image.size() + chunk_size - 1U) / chunk_size;
  std::size_t block = 0;
  while (offset < image.size()) {
    check_cancelled();
    const auto count = std::min(chunk_size, image.size() - offset);
    std::vector<std::uint8_t> transfer{0x36, sequence};
    transfer.insert(
        transfer.end(), image.begin() + static_cast<std::ptrdiff_t>(offset),
        image.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const auto percent = begin_percent + static_cast<int>(
        (end_percent - begin_percent) *
        static_cast<double>(offset + count) / static_cast<double>(image.size()));
    ++block;
    expect(physical_, transfer,
           std::array<std::uint8_t, 2>{0x76, sequence}, percent,
           "36 " + label + " block " + std::to_string(block) + "/" +
               std::to_string(blocks));
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }

  if (send_transfer_exit) {
    expect(physical_, std::array<std::uint8_t, 1>{0x37},
           std::array<std::uint8_t, 1>{0x77}, end_percent,
           "37 " + label);
  } else if (log_) {
    log_(end_percent,
         "APP TransferExit intentionally omitted to match KP31 Flash.can");
  }
}

void CheryKp31Flow::run_app_only(const CheryKp31Images& images) {
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.app, layout_.app_length, "APP");
  require_size(images.driver_verification, 512U, "Driver RSA");
  require_size(images.app_verification, 512U, "APP RSA");

  // maintest(): FileInit(), then one 2 s delay before Download().
  if (log_) log_(0, "FileInit-equivalent resources loaded; settle 2 s");
  wait_cancellable(2000ms);

  send_functional_suppressed(
      std::array<std::uint8_t, 2>{0x10, 0x83}, 1,
      "FUNC 10 83 ExtendedSession suppressPositiveResponse");
  expect_routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03},
                 3, "31 01 0203 CheckProgrammingPrecondition");
  send_functional_suppressed(
      std::array<std::uint8_t, 2>{0x85, 0x82}, 5,
      "FUNC 85 82 DisableDTCSetting suppressPositiveResponse");
  send_functional_suppressed(
      std::array<std::uint8_t, 3>{0x28, 0x81, 0x03}, 6,
      "FUNC 28 81 03 DisableCommunication suppressPositiveResponse");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 8,
         "10 02 ProgrammingSession");
  unlock_security(10);
  write_fingerprint(15);

  transfer_image(layout_.driver_start, images.driver, 18, 30, "Driver", true);
  wait_cancellable(2000ms);
  verify_rsa(0xDD02, images.driver_verification, 34, "VerifyDriverRSA");
  wait_cancellable(2000ms);

  expect_routine(chery_kp31_erase_memory(layout_.app_start,
                                         layout_.app_length),
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
                 38, "31 01 FF00 EraseAPP");
  // KP31 Download() hard-codes server_36(..., 258, ...) for APP, meaning
  // 256 data bytes per TransferData request, and comments out APP server_37.
  transfer_image(layout_.app_start, images.app, 42, 82, "APP", false, 256U);
  wait_cancellable(2000ms);
  verify_rsa(0xDD02, images.app_verification, 87, "VerifyAppRSA");
  wait_cancellable(2000ms);

  check_dependencies(92);
  hard_reset_and_clear_dtc(95, true);
  wait_cancellable(2000ms);
  if (log_) log_(100, "KP31 APP Download completed");
}

void CheryKp31Flow::run_app_cal(const CheryKp31Images& images) {
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.app, layout_.app_length, "APP");
  require_size(images.cal, layout_.cal_length, "CAL");
  require_size(images.driver_verification, 512U, "Driver RSA");
  require_size(images.app_verification, 512U, "APP RSA");
  require_size(images.cal_verification, 512U, "CAL RSA");

  // maintest(APPAndCAL) -> TC_2(): FileInit() then a one-second settle.
  if (log_) log_(0, "FileInit-equivalent APP+CAL resources loaded; settle 1 s");
  wait_cancellable(1000ms);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession (TC_2)");
  expect_routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xD0, 0x03},
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xD0, 0x03},
                 4, "31 01 D003 TC_2 precondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 6,
         "10 02 ProgrammingSession");
  unlock_security(8);
  write_fingerprint(12);

  // TC_2 calls fileData() before D004, so D004 carries the APP RSA payload.
  verify_rsa(0xD004, images.app_verification, 15,
             "SecuritySignatureCheck(APP RSA)");
  transfer_image(layout_.driver_start, images.driver, 18, 28, "Driver", true);
  verify_rsa(0xD002, images.driver_verification, 31, "VerifyDriverRSA");
  wait_cancellable(2000ms);

  expect_routine(chery_kp31_erase_memory(layout_.app_start,
                                         layout_.app_length),
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
                 35, "31 01 FF00 EraseAPP");
  transfer_image(layout_.app_start, images.app, 39, 67, "APP", true);
  verify_rsa(0xD002, images.app_verification, 70, "VerifyAppRSA");

  expect_routine(chery_kp31_erase_memory(layout_.cal_start,
                                         layout_.cal_length),
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
                 74, "31 01 FF00 EraseCAL");
  transfer_image(layout_.cal_start, images.cal, 77, 84, "CAL", true);
  verify_rsa(0xD002, images.cal_verification, 87, "VerifyCalRSA");
  wait_cancellable(2000ms);
  check_dependencies(90);

  // TC_2 accepts D005 routine result 00 or 01 (server_31 special case).
  const auto installation = expect(
      physical_, std::array<std::uint8_t, 4>{0x31, 0x01, 0xD0, 0x05},
      std::array<std::uint8_t, 4>{0x71, 0x01, 0xD0, 0x05}, 93,
      "31 01 D005 FlashFileInstallation");
  if (installation.response.size() < 5 ||
      (installation.response[4] != 0x00 && installation.response[4] != 0x01)) {
    throw std::runtime_error(
        "31 01 D005 FlashFileInstallation: routine status is neither 00 nor 01: " +
        to_hex(installation.response));
  }
  hard_reset_and_clear_dtc(96, false);
  if (log_) log_(100, "KP31 APP+CAL TC_2 completed");
}

void CheryKp31Flow::run_cal_only(const CheryKp31Images& images) {
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.cal, layout_.cal_length, "CAL");
  require_size(images.driver_verification, 512U, "Driver RSA");
  require_size(images.cal_verification, 512U, "CAL RSA");

  // maintest(CAL) -> TC_7(): FileInit() then a one-second settle.
  if (log_) log_(0, "FileInit-equivalent CAL resources loaded; settle 1 s");
  wait_cancellable(1000ms);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 3,
         "10 03 ExtendedSession (TC_7)");
  expect_routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03},
                 6, "31 01 0203 CheckProgrammingPrecondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 9,
         "10 02 ProgrammingSession");
  unlock_security(12);
  // Update_PublicKey is an optional panel flag in TC_7 and defaults to off;
  // it is not a separate flashing mode and is intentionally not synthesized.
  write_fingerprint(16);
  wait_cancellable(2000ms);

  transfer_image(layout_.driver_start, images.driver, 20, 35, "Driver", true);
  wait_cancellable(2000ms);
  verify_rsa(0xDD02, images.driver_verification, 39, "VerifyDriverRSA");
  wait_cancellable(2000ms);

  expect_routine(chery_kp31_erase_memory(layout_.cal_start,
                                         layout_.cal_length),
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
                 45, "31 01 FF00 EraseCAL");
  transfer_image(layout_.cal_start, images.cal, 50, 72, "CAL", true);
  wait_cancellable(2000ms);
  verify_rsa(0xDD02, images.cal_verification, 78, "VerifyCalRSA");
  wait_cancellable(2000ms);
  check_dependencies(84);
  expect_routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xDD, 0x03},
                 std::array<std::uint8_t, 4>{0x71, 0x01, 0xDD, 0x03},
                 90, "31 01 DD03 FlashFileInstallation");
  hard_reset_and_clear_dtc(95, false);
  if (log_) log_(100, "KP31 CAL TC_7 completed");
}

void CheryKp31Flow::run(const CheryKp31Images& images,
                        CheryKp31FlashMode mode,
                        std::stop_token stop) {
  stop_ = stop;
  check_cancelled();
  switch (mode) {
    case CheryKp31FlashMode::AppOnly:
      run_app_only(images);
      return;
    case CheryKp31FlashMode::CalOnly:
      run_cal_only(images);
      return;
    case CheryKp31FlashMode::AppCal:
      run_app_cal(images);
      return;
  }
  throw std::runtime_error("unsupported KP31 flashing mode");
}

} // namespace uds
