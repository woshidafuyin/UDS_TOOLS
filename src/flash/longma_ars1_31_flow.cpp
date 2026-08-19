#include "flash/longma_ars1_31_flow.hpp"

#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
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

std::string crc_text(std::uint16_t crc) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
      << crc;
  return out.str();
}
} // namespace

std::vector<std::uint8_t> longma_ars131_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> longma_ars131_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  // EP32_V1.7.100.cdd defines RID FF00 as Address[4] + Length[4].  Unlike
  // RequestDownload, this passing CAPL project does not send an ALFID byte.
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::size_t longma_ars131_max_block_length(
    std::span<const std::uint8_t> response) {
  if (response.size() < 2 || response[0] != 0x74) {
    throw std::runtime_error("invalid RequestDownload response");
  }
  const auto length_bytes =
      static_cast<std::size_t>((response[1] >> 4U) & 0x0FU);
  if (length_bytes == 0 || response.size() < 2U + length_bytes) {
    throw std::runtime_error(
        "RequestDownload response has no maxNumberOfBlockLength");
  }
  std::size_t value = 0;
  for (std::size_t i = 0; i < length_bytes; ++i) {
    value = (value << 8U) | response[2U + i];
  }
  if (value <= 2U) {
    throw std::runtime_error("ECU maxNumberOfBlockLength is too small");
  }
  if (value > 4095U) {
    throw std::runtime_error(
        "ECU maxNumberOfBlockLength exceeds Classic ISO-TP implementation");
  }
  return value;
}

std::uint16_t longma_ars131_crc16_ccitt_false(
    std::span<const std::uint8_t> data) {
  std::uint16_t crc = 0xFFFF;
  for (const auto byte : data) {
    crc ^= static_cast<std::uint16_t>(byte) << 8U;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<std::uint16_t>(crc << 1U);
    }
  }
  return crc;
}

LongmaArs131DownloadPlan resolve_longma_ars131_download_plan(
    std::wstring_view operation_mode) {
  if (operation_mode.empty() || operation_mode == L"app") {
    return {false, true, false, "APP"};
  }
  if (operation_mode == L"ft") {
    return {true, true, false, "FT"};
  }
  if (operation_mode == L"cal") {
    return {false, false, true, "CAL"};
  }
  if (operation_mode == L"app_cal") {
    return {false, true, true, "APP+CAL"};
  }
  throw std::runtime_error(
      "ARS1.31 operation mode must be APP, FT, CAL or APP+CAL");
}

LongmaArs131Flow::LongmaArs131Flow(UdsClient& physical,
                                   UdsClient& functional,
                                   LongmaArs131Layout layout, Log log,
                                   KeyGenerator key_generator,
                                   HealthCheck health_check,
                                   UdsClient* ft_physical)
    : physical_(physical), functional_(functional),
      ft_physical_(ft_physical), layout_(layout),
      log_(std::move(log)), key_generator_(std::move(key_generator)),
      health_check_(std::move(health_check)) {}

void LongmaArs131Flow::check_cancelled() const {
  if (health_check_) health_check_();
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

void LongmaArs131Flow::wait_cancellable(
    std::chrono::milliseconds duration) const {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 25ms));
  }
}

