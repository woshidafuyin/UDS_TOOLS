#include "flash/baic_radar_flow.hpp"

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

std::uint8_t bcd(unsigned value) {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | value % 10U);
}

std::string crc_text(std::uint32_t crc) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
      << crc;
  return out.str();
}
} // namespace

std::vector<std::uint8_t> baic_radar_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> baic_radar_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::size_t baic_radar_max_block_length(
    std::span<const std::uint8_t> response) {
  if (response.size() < 3U || response[0] != 0x74) {
    throw std::runtime_error("invalid RequestDownload response");
  }
  const auto count = static_cast<std::size_t>((response[1] >> 4U) & 0x0FU);
  if (count == 0U || response.size() < count + 2U) {
    throw std::runtime_error(
        "RequestDownload response has no maxNumberOfBlockLength");
  }
  std::size_t value{};
  for (std::size_t index = 0; index < count; ++index) {
    value = (value << 8U) | response[index + 2U];
  }
  if (value <= 2U || value > 4095U) {
    throw std::runtime_error(
        "ECU maxNumberOfBlockLength is outside the supported range");
  }
  return value;
}

std::uint32_t baic_radar_crc32(std::span<const std::uint8_t> data) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const auto byte : data) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8U; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  // The archived BAIC CAPL starts at 0xFFFFFFFF and deliberately returns the
  // table-loop state (its temporary complement is complemented once again).
  // This is the no-final-XOR form, not the common ZIP presentation value.
  return crc;
}

BaicRadarFlow::BaicRadarFlow(UdsClient& physical, UdsClient& functional,
                             BaicRadarProtocol protocol, Log log,
                             KeyGenerator key_generator)
    : physical_(physical), functional_(functional),
      protocol_(std::move(protocol)), log_(std::move(log)),
      key_generator_(std::move(key_generator)) {}

