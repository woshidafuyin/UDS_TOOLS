#include "flash/chery_ars1_31_app_flow.hpp"

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

const CheryArs131AppSpec kT1ej{
    CheryArs131Project::t1ej, "Chery T1EJ ARS1.31", 0x7AF, 0x7BF,
    0x07, 4, 4, 0xD003, 0xD002, 0xF15A, 15,
    CheryArs131D004Mode::routine_only, 0ms, false, false, true, true};
const CheryArs131AppSpec kT22{
    CheryArs131Project::t22, "Chery T22 ARS1.31", 0x7AF, 0x7BF,
    0x07, 4, 4, 0xD003, 0xD002, 0xF15A, 15,
    CheryArs131D004Mode::app_signature, 2000ms, true, false, true, false};
const CheryArs131AppSpec kE0y{
    CheryArs131Project::e0y, "Chery E0Y ARS1.31", 0x70D, 0x78D,
    0x11, 16, 16, 0x0203, 0xDD02, 0xF184, 19,
    CheryArs131D004Mode::none, 0ms, false, true, false, true};

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

std::uint8_t bcd(unsigned value) {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | value % 10U);
}

std::size_t max_block_length(std::span<const std::uint8_t> response) {
  if (response.size() < 3 || response[0] != 0x74) {
    throw std::runtime_error("invalid RequestDownload response");
  }
  const auto bytes = static_cast<std::size_t>((response[1] >> 4U) & 0x0F);
  if (bytes == 0 || response.size() < 2 + bytes) {
    throw std::runtime_error("RequestDownload response has no block length");
  }
  std::size_t result{};
  for (std::size_t i = 0; i < bytes; ++i) result = (result << 8U) | response[2 + i];
  if (result <= 2) throw std::runtime_error("ECU block length is too small");
  return std::min<std::size_t>(result, 0x0FFF);
}

void require_size(std::span<const std::uint8_t> bytes, std::size_t expected,
                  const std::string& label) {
  if (bytes.size() != expected) {
    throw std::runtime_error(label + " must be exactly " +
                             std::to_string(expected) + " bytes");
  }
}
} // namespace

const CheryArs131AppSpec& chery_ars1_31_app_spec(
    CheryArs131Project project) {
  switch (project) {
    case CheryArs131Project::t1ej: return kT1ej;
    case CheryArs131Project::t22: return kT22;
    case CheryArs131Project::e0y: return kE0y;
  }
  throw std::invalid_argument("unsupported Chery ARS1.31 project");
}

CheryArs131DownloadPlan resolve_chery_ars1_31_download_plan(
    CheryArs131Project project, std::wstring_view entry_mode) {
  if (entry_mode.empty() || entry_mode == L"app") {
    return {CheryArs131FlashMode::app_only, true, false};
  }
  if (entry_mode == L"cal") {
    return {CheryArs131FlashMode::cal_only, false, true};
  }
  if (entry_mode == L"app_cal") {
    return {CheryArs131FlashMode::app_cal, true, true};
  }
  throw std::invalid_argument("unsupported Chery ARS1.31 flashing mode");
}

std::vector<std::uint8_t> chery_ars1_31_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> result{0x34, 0x00, 0x44};
  append_u32(result, address);
  append_u32(result, length);
  return result;
}

std::vector<std::uint8_t> chery_ars1_31_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> result{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32(result, address);
  append_u32(result, length);
  return result;
}

