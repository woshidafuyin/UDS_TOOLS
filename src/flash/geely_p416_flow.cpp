#include "flash/geely_p416_flow.hpp"

#include "core/hex.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uds {
namespace {

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

std::string endpoint(std::uint32_t tx_id, std::uint32_t rx_id) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << tx_id << "/0x" << rx_id;
  return output.str();
}

} // namespace

CanFrame geely_p416_nm_wakeup_frame() {
  return {kGeelyP416NmId,
          {0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
          false, false, false};
}

GeelyP416EntryMode resolve_geely_p416_entry_mode(
    std::wstring_view entry_mode) {
  if (entry_mode == L"app") return GeelyP416EntryMode::app_to_app;
  if (entry_mode == L"ft") return GeelyP416EntryMode::pls_to_app;
  throw std::invalid_argument(
      "Geely P416 entry mode must be 'app' (APP-to-APP) or 'ft' (PLS-to-APP)");
}

std::uint8_t geely_p416_family_ess_data_format_identifier(
    std::wstring_view profile_id, std::uint8_t vbf_value) {
  (void)profile_id;
  return vbf_value;
}

std::array<std::uint8_t, 3> geely_p416_seed_key(
    std::span<const std::uint8_t> seed) {
  if (seed.size() != 3U) {
    throw std::invalid_argument("Geely P416 security seed must be 3 bytes");
  }
  std::array<std::uint8_t, 8> input{};
  std::copy(seed.begin(), seed.end(), input.begin());
  std::fill(input.begin() + 3, input.end(), std::uint8_t{0xFF});

  std::uint32_t state{0xC541A9U};
  constexpr std::uint32_t polynomial =
      (1U << 23U) | (1U << 20U) | (1U << 15U) | (1U << 12U) |
      (1U << 5U) | (1U << 3U);
  for (const auto byte : input) {
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const auto feedback =
          (state & 1U) ^ ((static_cast<std::uint32_t>(byte) >> bit) & 1U);
      state = (state >> 1U) & 0x7FFFFFU;
      if (feedback != 0U) state ^= polynomial;
    }
  }
  return {
      static_cast<std::uint8_t>((state >> 4U) & 0xFFU),
      static_cast<std::uint8_t>(((state >> 8U) & 0xF0U) |
                                ((state >> 20U) & 0x0FU)),
      static_cast<std::uint8_t>(((state << 4U) & 0xF0U) |
                                ((state >> 16U) & 0x0FU)),
  };
}

std::vector<std::uint8_t> geely_p416_request_download(
    std::uint8_t data_format_identifier, std::uint32_t address,
    std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, data_format_identifier, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> geely_p416_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> geely_p416_verify_signature(
    std::span<const std::uint8_t> signature) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0x02, 0x12};
  request.insert(request.end(), signature.begin(), signature.end());
  return request;
}

std::vector<std::uint8_t> geely_p416_call(std::uint32_t address) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0x03, 0x01};
  append_u32(request, address);
  return request;
}

std::size_t geely_p416_transfer_chunk_size(
    std::span<const std::uint8_t> response) {
  if (response.size() < 3U || response[0] != 0x74U) {
    throw std::runtime_error("Geely P416 invalid RequestDownload response");
  }
  const auto length_bytes = static_cast<std::size_t>(response[1] >> 4U);
  if (length_bytes == 0U || response.size() < 2U + length_bytes) {
    throw std::runtime_error(
        "Geely P416 RequestDownload response has no max block length");
  }
  std::size_t max_block{};
  for (std::size_t index = 0; index < length_bytes; ++index) {
    max_block = (max_block << 8U) | response[2U + index];
  }
  if (max_block < 3U) {
    throw std::runtime_error("Geely P416 ECU max block length is invalid");
  }
  return std::min(max_block, kGeelyP416TransferBlockLength) - 2U;
}

GeelyP416Flow::GeelyP416Flow(
    UdsClient& app_physical, UdsClient& sbl_transition_physical,
    UdsClient& programming_physical, UdsClient& app_functional,
    UdsClient& pls_physical, UdsClient& pls_functional,
    IsoTpSession& raw_transport, IsoTpSession& sbl_transition_transport,
    Log log, GeelyP416Timing timing, std::uint32_t app_tx_id,
    std::uint32_t app_rx_id, std::uint32_t programming_tx_id,
    std::uint32_t programming_rx_id)
    : app_physical_(app_physical),
      sbl_transition_physical_(sbl_transition_physical),
      programming_physical_(programming_physical),
      app_functional_(app_functional),
      pls_physical_(pls_physical), pls_functional_(pls_functional),
      raw_transport_(raw_transport),
      sbl_transition_transport_(sbl_transition_transport),
      log_(std::move(log)), timing_(timing), app_tx_id_(app_tx_id),
      app_rx_id_(app_rx_id), programming_tx_id_(programming_tx_id),
      programming_rx_id_(programming_rx_id) {}

