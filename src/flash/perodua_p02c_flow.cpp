#include "flash/perodua_p02c_flow.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace uds {
namespace {
using namespace std::chrono_literals;
void u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift : {24, 16, 8, 0}) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void validate_block(const PeroduaMemoryBlock& block) {
  if (block.image.segments.empty()) throw std::runtime_error(block.name + ": no firmware segments");
  for (const auto& segment : block.image.segments) {
    const auto end = static_cast<std::uint64_t>(segment.address) + segment.data.size();
    if (segment.data.empty() || segment.data.size() > 0xFFFFFFFFULL || end > 0x100000000ULL)
      throw std::runtime_error(block.name + ": empty segment or address/length cannot be encoded in UDS");
  }
}
} // namespace

PeroduaMode perodua_mode(std::wstring_view entry) {
  if (entry == L"app") return PeroduaMode::app;
  if (entry == L"cal") return PeroduaMode::cal;
  if (entry == L"app_cal") return PeroduaMode::app_cal;
  throw std::invalid_argument("Perodua supports APP, CAL and APP+CAL; FT/PLS and A/B switching are not defined for this ECU");
}

std::vector<std::uint8_t> perodua_fingerprint(const std::tm& date,
                                             std::wstring_view identity) {
  const auto year = date.tm_year + 1900;
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int days[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 2000 || year > 2099 || date.tm_mon < 0 || date.tm_mon > 11 ||
      date.tm_mday < 1 || date.tm_mday > days[date.tm_mon])
    throw std::invalid_argument("invalid F107 programming date");
  if (identity.empty() || identity.size() > 27 ||
      !std::all_of(identity.begin(), identity.end(), [](wchar_t c) { return c >= 0x20 && c <= 0x7E; }))
    throw std::invalid_argument("F107 tester identity must be 1..27 printable ASCII characters");
  const auto bcd = [](unsigned value) { return static_cast<std::uint8_t>((value / 10) * 16 + value % 10); };
  std::vector<std::uint8_t> result(30, 0x20);
  result[0] = bcd(static_cast<unsigned>(year % 100));
  result[1] = bcd(static_cast<unsigned>(date.tm_mon + 1));
  result[2] = bcd(static_cast<unsigned>(date.tm_mday));
  std::transform(identity.begin(), identity.end(), result.begin() + 3,
                 [](wchar_t c) { return static_cast<std::uint8_t>(c); });
  return result;
}