std::vector<std::uint8_t> chery_e0y_update_public_key_request() {
  // Frozen byte-for-byte from FlashChery_E0Y_20240410/Capl/Flash.can
  // publickeydata[514]. SHA-256 of the payload:
  // 517fb58290445bacca042a72594544eb64e5a41cd34a9e4a4e7243ea66e60762
  static constexpr std::array<std::uint8_t, 514> public_key{
    0xB0, 0xB2, 0xB2, 0xC5, 0x44, 0xBD, 0x1E, 0x85, 0xD2, 0x73, 0x5B, 0x0A,
    0xC0, 0x2E, 0x6F, 0x4C, 0xF9, 0xA5, 0xE1, 0x42, 0x45, 0x02, 0xE9, 0xC6,
    0xC8, 0x6D, 0x85, 0x83, 0x1B, 0x7E, 0xD1, 0x60, 0xCC, 0xF8, 0x59, 0x02,
    0x7E, 0xDD, 0xB0, 0xC1, 0x55, 0x8B, 0x77, 0xEB, 0x74, 0xA9, 0x97, 0x97,
    0xFB, 0x09, 0x3F, 0x13, 0xAF, 0x45, 0xAB, 0x47, 0x60, 0x58, 0xAA, 0xC6,
    0xCA, 0x5A, 0xCC, 0x71, 0x39, 0xFE, 0x32, 0xA0, 0xBB, 0xC2, 0x56, 0xB8,
    0x04, 0x4C, 0xDA, 0x31, 0x83, 0x2C, 0xCE, 0xC1, 0xD5, 0x2A, 0x75, 0xCC,
    0x95, 0xC1, 0x25, 0xE8, 0xF6, 0xA2, 0x60, 0x69, 0xD0, 0x5D, 0x01, 0x07,
    0x3B, 0xD8, 0xF9, 0x3C, 0xF0, 0xEF, 0x84, 0x5C, 0x09, 0xF2, 0xAF, 0x00,
    0xAF, 0x27, 0xAF, 0xF5, 0x97, 0x9B, 0xEF, 0xAF, 0x67, 0xCD, 0x0C, 0x82,
    0xE5, 0x0B, 0x56, 0xDE, 0x80, 0xC9, 0x90, 0xAB, 0x7B, 0x18, 0x08, 0x3F,
    0x8F, 0x18, 0xCB, 0x1E, 0xD0, 0xAC, 0xA9, 0x3A, 0xBB, 0x93, 0x22, 0x27,
    0x69, 0xC8, 0x14, 0x2A, 0xAA, 0xC4, 0xE9, 0x73, 0x48, 0x02, 0x3E, 0xDA,
    0x4C, 0x39, 0xE4, 0x08, 0x5E, 0x55, 0xB1, 0xD8, 0x59, 0x54, 0xB4, 0x90,
    0xF7, 0x67, 0x10, 0xFF, 0x8A, 0xE8, 0x30, 0xB1, 0x71, 0xA9, 0x94, 0xCE,
    0xE7, 0xCD, 0xAB, 0x9C, 0xED, 0xB0, 0xB3, 0x5A, 0x5A, 0x7C, 0x89, 0x92,
    0x7E, 0xEE, 0x00, 0x11, 0x90, 0x51, 0x9D, 0xF5, 0x9C, 0x9D, 0x77, 0xF9,
    0x32, 0x3C, 0x12, 0x54, 0x18, 0x9A, 0x72, 0xB8, 0x87, 0x35, 0x20, 0x82,
    0x89, 0x5A, 0x2E, 0x5B, 0xA8, 0x47, 0x9E, 0x6F, 0x13, 0xAB, 0x22, 0xFE,
    0xDC, 0xA9, 0x87, 0xB6, 0xF6, 0xF8, 0x29, 0xF3, 0x5C, 0x97, 0x29, 0xBB,
    0x85, 0xF7, 0x66, 0xD6, 0x6B, 0x92, 0xF3, 0xEB, 0x27, 0xF4, 0x7D, 0x42,
    0x8B, 0xA6, 0xE8, 0x37, 0xA4, 0xF1, 0x1D, 0xA6, 0x7D, 0xA4, 0x0B, 0x63,
    0xC6, 0x7C, 0x98, 0xB7, 0xA0, 0xDD, 0x2E, 0x39, 0x6C, 0x6A, 0x84, 0x95,
    0xDD, 0xAB, 0xC1, 0x91, 0x00, 0x3F, 0xF1, 0xF5, 0x18, 0x9D, 0xCB, 0xD9,
    0x9E, 0x86, 0xF0, 0x31, 0x48, 0xE2, 0x77, 0x94, 0xDF, 0x4B, 0x08, 0x75,
    0xAF, 0x4B, 0x57, 0x6A, 0x51, 0x11, 0x2A, 0x39, 0x8F, 0x5F, 0x05, 0xB7,
    0x9F, 0xEB, 0x9A, 0x8C, 0xA7, 0xF1, 0x88, 0x3C, 0xE8, 0xD0, 0x1B, 0x74,
    0xE0, 0x32, 0x97, 0x09, 0xE9, 0x02, 0x11, 0x4D, 0xAF, 0xE0, 0xE3, 0x44,
    0x32, 0xDF, 0xD6, 0x8B, 0x70, 0x93, 0xE3, 0x2C, 0xF3, 0xBA, 0x4A, 0xC5,
    0xDA, 0xFB, 0x7A, 0xB8, 0x6B, 0x14, 0xC5, 0x19, 0xB3, 0xDF, 0xF8, 0x96,
    0x2B, 0xA9, 0x0A, 0xC5, 0xC5, 0x7C, 0xF8, 0x90, 0x64, 0x4D, 0xBD, 0x33,
    0x2C, 0x54, 0xE2, 0x7D, 0x42, 0x24, 0xF1, 0x1D, 0x38, 0xC6, 0x4A, 0xDB,
    0xAB, 0x8E, 0xC5, 0x94, 0xAB, 0xB2, 0x39, 0x86, 0xB7, 0x5F, 0x0A, 0x90,
    0x6D, 0xEA, 0xF5, 0x68, 0x14, 0x7A, 0x45, 0xA6, 0xDC, 0x87, 0x96, 0x2A,
    0x78, 0xA7, 0x3B, 0x60, 0x1C, 0x06, 0xC6, 0x7F, 0xFA, 0x53, 0x83, 0x51,
    0x7E, 0xD6, 0x18, 0x42, 0x3C, 0x3C, 0x50, 0xD9, 0x57, 0x7B, 0x10, 0xA5,
    0x4E, 0x22, 0xC6, 0x19, 0xB2, 0x4A, 0xC0, 0x94, 0xB8, 0xCF, 0x6F, 0xD5,
    0x02, 0xCE, 0x97, 0x16, 0x5E, 0x85, 0x3B, 0xA9, 0x02, 0x3F, 0xDB, 0xE1,
    0x3D, 0xF0, 0xBD, 0xCF, 0x22, 0x90, 0x5D, 0x03, 0x42, 0x3C, 0x47, 0x88,
    0x2E, 0xD5, 0x18, 0xA4, 0xF9, 0xD0, 0x3B, 0xBB, 0x49, 0x61, 0x70, 0x01,
    0x17, 0x6C, 0xC4, 0xB0, 0xD6, 0x75, 0xEA, 0x1B, 0x37, 0x49, 0xF8, 0xED,
    0x7F, 0xA9, 0x26, 0xCC, 0x06, 0xE4, 0xE4, 0x65, 0x68, 0x05, 0xDC, 0x52,
    0x0B, 0x71, 0x5E, 0x08, 0xC2, 0x79, 0xCF, 0x59, 0x26, 0xF5,
  };
  std::vector<std::uint8_t> request{0x2E, 0x6F, 0x00};
  request.insert(request.end(), public_key.begin(), public_key.end());
  return request;
}

