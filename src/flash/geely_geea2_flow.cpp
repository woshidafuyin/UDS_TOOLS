#include "flash/geely_geea2_flow.hpp"

#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace uds {
namespace {

constexpr std::size_t kTransferBlockLimit{0x1000};

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

bool starts_with(std::span<const std::uint8_t> value,
                 std::span<const std::uint8_t> prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return value;
}

} // namespace

std::vector<std::uint8_t> geely_geea2_request_download(
    std::uint8_t data_format_identifier, std::uint32_t address,
    std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, data_format_identifier, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> geely_geea2_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> geely_geea2_check_memory(
    std::span<const std::uint8_t> signature) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0x02, 0x12};
  request.insert(request.end(), signature.begin(), signature.end());
  return request;
}

std::vector<std::uint8_t> geely_geea2_activate_sbl(std::uint32_t address) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0x03, 0x01};
  append_u32(request, address);
  return request;
}

std::size_t geely_geea2_transfer_chunk_size(
    std::span<const std::uint8_t> response) {
  if (response.size() < 3U || response[0] != 0x74U) {
    throw std::runtime_error("GEEA2 invalid RequestDownload response");
  }
  const auto length_bytes = static_cast<std::size_t>(response[1] >> 4U);
  if (length_bytes == 0U || response.size() < 2U + length_bytes) {
    throw std::runtime_error("GEEA2 RequestDownload response has no max block length");
  }
  std::size_t max_block{};
  for (std::size_t index = 0; index < length_bytes; ++index) {
    max_block = (max_block << 8U) | response[2U + index];
  }
  if (max_block < 3U) {
    throw std::runtime_error("GEEA2 ECU max block length is invalid");
  }
  return std::min(max_block, kTransferBlockLimit) - 2U;
}

void validate_geely_geea2_images(
    const std::vector<GeelyGeea2Image>& images) {
  if (images.empty()) {
    throw std::runtime_error("GEEA2 requires at least one VBF image");
  }
  bool application_found{};
  bool sbl_found{};
  std::uint32_t ecu_address{};
  bool ecu_address_known{};
  for (const auto& image : images) {
    if (image.file.blocks.empty()) {
      throw std::runtime_error("GEEA2 " + image.label + " VBF has no blocks");
    }
    if (image.file.signature.empty()) {
      throw std::runtime_error("GEEA2 " + image.label +
                               " VBF has no development or production signature");
    }
    const auto type = upper(image.file.sw_part_type);
    if (image.secondary_bootloader) {
      if (sbl_found || type != "SBL" || !image.file.has_call_address) {
        throw std::runtime_error(
            "GEEA2 SBL must be unique, have sw_part_type=SBL and contain call");
      }
      sbl_found = true;
    } else {
      if (type == "SBL" || image.file.erase_ranges.empty()) {
        throw std::runtime_error("GEEA2 " + image.label +
                                 " must be a non-SBL VBF with erase metadata");
      }
      application_found = true;
    }
    if (image.file.has_ecu_address) {
      if (ecu_address_known && image.file.ecu_address != ecu_address) {
        throw std::runtime_error(
            "GEEA2 selected VBF files target different ecu_address values");
      }
      ecu_address = image.file.ecu_address;
      ecu_address_known = true;
    }
  }
  if (!application_found) {
    throw std::runtime_error("GEEA2 requires at least one APP/CAL/DATA/ESS VBF");
  }
}

GeelyGeea2Flow::GeelyGeea2Flow(UdsClient& physical, Log log, Keygen keygen,
                               GeelyGeea2Protocol protocol)
    : physical_(physical), log_(std::move(log)), keygen_(std::move(keygen)),
      protocol_(std::move(protocol)) {}