UdsResponse LongmaArs131Flow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> expected, int percent,
    const std::string& name, bool exact_response,
    std::chrono::milliseconds p2, std::chrono::milliseconds p2_star) {
  check_cancelled();
  if (log_) log_(percent, name);

  UdsResponse result;
  try {
    // UdsClient keeps waiting after every NRC 0x78.  Thus erase and the two
    // session changes accept the same arbitrary ResponsePending count as CANoe.
    result = client.request(request, p2, p2_star);
  } catch (const std::exception& error) {
    throw std::runtime_error(name + ": " + error.what());
  }
  check_cancelled();
  if (!result.success) {
    throw std::runtime_error(name + ": NRC/timeout " +
                             to_hex(result.response));
  }

  const auto matches_prefix = result.response.size() >= expected.size() &&
                              std::equal(expected.begin(), expected.end(),
                                         result.response.begin());
  const auto matches_length =
      !exact_response || result.response.size() == expected.size();
  if (!matches_prefix || !matches_length) {
    throw std::runtime_error(name + ": response mismatch, expected " +
                             to_hex(expected) +
                             (exact_response ? " exactly, got " : " prefix, got ") +
                             to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  return result;
}

void LongmaArs131Flow::send_suppressed(
    std::span<const std::uint8_t> request, int percent,
    const std::string& name) {
  check_cancelled();
  if (log_) log_(percent, name);
  try {
    functional_.send_only(request);
  } catch (const std::exception& error) {
    throw std::runtime_error(name + ": " + error.what());
  }
  // ResquestDiagServiceTestReport(response=0) waits 50 ms to confirm that a
  // suppress-positive-response request produced no positive reply.
  wait_cancellable(50ms);
  if (log_) log_(percent, name + " PASS: sent without waiting for a response");
}

void LongmaArs131Flow::unlock_security(int begin_percent,
                                       const std::string& label) {
  const std::array<std::uint8_t, 2> seed_request{0x27, 0x01};
  const std::array<std::uint8_t, 2> seed_prefix{0x67, 0x01};
  auto seed_result = expect(physical_, seed_request, seed_prefix,
                            begin_percent, label + " 27 01 RequestSeed");
  if (seed_result.response.size() != 6) {
    throw std::runtime_error(label +
                             ": security seed must be exactly four bytes, got " +
                             std::to_string(seed_result.response.size() - 2U));
  }

  std::vector<std::uint8_t> key;
  try {
    key = key_generator_(std::span(seed_result.response).subspan(2, 4));
  } catch (const std::exception& error) {
    throw std::runtime_error(label + ": GenerateKeyEx failed: " + error.what());
  }
  if (key.size() != 4) {
    throw std::runtime_error(label +
                             ": GenerateKeyEx key must be exactly four bytes");
  }
  std::vector<std::uint8_t> key_request{0x27, 0x02};
  key_request.insert(key_request.end(), key.begin(), key.end());
  expect(physical_, key_request,
         std::array<std::uint8_t, 2>{0x67, 0x02}, begin_percent + 1,
         label + " 27 02 SendKey", true);
}

std::vector<std::uint8_t> LongmaArs131Flow::fingerprint_f184() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  const auto year = local.tm_year + 1900;
  if (year < 2000 || year > 2255) {
    throw std::runtime_error("current year cannot be encoded in DID F184");
  }
  // Flash.can writes ordinary binary YY/MM/DD (not BCD), followed by the
  // four zero-initialised Sign bytes observed in the passing trace.
  return {static_cast<std::uint8_t>(year - 2000),
          static_cast<std::uint8_t>(local.tm_mon + 1),
          static_cast<std::uint8_t>(local.tm_mday), 0x00, 0x00, 0x00, 0x00};
}

void LongmaArs131Flow::transfer_image(
    std::uint32_t address, std::span<const std::uint8_t> image,
    int begin_percent, int end_percent, const std::string& label) {
  if (image.empty()) throw std::runtime_error(label + " image is empty");

  const auto download = longma_ars131_request_download(
      address, static_cast<std::uint32_t>(image.size()));
  const auto response = expect(physical_, download,
                               std::array<std::uint8_t, 1>{0x74},
                               begin_percent, "34 " + label);
  const auto max_block_length =
      longma_ars131_max_block_length(response.response);
  const auto chunk_size = max_block_length - 2U;
  if (log_) {
    log_(begin_percent,
         "34 " + label + " negotiated max block=" +
             std::to_string(max_block_length) + ", data=" +
             std::to_string(chunk_size));
  }
  // Sever34() has one explicit 50 ms settle delay after the positive reply.
  wait_cancellable(50ms);

  std::size_t offset = 0;
  std::uint8_t sequence = 1;
  const auto blocks = (image.size() + chunk_size - 1U) / chunk_size;
  std::size_t block = 0;
  while (offset < image.size()) {
    check_cancelled();
    const auto count = std::min(chunk_size, image.size() - offset);
    std::vector<std::uint8_t> transfer{0x36, sequence};
    transfer.insert(transfer.end(),
                    image.begin() + static_cast<std::ptrdiff_t>(offset),
                    image.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const std::array<std::uint8_t, 2> expected{0x76, sequence};
    const auto percent = begin_percent + static_cast<int>(
        (end_percent - begin_percent) *
        static_cast<double>(offset + count) / static_cast<double>(image.size()));
    ++block;
    expect(physical_, transfer, expected, percent,
           "36 " + label + " block " + std::to_string(block) + "/" +
               std::to_string(blocks),
           true);
    offset += count;
    // The uint8_t conversion deliberately reproduces CAPL BSC FF -> 00.
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }

  const auto crc = longma_ars131_crc16_ccitt_false(image);
  const std::array<std::uint8_t, 3> expected_exit{
      0x77, static_cast<std::uint8_t>(crc >> 8U),
      static_cast<std::uint8_t>(crc)};
  expect(physical_, std::array<std::uint8_t, 1>{0x37}, expected_exit,
         end_percent, "37 " + label + " CRC16=0x" + crc_text(crc), true);
}

void LongmaArs131Flow::run(const LongmaArs131Images& images,
                           std::wstring_view entry_mode,
                           std::stop_token stop) {
  using namespace std::chrono_literals;
  stop_ = stop;
  check_cancelled();
  const auto plan = resolve_longma_ars131_download_plan(entry_mode);
  if (images.driver.size() != layout_.driver_length ||
      (plan.download_app && images.app.size() != layout_.app_length) ||
      (plan.download_cal && images.cal.size() != layout_.cal_length)) {
    throw std::runtime_error(
        "ARS1.31 image length does not match the configured flash layout");
  }
  if (plan.ft_entry && ft_physical_ == nullptr) {
    throw std::runtime_error("ARS1.31 FT recovery endpoint is not configured");
  }

  // maintest() starts its periodic senders before PreReadS19File(), and that
  // function leaves a one-second settle interval before Download().  The C++
  // workflow loads files before opening the bus, so reproduce the same interval
  // here after the 0x400/TesterPresent sender threads have started.
  if (log_) log_(0, "PreReadS19File-equivalent precondition settle (1 s)");
  wait_cancellable(1000ms);

  if (plan.ft_entry) {
    // Flash.can::Download() Protocol=FT uses the target-specific raw recovery
    // endpoint only for the two raw session requests: 10 03, 200 ms, 10 02,
    // then a 2 s endpoint-switch delay. The original CAPL does not wait for
    // either raw request's response before continuing on the APP endpoint.
    if (log_) log_(1, "FT recovery entry: raw 10 03");
    ft_physical_->send_only(std::array<std::uint8_t, 2>{0x10, 0x03});
    wait_cancellable(200ms);
    if (log_) log_(3, "FT recovery entry: raw 10 02");
    ft_physical_->send_only(std::array<std::uint8_t, 2>{0x10, 0x02});
    wait_cancellable(2000ms);
    if (log_) {
      log_(5, "FT endpoint switch completed; continue on the APP physical "
              "endpoint");
    }
  } else {
    expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
           std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
           "10 03 ExtendedSession");
    send_suppressed(std::array<std::uint8_t, 2>{0x10, 0x83}, 3,
                    "FUNC 10 83 ExtendedSession suppressPositiveResponse");

    unlock_security(5, "Application security");
    expect(physical_, std::array<std::uint8_t, 3>{0x22, 0xF1, 0x8A},
           std::array<std::uint8_t, 3>{0x62, 0xF1, 0x8A}, 8,
           "22 F18A SystemSupplierIdentifier");
    expect(physical_, std::array<std::uint8_t, 3>{0x22, 0xF0, 0x89},
           std::array<std::uint8_t, 3>{0x62, 0xF0, 0x89}, 9,
           "22 F089 ECUHardwareVersion");
    expect(physical_, std::array<std::uint8_t, 3>{0x22, 0xF1, 0x89},
           std::array<std::uint8_t, 3>{0x62, 0xF1, 0x89}, 10,
           "22 F189 ECUSoftwareVersion");
    expect(physical_, std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
           std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03}, 12,
           "31 01 0203 CheckProgrammingPreconditions", true);

    send_suppressed(std::array<std::uint8_t, 2>{0x85, 0x82}, 13,
                    "FUNC 85 82 DisableDTCSetting suppressPositiveResponse");
    send_suppressed(
        std::array<std::uint8_t, 3>{0x28, 0x83, 0x03}, 14,
        "FUNC 28 83 03 DisableCommunication suppressPositiveResponse");
    expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
           std::array<std::uint8_t, 2>{0x50, 0x02}, 16,
           "10 02 ProgrammingSession");
    wait_cancellable(200ms);
  }

  unlock_security(18, "Programming security");
  expect(physical_, std::array<std::uint8_t, 3>{0x22, 0xF1, 0x70},
         std::array<std::uint8_t, 3>{0x62, 0xF1, 0x70}, 21,
         "22 F170 FBLVersion");
  expect(physical_, std::array<std::uint8_t, 3>{0x22, 0xF1, 0x71},
         std::array<std::uint8_t, 3>{0x62, 0xF1, 0x71}, 22,
         "22 F171 FBLRequirementVersion");

  auto fingerprint = fingerprint_f184();
  std::vector<std::uint8_t> write_fingerprint{0x2E, 0xF1, 0x84};
  write_fingerprint.insert(write_fingerprint.end(), fingerprint.begin(),
                           fingerprint.end());
  expect(physical_, write_fingerprint,
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x84}, 24,
         "2E F184 ProgrammingDate");

  transfer_image(layout_.driver_start, images.driver, 25, 30, "Driver");
  expect(physical_,
         std::array<std::uint8_t, 8>{0x31, 0x01, 0x02, 0x02,
                                     0x08, 0x00, 0x00, 0x00},
         std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x02}, 32,
         "31 01 0202 DriverChecksum", true);

  const auto check_dependencies = [&](int percent,
                                      const std::string& content) {
    expect(physical_,
           std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
           std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x01}, percent,
           "31 01 FF01 CheckProgrammingDependencies " + content, true);
  };

  if (plan.download_app) {
    const auto erase_app =
        longma_ars131_erase_memory(layout_.app_start, layout_.app_length);
    expect(physical_, erase_app,
           std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00}, 35,
           "31 01 FF00 EraseAPP", true, 2000ms, 15000ms);
    transfer_image(layout_.app_start, images.app, 37,
                   plan.download_cal ? 78 : 94, "APP");
    check_dependencies(plan.download_cal ? 79 : 96, "APP");
  }

  if (plan.download_cal) {
    const auto erase_cal =
        longma_ars131_erase_memory(layout_.cal_start, layout_.cal_length);
    expect(physical_, erase_cal,
           std::array<std::uint8_t, 4>{0x71, 0x01, 0xFF, 0x00},
           plan.download_app ? 80 : 35, "31 01 FF00 EraseCAL", true,
           2000ms, 15000ms);
    transfer_image(layout_.cal_start, images.cal,
                   plan.download_app ? 82 : 37, 94, "CAL");
    check_dependencies(96, "CAL");
  }

  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x03},
         std::array<std::uint8_t, 2>{0x51, 0x03}, 97,
         "11 03 SoftReset", true);
  wait_cancellable(1000ms);

  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 98,
         "10 03 PostResetExtendedSession");
  send_suppressed(std::array<std::uint8_t, 3>{0x28, 0x80, 0x03}, 99,
                  "FUNC 28 80 03 EnableCommunication suppressPositiveResponse");
  send_suppressed(std::array<std::uint8_t, 2>{0x85, 0x81}, 99,
                  "FUNC 85 81 EnableDTCSetting suppressPositiveResponse");
  send_suppressed(std::array<std::uint8_t, 2>{0x10, 0x81}, 100,
                  "FUNC 10 81 DefaultSession suppressPositiveResponse");
  if (log_) {
    log_(100, "ARS1.31 " + std::string(plan.display_name) +
                  " Download completed");
  }
}

} // namespace uds