CheryArs131AppFlow::CheryArs131AppFlow(
    UdsClient& physical, UdsClient& functional, CheryArs131AppSpec spec,
    CheryArs131AppLayout layout, Log log, KeyGenerator key_generator)
    : physical_(physical), functional_(functional), spec_(std::move(spec)),
      layout_(layout), log_(std::move(log)),
      key_generator_(std::move(key_generator)) {}

void CheryArs131AppFlow::cancelled() const {
  if (stop_.stop_requested()) throw std::runtime_error("operation cancelled by user");
}

void CheryArs131AppFlow::wait(std::chrono::milliseconds duration) const {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    cancelled();
    std::this_thread::sleep_for(50ms);
  }
}

UdsResponse CheryArs131AppFlow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> prefix, int percent,
    const std::string& name) {
  cancelled();
  if (log_) log_(percent, name);
  auto result = client.request(request, 2000ms, 5000ms);
  if (!result.success) throw std::runtime_error(name + ": " + result.detail);
  if (result.response.size() < prefix.size() ||
      !std::equal(prefix.begin(), prefix.end(), result.response.begin())) {
    throw std::runtime_error(name + ": response mismatch " + to_hex(result.response));
  }
  if (log_) log_(percent, name + " PASS: " + to_hex(result.response));
  wait(50ms);
  return result;
}