UdsResponse GeelyGeea2Flow::expect(
    std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> prefix, int percent,
    const std::string& name) {
  check_cancelled();
  if (log_) log_(percent, name);
  auto result = physical_.request(request, std::chrono::milliseconds(2000),
                                  std::chrono::milliseconds(30000), stop_);
  if (!result.success) {
    throw std::runtime_error(name + ": NRC/timeout " + result.detail);
  }
  if (!starts_with(result.response, prefix)) {
    throw std::runtime_error(name + ": response mismatch " +
                             to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  return result;
}

void GeelyGeea2Flow::check_cancelled() const {
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

void GeelyGeea2Flow::unlock() {
  const auto seed_subfunction = protocol_.security_seed_subfunction;
  if ((seed_subfunction & 1U) == 0U || seed_subfunction == 0U ||
      seed_subfunction == 0x7FU) {
    throw std::runtime_error("GEEA2 security seed subfunction must be an odd byte");
  }
  const std::array<std::uint8_t, 2> seed_request{0x27, seed_subfunction};
  const std::array<std::uint8_t, 2> seed_prefix{0x67, seed_subfunction};
  const auto seed = expect(seed_request, seed_prefix, 10, "27 RequestSeed");
  if (seed.response.size() <= 2U) {
    throw std::runtime_error("GEEA2 SecurityAccess seed is empty");
  }
  const auto key = keygen_(std::span(seed.response).subspan(2U),
                           seed_subfunction);
  if (key.empty()) {
    throw std::runtime_error("GEEA2 SeedKey provider returned an empty key");
  }
  std::vector<std::uint8_t> key_request{0x27,
      static_cast<std::uint8_t>(seed_subfunction + 1U)};
  key_request.insert(key_request.end(), key.begin(), key.end());
  const std::array<std::uint8_t, 2> key_prefix{
      0x67, static_cast<std::uint8_t>(seed_subfunction + 1U)};
  expect(key_request, key_prefix, 12, "27 SendKey");
}

void GeelyGeea2Flow::transfer_file(const GeelyGeea2Image& image,
                                   int begin_percent, int end_percent) {
  std::size_t total{};
  for (const auto& block : image.file.blocks) total += block.data.size();
  std::size_t completed_before{};
  for (std::size_t block_index = 0; block_index < image.file.blocks.size();
       ++block_index) {
    const auto& block = image.file.blocks[block_index];
    const auto response = expect(
        geely_geea2_request_download(
            image.file.data_format_identifier, block.address,
            static_cast<std::uint32_t>(block.data.size())),
        std::array<std::uint8_t, 1>{0x74}, begin_percent,
        "34 RequestDownload " + image.label + " block " +
            std::to_string(block_index + 1U));
    const auto chunk_size = geely_geea2_transfer_chunk_size(response.response);
    std::size_t offset{};
    std::uint8_t sequence{1};
    while (offset < block.data.size()) {
      check_cancelled();
      const auto count = std::min(chunk_size, block.data.size() - offset);
      std::vector<std::uint8_t> transfer{0x36, sequence};
      transfer.insert(
          transfer.end(),
          block.data.begin() + static_cast<std::ptrdiff_t>(offset),
          block.data.begin() + static_cast<std::ptrdiff_t>(offset + count));
      const auto completed = completed_before + offset + count;
      const auto percent = begin_percent + static_cast<int>(
          (end_percent - begin_percent) * static_cast<double>(completed) /
          static_cast<double>(total));
      expect(transfer, std::array<std::uint8_t, 2>{0x76, sequence}, percent,
             "36 TransferData " + image.label);
      offset += count;
      sequence = static_cast<std::uint8_t>(sequence + 1U);
    }
    expect(std::array<std::uint8_t, 1>{0x37},
           std::array<std::uint8_t, 1>{0x77}, end_percent,
           "37 RequestTransferExit " + image.label);
    completed_before += block.data.size();
  }
}

void GeelyGeea2Flow::erase_file(const GeelyGeea2Image& image, int percent) {
  for (std::size_t index = 0; index < image.file.erase_ranges.size(); ++index) {
    const auto& range = image.file.erase_ranges[index];
    expect(geely_geea2_erase_memory(range.address, range.length),
           std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x00, 0x10},
           percent, "31 EraseMemory " + image.label + " range " +
                        std::to_string(index + 1U));
  }
}

void GeelyGeea2Flow::verify_file(const GeelyGeea2Image& image, int percent) {
  expect(geely_geea2_check_memory(image.file.signature),
         std::array<std::uint8_t, 6>{0x71, 0x01, 0x02, 0x12, 0x10, 0x00},
         percent, "31 CheckMemory " + image.label);
}

void GeelyGeea2Flow::run(const std::vector<GeelyGeea2Image>& images,
                         std::stop_token stop) {
  stop_ = stop;
  core_programming_completed_ = false;
  validate_geely_geea2_images(images);

  expect(std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession");
  expect(protocol_.programming_precondition,
         std::array<std::uint8_t, 6>{0x71, 0x01, 0x02, 0x06, 0x10, 0x00},
         5, "31 CheckProgrammingPreconditions");
  expect(std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 8,
         "10 02 ProgrammingSession");
  unlock();

  const auto count = static_cast<int>(images.size());
  for (int index = 0; index < count; ++index) {
    const auto& image = images[static_cast<std::size_t>(index)];
    const auto begin = 15 + (75 * index) / count;
    const auto end = 15 + (75 * (index + 1)) / count;
    if (!image.secondary_bootloader) erase_file(image, begin);
    transfer_file(image, begin + 1, end - 3);
    verify_file(image, end - 2);
    if (image.secondary_bootloader) {
      expect(geely_geea2_activate_sbl(image.file.call_address),
             std::array<std::uint8_t, 5>{0x71, 0x01, 0x03, 0x01, 0x10},
             end - 1, "31 ActivateSecondaryBootloader");
    }
  }

  expect(protocol_.complete_and_compatible,
         std::array<std::uint8_t, 6>{0x71, 0x01, 0x02, 0x05, 0x10, 0x00},
         94, "31 CheckCompleteAndCompatible");
  expect(protocol_.report_dtc,
         std::array<std::uint8_t, 2>{0x59, 0x02}, 96,
         "19 02 08 ReportDTCByStatusMask");
  expect(std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 99, "11 01 ECUReset");
  core_programming_completed_ = true;
  if (log_) log_(100, "GEEA2 normal download sequence complete");
}

bool GeelyGeea2Flow::core_programming_completed() const noexcept {
  return core_programming_completed_;
}

} // namespace uds