UdsResponse GeelyP416Flow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> prefix, int percent,
    const std::string& name) {
  check_cancelled();
  if (log_) log_(percent, name);
  auto result = client.request(request, std::chrono::milliseconds(2000),
                               std::chrono::milliseconds(10000), stop_);
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

void GeelyP416Flow::check_cancelled() const {
  if (wake_failed_.load()) {
    throw std::runtime_error(
        "Geely P416 periodic 0x53F wake transmission failed");
  }
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

void GeelyP416Flow::wait_for(std::chrono::milliseconds duration) const {
  for (auto elapsed = std::chrono::milliseconds{0}; elapsed < duration;
       elapsed += std::chrono::milliseconds{10}) {
    check_cancelled();
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds{10}, duration - elapsed));
  }
}

void GeelyP416Flow::enter_from_app() {
  expect(app_functional_, std::array<std::uint8_t, 2>{0x3E, 0x00},
         std::array<std::uint8_t, 2>{0x7E, 0x00}, 2,
         "APP->APP 3E 00 TesterPresent (" +
             endpoint(kGeelyP416AppFunctionalId, app_rx_id_) + ")");
  expect(app_physical_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 4,
         "APP->APP 10 01 DefaultSession (" +
             endpoint(app_tx_id_, app_rx_id_) + ")");
  expect(app_physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 6,
         "APP->APP 10 02 ProgrammingSession (" +
             endpoint(app_tx_id_, app_rx_id_) + ")");
  wait_for(timing_.transition_settle);
  expect(app_functional_, std::array<std::uint8_t, 2>{0x3E, 0x00},
         std::array<std::uint8_t, 2>{0x7E, 0x00}, 8,
         "APP->APP post-transition 3E 00 (" +
             endpoint(kGeelyP416AppFunctionalId, app_rx_id_) + ")");
}

void GeelyP416Flow::enter_from_pls() {
  expect(pls_functional_, std::array<std::uint8_t, 2>{0x3E, 0x00},
         std::array<std::uint8_t, 2>{0x7E, 0x00}, 2,
         "PLS->APP 3E 00 TesterPresent (0x7DF/0x761)");
  expect(pls_physical_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 3,
         "PLS->APP 10 01 DefaultSession (0x701/0x761)");
  expect(pls_physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 5,
         "PLS->APP 10 03 ExtendedSession (0x701/0x761)");
  expect(pls_physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 6,
         "PLS->APP 10 02 ProgrammingSession (0x701/0x761)");
  wait_for(timing_.transition_settle);
  expect(app_functional_, std::array<std::uint8_t, 2>{0x3E, 0x00},
         std::array<std::uint8_t, 2>{0x7E, 0x00}, 8,
         "PLS->APP post-transition 3E 00 (" +
             endpoint(kGeelyP416AppFunctionalId, app_rx_id_) + ")");
}

void GeelyP416Flow::unlock() {
  const auto seed =
      expect(app_physical_, std::array<std::uint8_t, 2>{0x27, 0x01},
             std::array<std::uint8_t, 2>{0x67, 0x01}, 10,
             "27 01 RequestSeed");
  if (seed.response.size() != 5U) {
    throw std::runtime_error("Geely P416 security seed response must be 5 bytes");
  }
  const auto key = geely_p416_seed_key(std::span(seed.response).subspan(2U));
  std::vector<std::uint8_t> request{0x27, 0x02};
  request.insert(request.end(), key.begin(), key.end());
  expect(app_physical_, request,
         std::array<std::uint8_t, 2>{0x67, 0x02}, 12,
         "27 02 SendKey");
}

void GeelyP416Flow::transfer_file(UdsClient& client, const VbfFile& file,
                                  int begin_percent,
                                  int end_percent,
                                  const std::string& label) {
  std::size_t total{};
  for (const auto& block : file.blocks) total += block.data.size();
  std::size_t transferred{};
  for (std::size_t block_index = 0; block_index < file.blocks.size();
       ++block_index) {
    const auto& block = file.blocks[block_index];
    const auto download = geely_p416_request_download(
        file.data_format_identifier, block.address,
        static_cast<std::uint32_t>(block.data.size()));
    const auto response = expect(
        client, download, std::array<std::uint8_t, 1>{0x74},
        begin_percent, "34 RequestDownload " + label + " block " +
                           std::to_string(block_index + 1U));
    const auto chunk_size = geely_p416_transfer_chunk_size(response.response);
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
      const auto completed = transferred + offset + count;
      const auto percent =
          begin_percent + static_cast<int>(
                              (end_percent - begin_percent) *
                              static_cast<double>(completed) /
                              static_cast<double>(total));
      expect(client, transfer,
             std::array<std::uint8_t, 2>{0x76, sequence}, percent,
             "36 TransferData " + label);
      offset += count;
      sequence = static_cast<std::uint8_t>(sequence + 1U);
    }
    expect(client, std::array<std::uint8_t, 1>{0x37},
           std::array<std::uint8_t, 1>{0x77}, end_percent,
           "37 RequestTransferExit " + label);
    transferred += block.data.size();
  }
}