UdsResponse CheryArs131AppFlow::routine(
    std::span<const std::uint8_t> request, std::uint16_t routine_id,
    int percent, const std::string& name, bool allow_status_one) {
  const std::array<std::uint8_t, 4> prefix{
      0x71, 0x01, static_cast<std::uint8_t>(routine_id >> 8U),
      static_cast<std::uint8_t>(routine_id)};
  auto result = expect(physical_, request, prefix, percent, name);
  if (result.response.size() < 5 ||
      (result.response[4] != 0 && !(allow_status_one && result.response[4] == 1))) {
    throw std::runtime_error(name + ": routine status " + to_hex(result.response));
  }
  return result;
}

void CheryArs131AppFlow::functional_send(
    std::span<const std::uint8_t> request, int percent,
    const std::string& name) {
  cancelled();
  if (log_) log_(percent, name);
  functional_.send_only(request);
  wait(50ms);
}

void CheryArs131AppFlow::precondition(int percent) {
  const std::array<std::uint8_t, 4> request{
      0x31, 0x01, static_cast<std::uint8_t>(spec_.precondition_routine >> 8U),
      static_cast<std::uint8_t>(spec_.precondition_routine)};
  routine(request, spec_.precondition_routine, percent,
          "31 01 precondition " + spec_.name);
}

void CheryArs131AppFlow::unlock(int percent) {
  const std::array<std::uint8_t, 2> seed_request{0x27, spec_.seed_subfunction};
  const std::array<std::uint8_t, 2> seed_prefix{0x67, spec_.seed_subfunction};
  auto seed = expect(physical_, seed_request, seed_prefix, percent, "27 RequestSeed");
  if (seed.response.size() != spec_.seed_length + 2) {
    throw std::runtime_error(spec_.name + " seed length mismatch");
  }
  auto key = key_generator_(std::span(seed.response).subspan(2),
                            spec_.seed_subfunction);
  require_size(key, spec_.key_length, spec_.name + " key");
  const auto key_subfunction = static_cast<std::uint8_t>(spec_.seed_subfunction + 1);
  std::vector<std::uint8_t> request{0x27, key_subfunction};
  request.insert(request.end(), key.begin(), key.end());
  expect(physical_, request,
         std::array<std::uint8_t, 2>{0x67, key_subfunction}, percent + 2,
         "27 SendKey");
}

void CheryArs131AppFlow::write_fingerprint(int percent, std::uint16_t did,
                                           std::size_t length) {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::vector<std::uint8_t> request{
      0x2E, static_cast<std::uint8_t>(did >> 8U),
      static_cast<std::uint8_t>(did)};
  std::vector<std::uint8_t> value(length, 0xFF);
  value[0] = bcd(static_cast<unsigned>((local.tm_year + 1900) % 100));
  value[1] = bcd(static_cast<unsigned>(local.tm_mon + 1));
  value[2] = bcd(static_cast<unsigned>(local.tm_mday));
  request.insert(request.end(), value.begin(), value.end());
  expect(physical_, request,
         std::array<std::uint8_t, 3>{0x6E, request[1], request[2]}, percent,
         "2E WriteFingerprint");
}

void CheryArs131AppFlow::write_public_key(int percent) {
  const auto request = chery_e0y_update_public_key_request();
  expect(physical_, request,
         std::array<std::uint8_t, 3>{0x6E, 0x6F, 0x00}, percent,
         "2E 6F00 Update_PublicKey");
}