void BaicRadarFlow::check_cancelled() const {
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

UdsResponse BaicRadarFlow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> expected, int percent,
    const std::string& name, bool exact) {
  check_cancelled();
  if (log_) log_(percent, name);
  auto result = client.request(request);
  if (!result.success) {
    throw std::runtime_error(name + ": NRC/timeout " + result.detail);
  }
  const auto prefix = result.response.size() >= expected.size() &&
                      std::equal(expected.begin(), expected.end(),
                                 result.response.begin());
  if (!prefix || (exact && result.response.size() != expected.size())) {
    throw std::runtime_error(name + ": response mismatch " +
                             to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  return result;
}

void BaicRadarFlow::send_cleanup(
    std::span<const std::uint8_t> request, int percent,
    const std::string& name) {
  check_cancelled();
  if (log_) log_(percent, name);
  functional_.send_only(request, stop_);
  std::this_thread::sleep_for(50ms);
}

std::vector<std::uint8_t> BaicRadarFlow::fingerprint_f184() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  return {bcd(static_cast<unsigned>((local.tm_year + 1900) % 100)),
          bcd(static_cast<unsigned>(local.tm_mon + 1)),
          bcd(static_cast<unsigned>(local.tm_mday)),
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

void BaicRadarFlow::transfer_image(
    std::uint32_t address, std::span<const std::uint8_t> image,
    int begin_percent, int end_percent, const std::string& label) {
  const auto download = baic_radar_request_download(
      address, static_cast<std::uint32_t>(image.size()));
  const auto response = expect(physical_, download,
                               std::array<std::uint8_t, 1>{0x74},
                               begin_percent, "34 " + label);
  const auto block_length = baic_radar_max_block_length(response.response);
  const auto chunk_size = block_length - 2U;
  std::size_t offset{};
  std::uint8_t sequence{1};
  const auto blocks = (image.size() + chunk_size - 1U) / chunk_size;
  std::size_t block{};
  while (offset < image.size()) {
    check_cancelled();
    const auto count = std::min(chunk_size, image.size() - offset);
    std::vector<std::uint8_t> transfer{0x36, sequence};
    transfer.insert(transfer.end(), image.begin() + offset,
                    image.begin() + offset + count);
    const auto percent = begin_percent + static_cast<int>(
        (end_percent - begin_percent) *
        static_cast<double>(offset + count) /
        static_cast<double>(image.size()));
    ++block;
    expect(physical_, transfer,
           std::array<std::uint8_t, 2>{0x76, sequence}, percent,
           "36 " + label + " block " + std::to_string(block) + "/" +
               std::to_string(blocks),
           true);
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }
  expect(physical_, std::array<std::uint8_t, 1>{0x37},
         std::array<std::uint8_t, 1>{0x77}, end_percent,
         "37 " + label, true);
}

void BaicRadarFlow::verify_crc(std::span<const std::uint8_t> image,
                               int percent, const std::string& label) {
  const auto crc = baic_radar_crc32(image);
  std::vector<std::uint8_t> request{0x31, 0x01, 0x02, 0x02};
  append_u32(request, crc);
  expect(physical_, request,
         std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x02, 0x00},
         percent, "31 01 0202 " + label + " CRC32=0x" + crc_text(crc));
}

void BaicRadarFlow::run(const BaicRadarImages& images,
                        std::stop_token stop) {
  stop_ = stop;
  if (images.driver.size() != protocol_.driver_length ||
      images.app.size() != protocol_.app_length) {
    throw std::runtime_error(protocol_.project_name +
                             " image length does not match its frozen layout");
  }

  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 1,
         "10 01 Physical DefaultSession");
  expect(functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "FUNC 10 03 ExtendedSession");
  expect(physical_, std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
         std::array<std::uint8_t, 4>{0x71, 0x01, 0x02, 0x03}, 4,
         "31 01 0203 CheckProgrammingPreconditions");
  expect(functional_, std::array<std::uint8_t, 2>{0x85, 0x02},
         std::array<std::uint8_t, 2>{0xC5, 0x02}, 6,
         "FUNC 85 02 DisableDTCSetting");
  expect(functional_, std::array<std::uint8_t, 3>{0x28, 0x03, 0x01},
         std::array<std::uint8_t, 2>{0x68, 0x03}, 8,
         "FUNC 28 03 01 DisableCommunication");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 10,
         "10 02 ProgrammingSession");

  const auto seed_sub = protocol_.security_seed_subfunction;
  const std::array<std::uint8_t, 2> seed_request{0x27, seed_sub};
  const std::array<std::uint8_t, 2> seed_prefix{0x67, seed_sub};
  const auto seed_result = expect(physical_, seed_request, seed_prefix, 12,
                                  "27 11 RequestSeed");
  if (seed_result.response.size() != protocol_.security_seed_length + 2U) {
    throw std::runtime_error("SecurityAccess returned an unexpected seed length");
  }
  const auto seed = std::span(seed_result.response).subspan(
      2U, protocol_.security_seed_length);
  const auto key = key_generator_(seed);
  if (key.size() != protocol_.security_key_length) {
    throw std::runtime_error("SeedKey DLL returned an unexpected key length");
  }
  std::vector<std::uint8_t> key_request{
      0x27, static_cast<std::uint8_t>(seed_sub + 1U)};
  key_request.insert(key_request.end(), key.begin(), key.end());
  expect(physical_, key_request,
         std::array<std::uint8_t, 2>{
             0x67, static_cast<std::uint8_t>(seed_sub + 1U)},
         14, "27 12 SendKey", true);

  auto fingerprint = fingerprint_f184();
  std::vector<std::uint8_t> write_fingerprint{0x2E, 0xF1, 0x84};
  write_fingerprint.insert(write_fingerprint.end(), fingerprint.begin(),
                           fingerprint.end());
  expect(physical_, write_fingerprint,
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x84}, 16,
         "2E F184 ProgrammingDate", true);

  transfer_image(protocol_.driver_start, images.driver, 18, 28,
                 "FlashDriver");
  verify_crc(images.driver, 30, "FlashDriver");
  expect(physical_,
         baic_radar_erase_memory(protocol_.app_start, protocol_.app_length),
         std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x00, 0x00}, 34,
         "31 01 FF00 Erase APP");
  transfer_image(protocol_.app_start, images.app, 36, 88, "APP");
  verify_crc(images.app, 91, "APP");
  expect(physical_, std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
         std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x01, 0x00}, 94,
         "31 01 FF01 DependencyCheck");
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 96,
         "11 01 ECUReset", true);

  std::this_thread::sleep_for(2s);
  send_cleanup(std::array<std::uint8_t, 2>{0x10, 0x03}, 97,
               "FUNC 10 03 Post-reset ExtendedSession");
  send_cleanup(std::array<std::uint8_t, 3>{0x28, 0x00, 0x01}, 98,
               "FUNC 28 00 01 EnableCommunication");
  send_cleanup(std::array<std::uint8_t, 2>{0x85, 0x01}, 98,
               "FUNC 85 01 EnableDTCSetting");
  send_cleanup(std::array<std::uint8_t, 2>{0x10, 0x01}, 99,
               "FUNC 10 01 DefaultSession");
  send_cleanup(std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF}, 100,
               "FUNC 14 FFFFFF ClearDTC");
  if (log_) log_(100, protocol_.project_name + " Download completed");
}

} // namespace uds
