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
    CheryArs131D004Mode::routine_only, false, false, true, true};
const CheryArs131AppSpec kT22{
    CheryArs131Project::t22, "Chery T22 ARS1.31", 0x7AF, 0x7BF,
    0x07, 4, 4, 0xD003, 0xD002, 0xF15A, 15,
    CheryArs131D004Mode::app_signature, true, false, true, false};
const CheryArs131AppSpec kE0y{
    CheryArs131Project::e0y, "Chery E0Y ARS1.31", 0x70D, 0x78D,
    0x11, 16, 16, 0x0203, 0xDD02, 0xF184, 19,
    CheryArs131D004Mode::none, false, true, false, true};

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

void CheryArs131AppFlow::write_fingerprint(int percent) {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::vector<std::uint8_t> request{
      0x2E, static_cast<std::uint8_t>(spec_.fingerprint_did >> 8U),
      static_cast<std::uint8_t>(spec_.fingerprint_did)};
  std::vector<std::uint8_t> value(spec_.fingerprint_length, 0xFF);
  value[0] = bcd(static_cast<unsigned>((local.tm_year + 1900) % 100));
  value[1] = bcd(static_cast<unsigned>(local.tm_mon + 1));
  value[2] = bcd(static_cast<unsigned>(local.tm_mday));
  request.insert(request.end(), value.begin(), value.end());
  expect(physical_, request,
         std::array<std::uint8_t, 3>{0x6E, request[1], request[2]}, percent,
         "2E WriteFingerprint");
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

void CheryArs131AppFlow::run(const CheryArs131AppImages& images,
                             std::stop_token stop) {
  stop_ = stop;
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
  write_fingerprint(14);

  if (spec_.d004_mode != CheryArs131D004Mode::none) {
    std::vector<std::uint8_t> request{0x31, 0x01, 0xD0, 0x04};
    if (spec_.d004_mode == CheryArs131D004Mode::app_signature) {
      request.insert(request.end(), images.app_signature.begin(),
                     images.app_signature.end());
    }
    routine(request, 0xD004, 17, "31 01 D004 SecuritySignatureCheck");
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

} // namespace uds