void CheryArs131AppFlow::verify(
    std::uint16_t routine_id, std::span<const std::uint8_t> signature,
    int percent, const std::string& label) {
  require_size(signature, 512, label);
  std::vector<std::uint8_t> request{
      0x31, 0x01, static_cast<std::uint8_t>(routine_id >> 8U),
      static_cast<std::uint8_t>(routine_id)};
  request.insert(request.end(), signature.begin(), signature.end());
  routine(request, routine_id, percent, label);
}

void CheryArs131AppFlow::transfer(
    std::uint32_t address, std::span<const std::uint8_t> image,
    int begin_percent, int end_percent, const std::string& label) {
  const auto response = expect(
      physical_, chery_ars1_31_request_download(
                     address, static_cast<std::uint32_t>(image.size())),
      std::array<std::uint8_t, 1>{0x74}, begin_percent, "34 " + label);
  const auto chunk = max_block_length(response.response) - 2;
  std::size_t offset{};
  std::uint8_t sequence{1};
  while (offset < image.size()) {
    const auto count = std::min(chunk, image.size() - offset);
    std::vector<std::uint8_t> request{0x36, sequence};
    request.insert(request.end(), image.begin() + static_cast<std::ptrdiff_t>(offset),
                   image.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const auto progress = begin_percent + static_cast<int>(
        (end_percent - begin_percent) *
        static_cast<double>(offset + count) / image.size());
    expect(physical_, request,
           std::array<std::uint8_t, 2>{0x76, sequence}, progress,
           "36 " + label);
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1);
  }
  expect(physical_, std::array<std::uint8_t, 1>{0x37},
         std::array<std::uint8_t, 1>{0x77}, end_percent, "37 " + label);
}

void CheryArs131AppFlow::run_app_only(
    const CheryArs131AppImages& images, bool update_public_key) {
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.app, layout_.app_length, "APP");
  require_size(images.driver_signature, 512, "Driver signature");
  require_size(images.app_signature, 512, "APP signature");

  if (spec_.project == CheryArs131Project::t22) {
    if (log_) log_(0, "T22 FileInit-equivalent resources loaded; settle 1 s");
    wait(1000ms);
  }

  if (spec_.initial_physical_extended_session) {
    expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
           std::array<std::uint8_t, 2>{0x50, 0x03}, 1,
           "10 03 ExtendedSession");
  } else {
    functional_send(std::array<std::uint8_t, 2>{0x10, 0x83}, 1,
                    "FUNC 10 83 ExtendedSession suppressed");
  }
  if (spec_.precondition_before_network_disable) precondition(3);
  functional_send(std::array<std::uint8_t, 2>{0x85, 0x82}, 4,
                  "FUNC 85 82 DisableDTCSetting");
  functional_send(std::array<std::uint8_t, 3>{0x28, 0x81, 0x03}, 5,
                  "FUNC 28 81 03 DisableCommunication");
  if (!spec_.precondition_before_network_disable) precondition(6);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 8,
         "10 02 ProgrammingSession");
  unlock(10);
  if (update_public_key) write_public_key(13);
  write_fingerprint(14, spec_.fingerprint_did, spec_.fingerprint_length);

  if (spec_.d004_mode != CheryArs131D004Mode::none) {
    std::vector<std::uint8_t> request{0x31, 0x01, 0xD0, 0x04};
    if (spec_.d004_mode == CheryArs131D004Mode::app_signature) {
      request.insert(request.end(), images.app_signature.begin(),
                     images.app_signature.end());
    }
    routine(request, 0xD004, 17, "31 01 D004 SecuritySignatureCheck");
    wait(spec_.post_d004_delay);
  }

  transfer(layout_.driver_start, images.driver, 20, 31, "Driver");
  wait(2000ms);
  verify(spec_.verification_routine, images.driver_signature, 34,
         "Verify Driver signature");
  wait(2000ms);
  routine(chery_ars1_31_erase_memory(layout_.app_start, layout_.app_length),
          0xFF00, 38, "31 01 FF00 EraseAPP");
  transfer(layout_.app_start, images.app, 42, 82, "APP");
  wait(2000ms);
  verify(spec_.verification_routine, images.app_signature, 86,
         "Verify APP signature");
  wait(2000ms);
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
          0xFF01, 90, "31 01 FF01 CheckProgrammingDependencies");
  if (spec_.install_d005) {
    routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xD0, 0x05},
            0xD005, 93, "31 01 D005 FlashFileInstallation", true);
  }
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 96, "11 01 HardReset");
  wait(2000ms);
  if (spec_.restore_default_session) {
    functional_send(std::array<std::uint8_t, 2>{0x10, 0x81}, 98,
                    "FUNC 10 81 DefaultSession suppressed");
  }
  expect(functional_, std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, 99,
         "FUNC 14 FF FF FF ClearDTC");
  if (log_) log_(100, spec_.name + " normal APP flow completed");
}