std::uint32_t perodua_crc32(std::span<const std::uint8_t> bytes, bool reflected) {
  // CES009 p14: polynomial 04C11DB7, init/xorout FFFFFFFF.
  // Reflected IEEE variant; validate reflection/coverage with first ECU sample.
  std::uint32_t crc = 0xFFFFFFFF;
  for (auto byte : bytes) {
    if (reflected) {
      crc ^= byte;
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc & 1U) ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    } else {
      crc ^= static_cast<std::uint32_t>(byte) << 24U;
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80000000U) ? (crc << 1U) ^ 0x04C11DB7U : crc << 1U;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::size_t perodua_max_block_length(std::span<const std::uint8_t> response) {
  if (response.size() < 3 || response[0] != 0x74 || (response[1] & 0x0FU) != 0)
    throw std::runtime_error("invalid Perodua RequestDownload response");
  const std::size_t count = response[1] >> 4U;
  if (count == 0 || count > 4 || response.size() != count + 2)
    throw std::runtime_error("invalid Perodua maxNumberOfBlockLength encoding");
  std::uint32_t length{};
  for (std::size_t i = 0; i < count; ++i) length = (length << 8U) | response[i + 2];
  // CES012 p38/p40: all non-final blocks use the ECU's negotiated length,
  // including SID and BSC. Do not silently clamp a larger negotiated block.
  if (length < 3 || length > 4095)
    throw std::runtime_error("Perodua maxNumberOfBlockLength must be 3..4095 bytes");
  return length;
}

void validate_perodua_images(const PeroduaImages& images) {
  validate_block(images.driver);
  if (images.modules.empty()) throw std::runtime_error("no Perodua APP/CAL selected");
  for (const auto& block : images.modules) {
    validate_block(block);
  }
}

SRecordImage load_perodua_image(const std::filesystem::path& file,
                                std::uint32_t binary_address) {
  if (file.empty() || !std::filesystem::is_regular_file(file))
    throw std::runtime_error("Perodua firmware file is missing");
  auto ext = file.extension().wstring();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
  if (ext == L".bin") {
    const auto size = std::filesystem::file_size(file);
    if (size == 0 || size > 0xFFFFFFFFULL || size + binary_address > 0x100000000ULL)
      throw std::runtime_error("Perodua BIN address/length cannot be encoded in UDS");
    std::ifstream input(file, std::ios::binary);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
      throw std::runtime_error("cannot read Perodua BIN");
    return {{{binary_address, std::move(data)}}, static_cast<std::size_t>(size)};
  }
  return load_srecord_image(file);
}

PeroduaP02cFlow::PeroduaP02cFlow(PeroduaIo io, bool crc_reflected)
    : io_(std::move(io)), crc_reflected_(crc_reflected) {
  if (!io_.request || !io_.functional_send || !io_.wait || !io_.key)
    throw std::invalid_argument("Perodua protocol callbacks are incomplete");
}
void PeroduaP02cFlow::check() const {
  if (stop_.stop_requested()) throw std::runtime_error("Perodua flashing cancelled");
  if (io_.health_check) io_.health_check();
}
void PeroduaP02cFlow::progress(int percent, const std::string& message) const {
  check();
  if (io_.progress) io_.progress(percent, message);
}

std::vector<std::uint8_t> PeroduaP02cFlow::exchange(
    PeroduaEndpoint endpoint, const std::vector<std::uint8_t>& request,
    const std::vector<std::uint8_t>& prefix, const std::string& label, bool retry) {
  for (unsigned attempt = 0;; ++attempt) {
    check();
    UdsResponse result;
    try {
      result = io_.request(endpoint, request, 150ms, 5000ms);
    } catch (const UdsResponseTimeout&) {
      check();
      if (!retry || attempt >= 2) throw;
      if (io_.progress) io_.progress(-1, label + ": response timeout, repeating identical request (max 2 retries)");
      continue;
    }
    check();
    if (!result.success) throw std::runtime_error(label + ": " + result.detail);
    if (result.response.size() < prefix.size() ||
        !std::equal(prefix.begin(), prefix.end(), result.response.begin()))
      throw std::runtime_error(label + ": response echo/length mismatch");
    return result.response;
  }
}

void PeroduaP02cFlow::functional(std::initializer_list<std::uint8_t> request) {
  check();
  const std::vector<std::uint8_t> bytes(request);
  io_.functional_send(bytes);
  io_.wait(50ms);
  check();
}

void PeroduaP02cFlow::routine(const std::vector<std::uint8_t>& request,
                             const std::string& label) {
  const auto reply = exchange(PeroduaEndpoint::ecu, request,
                              {0x71, 0x01, request[2], request[3]}, label);
  if (reply.size() != 5 || reply[4] != 0)
    throw std::runtime_error(label + ": routineResult is missing, malformed or not correctResult(00)");
}

void PeroduaP02cFlow::transfer(const SRecordSegment& segment,
                              const std::string& label, int begin, int end) {
  std::vector<std::uint8_t> download{0x34, 0x00, 0x44};
  u32(download, segment.address);
  u32(download, static_cast<std::uint32_t>(segment.data.size()));
  const auto block = perodua_max_block_length(exchange(PeroduaEndpoint::ecu,
      download, {0x74}, label + " RequestDownload"));
  const auto capacity = block - 2U;
  std::uint8_t sequence = 1;
  for (std::size_t offset = 0; offset < segment.data.size();) {
    const auto count = std::min(capacity, segment.data.size() - offset);
    std::vector<std::uint8_t> data{0x36, sequence};
    data.insert(data.end(), segment.data.begin() + static_cast<std::ptrdiff_t>(offset),
                segment.data.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const auto reply = exchange(PeroduaEndpoint::ecu, data, {0x76, sequence},
                                label + " TransferData", true);
    if (reply.size() != 2) throw std::runtime_error("unexpected TransferData response length");
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1U);
    progress(begin + static_cast<int>((end - begin) * offset / segment.data.size()), label + " TransferData");
  }
  const auto exit = exchange(PeroduaEndpoint::ecu, {0x37}, {0x77}, label + " TransferExit");
  if (exit.size() != 1) throw std::runtime_error("unexpected TransferExit response length");
  std::vector<std::uint8_t> verify{0x31, 0x01, 0x02, 0x02};
  u32(verify, perodua_crc32(segment.data, crc_reflected_));
  routine(verify, label + " CRC32");
}

void PeroduaP02cFlow::run(const PeroduaImages& images,
                         std::span<const std::uint8_t> fingerprint,
                         std::stop_token stop) {
  stop_ = stop;
  programming_completed_ = false;
  validate_perodua_images(images);
  if (fingerprint.size() != 30) throw std::invalid_argument("F107 requires 30 data bytes");
  check();
  progress(1, "Read ECU identification (F191)");
  const auto identity = exchange(PeroduaEndpoint::ecu, {0x22, 0xF1, 0x91},
                                  {0x62, 0xF1, 0x91}, "ECU identity", true);
  if (identity.size() <= 3) throw std::runtime_error("ECU identification data is empty");
  functional({0x10, 0x83});
  progress(3, "Gateway programming preconditions (0203)");
  const auto preconditions = exchange(PeroduaEndpoint::gateway, {0x31, 0x01, 0x02, 0x03},
                                      {0x71, 0x01, 0x02, 0x03}, "Gateway preconditions");
  // CES012 p31 specifies an empty list; p32 shows four zero bytes.
  const bool empty = preconditions.size() == 4;
  const bool four_zero = preconditions.size() == 8 &&
      std::all_of(preconditions.begin() + 4, preconditions.end(), [](auto v) { return v == 0; });
  if (!empty && !four_zero)
    throw std::runtime_error("Gateway programming preconditions not fulfilled or response layout unknown");
  functional({0x85, 0x82});
  functional({0x28, 0x81, 0x03});
  progress(5, "Enter programming session; AES-CMAC Level 4");
  const auto programming_session = exchange(PeroduaEndpoint::ecu, {0x10, 0x02},
                                             {0x50, 0x02}, "ProgrammingSession");
  if (programming_session.size() != 6)
    throw std::runtime_error("ProgrammingSession response must include P2/P2* timing bytes");
  const auto seed = exchange(PeroduaEndpoint::ecu, {0x27, 0x07}, {0x67, 0x07}, "RequestSeed");
  if (seed.size() != 18) throw std::runtime_error("Perodua security seed must be 16 bytes");
  if (!std::all_of(seed.begin() + 2, seed.end(), [](auto b) { return b == 0; })) {
    const auto key = io_.key(std::span(seed).subspan(2));
    if (key.size() != 16) throw std::runtime_error("Perodua security key must be 16 bytes");
    std::vector<std::uint8_t> request{0x27, 0x08};
    request.insert(request.end(), key.begin(), key.end());
    const auto reply = exchange(PeroduaEndpoint::ecu, request, {0x67, 0x08}, "SendKey");
    if (reply.size() != 2) throw std::runtime_error("unexpected SendKey response length");
  }
  std::vector<std::uint8_t> write{0x2E, 0xF1, 0x07};
  write.insert(write.end(), fingerprint.begin(), fingerprint.end());
  const auto written = exchange(PeroduaEndpoint::ecu, write, {0x6E, 0xF1, 0x07}, "Write fingerprint");
  if (written.size() != 3) throw std::runtime_error("unexpected WriteDataByIdentifier response length");
  progress(10, "Download and verify RAM Flash Driver");
  const auto transfer_block = [&](const PeroduaMemoryBlock& block, int begin, int end, bool erase) {
    std::size_t total{};
    for (const auto& segment : block.image.segments) total += segment.data.size();
    std::size_t done{};
    for (const auto& segment : block.image.segments) {
      const auto next = done + segment.data.size();
      if (erase) {
        // CES012 p26: each further segment returns to EraseMemory.
        // Use this segment's range, never repeat an entire module erase.
        std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00, 0x44};
        u32(request, segment.address);
        u32(request, static_cast<std::uint32_t>(segment.data.size()));
        progress(begin + static_cast<int>((end - begin) * done / total), "Erase " + block.name);
        routine(request, block.name + " EraseMemory");
      }
      transfer(segment, block.name,
          begin + static_cast<int>((end - begin) * done / total),
          begin + static_cast<int>((end - begin) * next / total));
      done = next;
    }
  };
  transfer_block(images.driver, 10, 25, false);
  const int slice = 65 / static_cast<int>(images.modules.size());
  for (std::size_t i = 0; i < images.modules.size(); ++i) {
    const auto& module = images.modules[i];
    transfer_block(module, 25 + static_cast<int>(i) * slice,
                    25 + static_cast<int>(i + 1) * slice, true);
  }
  progress(92, "Check programming dependencies");
  routine({0x31, 0x01, 0xFF, 0x01}, "Programming dependencies");
  const auto reset = exchange(PeroduaEndpoint::ecu, {0x11, 0x01}, {0x51, 0x01}, "Hard reset");
  if (reset.size() != 2) throw std::runtime_error("unexpected HardReset response length");
  programming_completed_ = true;
  io_.wait(2000ms);
  functional({0x10, 0x83});
  functional({0x28, 0x80, 0x03});
  functional({0x85, 0x81});
  functional({0x10, 0x81});
  progress(100, "Perodua CES012 programming and communication restoration completed");
}
} // namespace uds