void GeelyP416Flow::verify_file(UdsClient& client, const VbfFile& file,
                                int percent,
                                const std::string& label) {
  expect(client, geely_p416_verify_signature(file.signature),
         std::array<std::uint8_t, 6>{0x71, 0x01, 0x02, 0x12, 0x10, 0x00},
         percent, "31 01 02 12 Verify " + label + " signature");
}

void GeelyP416Flow::erase_file(UdsClient& client, const VbfFile& file,
                               int begin_percent,
                               const std::string& label) {
  for (std::size_t index = 0; index < file.erase_ranges.size(); ++index) {
    const auto& range = file.erase_ranges[index];
    expect(client, geely_p416_erase_memory(range.address, range.length),
           std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x00, 0x10},
           begin_percent + static_cast<int>(index),
           "31 01 FF 00 Erase " + label + " range " +
               std::to_string(index + 1U));
  }
}

void GeelyP416Flow::program(const GeelyP416Images& images) {
  transfer_file(app_physical_, images.sbl, 14, 34, "SBL");
  verify_file(app_physical_, images.sbl, 36, "SBL");
  expect(sbl_transition_physical_, geely_p416_call(images.sbl.call_address),
         std::array<std::uint8_t, 5>{0x71, 0x01, 0x03, 0x01, 0x10}, 38,
         "31 01 03 01 Start SBL");

  auto& active_physical =
      sbl_transition_transport_.last_rx_id() == app_rx_id_
          ? app_physical_
          : programming_physical_;
  if (log_) {
    log_(38, "SBL runtime endpoint selected from final response: " +
                 (sbl_transition_transport_.last_rx_id() == app_rx_id_
                      ? endpoint(app_tx_id_, app_rx_id_)
                      : endpoint(programming_tx_id_, programming_rx_id_)));
  }

  erase_file(active_physical, images.ess, 40, "ESS");
  transfer_file(active_physical, images.ess, 44, 50, "ESS");
  verify_file(active_physical, images.ess, 52, "ESS");

  erase_file(active_physical, images.app, 54, "APP");
  transfer_file(active_physical, images.app, 58, 92, "APP");
  verify_file(active_physical, images.app, 94, "APP");
  expect(active_physical,
         std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x05},
         std::array<std::uint8_t, 6>{0x71, 0x01, 0x02, 0x05, 0x10, 0x00}, 96,
         "31 01 02 05 DependencyCheck");
  expect(active_physical, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 97, "11 01 ECUReset");
  core_programming_completed_ = true;
  wait_for(timing_.post_reset_settle);
  expect(app_physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 100,
         "Post-reset 10 03 ExtendedSession");
}

void GeelyP416Flow::run(const GeelyP416Images& images,
                        GeelyP416EntryMode entry_mode,
                        std::stop_token stop) {
  stop_ = stop;
  wake_failed_.store(false);
  core_programming_completed_ = false;
  const auto wake = geely_p416_nm_wakeup_frame();
  try {
    raw_transport_.send_raw(wake.id, wake.data);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("Geely P416 initial 0x53F wake transmission failed: ") +
        error.what());
  } catch (...) {
    throw std::runtime_error(
        "Geely P416 initial 0x53F wake transmission failed");
  }

  std::jthread wake_sender([this, wake](std::stop_token sender_stop) {
    auto next = std::chrono::steady_clock::now() +
                std::max(timing_.wake_period, std::chrono::milliseconds{1});
    while (!sender_stop.stop_requested() && !stop_.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next) {
        try {
          raw_transport_.send_raw(wake.id, wake.data);
        } catch (...) {
          wake_failed_.store(true);
          if (log_) log_(0, "ERROR: periodic 0x53F wake transmission failed");
          return;
        }
        do {
          next += std::max(timing_.wake_period,
                           std::chrono::milliseconds{1});
        } while (next <= now);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  check_cancelled();
  if (entry_mode == GeelyP416EntryMode::app_to_app) {
    enter_from_app();
  } else {
    enter_from_pls();
  }
  unlock();
  program(images);
}

bool GeelyP416Flow::core_programming_completed() const noexcept {
  return core_programming_completed_;
}

} // namespace uds