void CheryArs131AppFlow::run_cal_only(
    const CheryArs131AppImages& images, bool update_public_key) {
  if (spec_.project == CheryArs131Project::t22) {
    run_t22_cal_only(images);
    return;
  }
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.cal, layout_.cal_length, "CAL");
  require_size(images.driver_signature, 512, "Driver signature");
  require_size(images.cal_signature, 512, "CAL signature");

  if (log_) log_(0, spec_.name + " CAL/TC_7 resources loaded; settle 1 s");
  wait(1000ms);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession (TC_7)");
  precondition(5);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 8,
         "10 02 ProgrammingSession");
  unlock(11);
  if (update_public_key) write_public_key(14);
  write_fingerprint(15, 0xF184, 19);
  wait(2000ms);

  transfer(layout_.driver_start, images.driver, 19, 34, "Driver");
  wait(2000ms);
  verify(0xDD02, images.driver_signature, 38, "Verify Driver signature");
  wait(2000ms);
  routine(chery_ars1_31_erase_memory(layout_.cal_start, layout_.cal_length),
          0xFF00, 44, "31 01 FF00 EraseCAL");
  transfer(layout_.cal_start, images.cal, 49, 72, "CAL");
  wait(2000ms);
  verify(0xDD02, images.cal_signature, 78, "Verify CAL signature");
  wait(2000ms);
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
          0xFF01, 84, "31 01 FF01 CheckProgrammingDependencies");
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xDD, 0x03},
          0xDD03, 90, "31 01 DD03 FlashFileInstallation");
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 96, "11 01 HardReset");
  expect(functional_, std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, 99,
         "FUNC 14 FF FF FF ClearDTC");
  if (log_) log_(100, spec_.name + " CAL/TC_7 flow completed");
}

void CheryArs131AppFlow::run_t22_cal_only(
    const CheryArs131AppImages& images) {
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.cal, layout_.cal_length, "CAL");
  require_size(images.driver_signature, 512, "Driver signature");
  require_size(images.app_signature, 512, "APP signature for D004");
  require_size(images.cal_signature, 512, "CAL signature");

  if (log_) log_(0, "Chery T22 CAL/TC_7 resources loaded; settle 1 s");
  wait(1000ms);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession (TC_7)");
  functional_send(std::array<std::uint8_t, 2>{0x85, 0x82}, 4,
                  "FUNC 85 82 DisableDTCSetting");
  functional_send(std::array<std::uint8_t, 3>{0x28, 0x81, 0x03}, 5,
                  "FUNC 28 81 03 DisableCommunication");
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xD0, 0x03},
          0xD003, 7, "31 01 D003 TC_7 precondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 9,
         "10 02 ProgrammingSession");
  unlock(11);
  write_fingerprint(15, 0xF15A, 15);
  verify(0xD004, images.app_signature, 18,
         "31 01 D004 SecuritySignatureCheck(APP RSA)");
  wait(2000ms);

  transfer(layout_.driver_start, images.driver, 22, 36, "Driver");
  wait(2000ms);
  verify(0xD002, images.driver_signature, 40, "Verify Driver signature");
  wait(2000ms);
  routine(chery_ars1_31_erase_memory(layout_.cal_start, layout_.cal_length),
          0xFF00, 46, "31 01 FF00 EraseCAL");
  transfer(layout_.cal_start, images.cal, 51, 73, "CAL");
  wait(2000ms);
  verify(0xD002, images.cal_signature, 78, "Verify CAL signature");

  functional_send(std::array<std::uint8_t, 3>{0x28, 0x00, 0x03}, 82,
                  "FUNC 28 00 03 EnableCommunication");
  wait(100ms);
  functional_send(std::array<std::uint8_t, 2>{0x85, 0x01}, 85,
                  "FUNC 85 01 EnableDTCSetting");
  wait(2000ms);
  // FlashKP31.can::TC_7 has FF01 and DD03 commented out. Keep them absent.
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 96, "11 01 HardReset");
  expect(functional_, std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, 99,
         "FUNC 14 FF FF FF ClearDTC");
  if (log_) log_(100, "Chery T22 CAL/TC_7 flow completed");
}

void CheryArs131AppFlow::run_app_cal(
    const CheryArs131AppImages& images) {
  require_size(images.driver, layout_.driver_length, "Driver");
  require_size(images.app, layout_.app_length, "APP");
  require_size(images.cal, layout_.cal_length, "CAL");
  require_size(images.driver_signature, 512, "Driver signature");
  require_size(images.app_signature, 512, "APP signature");
  require_size(images.cal_signature, 512, "CAL signature");

  if (log_) log_(0, spec_.name + " APPAndCAL/TC_2 resources loaded; settle 1 s");
  wait(1000ms);
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 2,
         "10 03 ExtendedSession (TC_2)");
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xD0, 0x03},
          0xD003, 4, "31 01 D003 TC_2 precondition");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 6,
         "10 02 ProgrammingSession");
  unlock(8);
  const auto fingerprint_did =
      spec_.project == CheryArs131Project::t22 ? 0xF15A : 0xF184;
  const auto fingerprint_length =
      spec_.project == CheryArs131Project::t22 ? 15U : 19U;
  write_fingerprint(12, fingerprint_did, fingerprint_length);
  wait(2000ms);
  verify(0xD004, images.app_signature, 15,
         "31 01 D004 SecuritySignatureCheck(APP RSA)");

  transfer(layout_.driver_start, images.driver, 18, 28, "Driver");
  wait(2000ms);
  verify(0xD002, images.driver_signature, 31, "Verify Driver signature");
  wait(2000ms);
  routine(chery_ars1_31_erase_memory(layout_.app_start, layout_.app_length),
          0xFF00, 35, "31 01 FF00 EraseAPP");
  transfer(layout_.app_start, images.app, 39, 67, "APP");
  wait(2000ms);
  verify(0xD002, images.app_signature, 70, "Verify APP signature");
  routine(chery_ars1_31_erase_memory(layout_.cal_start, layout_.cal_length),
          0xFF00, 74, "31 01 FF00 EraseCAL");
  transfer(layout_.cal_start, images.cal, 77, 84, "CAL");
  wait(2000ms);
  verify(0xD002, images.cal_signature, 87, "Verify CAL signature");
  wait(2000ms);
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
          0xFF01, 90, "31 01 FF01 CheckProgrammingDependencies");
  routine(std::array<std::uint8_t, 4>{0x31, 0x01, 0xD0, 0x05},
          0xD005, 93, "31 01 D005 FlashFileInstallation", true);
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 96, "11 01 HardReset");
  expect(functional_, std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, 99,
         "FUNC 14 FF FF FF ClearDTC");
  if (log_) log_(100, spec_.name + " APPAndCAL/TC_2 flow completed");
}

void CheryArs131AppFlow::run(const CheryArs131AppImages& images,
                             CheryArs131FlashMode mode,
                             bool update_public_key,
                             std::stop_token stop) {
  stop_ = stop;
  switch (mode) {
    case CheryArs131FlashMode::app_only:
      run_app_only(images, update_public_key);
      return;
    case CheryArs131FlashMode::cal_only:
      run_cal_only(images, update_public_key);
      return;
    case CheryArs131FlashMode::app_cal:
      run_app_cal(images);
      return;
  }
  throw std::invalid_argument("unsupported Chery ARS1.31 flashing mode");
}

} // namespace uds
