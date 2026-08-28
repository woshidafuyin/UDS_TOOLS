#include "flash/lingpao_radar_flow.hpp"

#include "core/high_resolution_timer.hpp"
#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

std::uint8_t bcd(unsigned value) {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

bool starts_with(std::span<const std::uint8_t> value,
                 std::span<const std::uint8_t> prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin());
}

} // namespace

LingpaoRadarEntryMode resolve_lingpao_radar_entry_mode(
    std::wstring_view entry_mode, std::string_view project_name) {
  if (entry_mode == L"app") return LingpaoRadarEntryMode::app_to_app;
  if (entry_mode == L"ft") return LingpaoRadarEntryMode::pls_to_app;
  throw std::invalid_argument(std::string(project_name) +
                              " entry mode must be 'app' (APP-to-APP) or "
                              "'ft' (PLS-to-APP)");
}

std::uint32_t lingpao_radar_crc32(
    std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const auto byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> lingpao_radar_request_download(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> lingpao_radar_erase_memory(
    std::uint32_t address, std::uint32_t length) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32(request, address);
  append_u32(request, length);
  return request;
}

std::vector<std::uint8_t> lingpao_radar_driver_crc_request(
    std::uint32_t crc) {
  std::vector<std::uint8_t> request{0x31, 0x01, 0x02, 0x02};
  append_u32(request, crc);
  return request;
}

std::vector<std::uint8_t> lingpao_radar_programming_date(
    const std::tm& local_time) {
  return {0x20,
          bcd(static_cast<unsigned>((local_time.tm_year + 1900) % 100)),
          bcd(static_cast<unsigned>(local_time.tm_mon + 1)),
          bcd(static_cast<unsigned>(local_time.tm_mday))};
}

std::size_t lingpao_radar_max_block_length(
    std::span<const std::uint8_t> response, std::size_t required_length,
    std::string_view project_name) {
  if (response.size() < 3 || response[0] != 0x74) {
    throw std::runtime_error(std::string(project_name) +
                             " invalid RequestDownload response");
  }
  const auto length_bytes =
      static_cast<std::size_t>((response[1] >> 4U) & 0x0FU);
  if (length_bytes == 0 || response.size() < 2U + length_bytes) {
    throw std::runtime_error(std::string(project_name) +
                             " RequestDownload response has no max block length");
  }
  std::size_t value{};
  for (std::size_t index = 0; index < length_bytes; ++index) {
    value = (value << 8U) | response[2U + index];
  }
  if (value < required_length) {
    throw std::runtime_error(std::string(project_name) +
                             " ECU max block length is smaller than required");
  }
  return required_length;
}

LingpaoRadarFlow::LingpaoRadarFlow(
    UdsClient& physical, UdsClient& app_functional,
    UdsClient& pls_functional, IsoTpSession& physical_transport,
    IsoTpSession& pls_transport, IsoTpSession& functional_transport, Log log,
    KeyGenerator key_generator, LingpaoRadarSpec spec,
    LingpaoRadarTiming timing)
    : physical_(physical), app_functional_(app_functional),
      pls_functional_(pls_functional),
      physical_transport_(physical_transport), pls_transport_(pls_transport),
      functional_transport_(functional_transport), log_(std::move(log)),
      key_generator_(std::move(key_generator)), spec_(std::move(spec)),
      timing_(timing) {
  if (spec_.name.empty() || spec_.app_tx_id == 0 || spec_.app_rx_id == 0 ||
      (spec_.supports_pls_entry &&
       (spec_.pls_tx_id == 0 || spec_.pls_rx_id == 0)) ||
      spec_.functional_id == 0 || spec_.app_length == 0 ||
      spec_.block_length < 3 || spec_.certificate_length == 0 ||
      spec_.security.seed_subfunction == 0 ||
      spec_.security.seed_subfunction >= 0x7F ||
      (spec_.security.seed_subfunction & 1U) == 0 ||
      spec_.security.seed_length == 0 || spec_.security.key_length == 0 ||
      spec_.driver_address.has_value() != spec_.driver_length.has_value()) {
    throw std::invalid_argument("invalid Leapmotor radar flow specification");
  }
}

std::string LingpaoRadarFlow::endpoint(std::uint32_t tx,
                                       std::uint32_t rx) const {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << tx << "/0x" << rx;
  return output.str();
}

UdsResponse LingpaoRadarFlow::expect(
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

void LingpaoRadarFlow::observe_certificate_response(
    std::span<const std::uint8_t> request, int percent,
    const std::string& name) {
  check_cancelled();
  if (log_) {
    log_(percent, name +
                      " (observe response; LP-ARF result is non-gating)");
  }
  const auto observation = physical_.request_observe(
      request, timing_.certificate_response_window,
      timing_.certificate_pending_window, stop_);
  if (!log_) return;

  switch (observation.kind) {
  case UdsObservationKind::positive:
    log_(percent, name + " response consumed; LP-ARF continues: " +
                      to_hex(observation.response));
    return;
  case UdsObservationKind::negative:
    log_(percent, "WARN: " + name + " received " + observation.detail +
                      "; response consumed and ignored by LP-ARF policy; "
                      "continuing");
    return;
  case UdsObservationKind::timeout:
    log_(percent, "WARN: " + name +
                      " received no final response after " +
                      std::to_string(observation.elapsed.count()) +
                      " ms; continuing by LP-ARF policy");
    return;
  }
}

void LingpaoRadarFlow::check_cancelled() const {
  if (periodic_wakeup_failed_.load()) {
    throw std::runtime_error(spec_.name +
                             " periodic wake-up transmission failed");
  }
  if (stop_.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

void LingpaoRadarFlow::wait_for(
    std::chrono::milliseconds duration) const {
  for (auto elapsed = std::chrono::milliseconds{0}; elapsed < duration;
       elapsed += std::chrono::milliseconds{10}) {
    check_cancelled();
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds{10}, duration - elapsed));
  }
}

void LingpaoRadarFlow::enter_from_app() {
  expect(app_functional_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 2,
         "APP->APP 10 01 DefaultSession (" +
             endpoint(spec_.functional_id, spec_.app_rx_id) + ")");
  wait_for(timing_.initial_session_settle);

  constexpr std::array<std::array<std::uint8_t, 3>, 3> dids{{
      {0x22, 0xF1, 0x97},
      {0x22, 0xF1, 0x50},
      {0x22, 0xF1, 0x89},
  }};
  for (const auto& did : dids) {
    expect(physical_, did,
           std::array<std::uint8_t, 3>{0x62, did[1], did[2]}, 4,
           "APP identification DID");
    wait_for(timing_.step_delay);
  }

  expect(app_functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 6,
         "APP->APP 10 03 ExtendedSession");
  expect(app_functional_, std::array<std::uint8_t, 2>{0x85, 0x02},
         std::array<std::uint8_t, 2>{0xC5, 0x02}, 7,
         "APP->APP 85 02 ControlDTCSetting");
  expect(app_functional_,
         std::array<std::uint8_t, 3>{0x28, 0x03, 0x01},
         std::array<std::uint8_t, 2>{0x68, 0x03}, 8,
         "APP->APP 28 03 01 CommunicationControl");
  expect(physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
         std::array<std::uint8_t, 2>{0x50, 0x02}, 10,
         "APP->APP 10 02 ProgrammingSession");
}

void LingpaoRadarFlow::enter_from_pls() {
  if (!spec_.supports_pls_entry) {
    throw std::runtime_error(spec_.name + " does not support PLS entry");
  }
  expect(pls_functional_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 2,
         "PLS->APP 10 01 DefaultSession (" +
             endpoint(spec_.functional_id, spec_.pls_rx_id) + ")");
  wait_for(timing_.initial_session_settle);
  expect(pls_functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 6,
         "PLS->APP 10 03 ExtendedSession");

  const std::array<std::uint8_t, 2> request{0x10, 0x02};
  if (log_) {
    log_(10, "PLS->APP 10 02 ProgrammingSession TX " +
                 endpoint(spec_.pls_tx_id, spec_.pls_rx_id) +
                 "; transition response may move to APP RX");
  }
  pls_transport_.send(request, stop_);
  const auto first =
      pls_transport_.receive(std::chrono::milliseconds(2000), stop_);
  if (starts_with(first, std::array<std::uint8_t, 2>{0x50, 0x02})) {
    if (log_) log_(10, "PLS->APP 10 02 PASS on PLS: " + to_hex(first));
    return;
  }
  if (!starts_with(first,
                   std::array<std::uint8_t, 3>{0x7F, 0x10, 0x78})) {
    throw std::runtime_error("PLS->APP 10 02 unexpected PLS response: " +
                             to_hex(first));
  }
  if (log_) {
    log_(10, std::string("PLS->APP 10 02 received PLS NRC78; waiting for ") +
                 (spec_.pls_programming_final_on_app ? "APP" : "PLS") +
                 " response");
  }
  const auto final = spec_.pls_programming_final_on_app
                         ? physical_transport_.receive(
                               std::chrono::milliseconds(10000), stop_)
                         : pls_transport_.receive(
                               std::chrono::milliseconds(10000), stop_);
  if (!starts_with(final, std::array<std::uint8_t, 2>{0x50, 0x02})) {
    throw std::runtime_error("PLS->APP 10 02 unexpected APP response: " +
                             to_hex(final));
  }
  if (log_) {
    log_(10, std::string("PLS->APP 10 02 PASS on ") +
                 (spec_.pls_programming_final_on_app ? "APP" : "PLS") +
                 ": " + to_hex(final));
  }
}

void LingpaoRadarFlow::transfer_image(const SRecordSegment& image,
                                      int begin_percent, int end_percent,
                                      const std::string& label) {
  if (image.data.empty()) {
    throw std::runtime_error(spec_.name + " " + label + " image is empty");
  }
  const auto response = expect(
      physical_,
      lingpao_radar_request_download(
          image.address, static_cast<std::uint32_t>(image.data.size())),
      std::array<std::uint8_t, 1>{0x74}, begin_percent,
      "34 RequestDownload " + label);
  const auto chunk_size =
      lingpao_radar_max_block_length(response.response, spec_.block_length,
                                     spec_.name) -
      2U;

  std::size_t offset{};
  std::uint8_t sequence{1};
  while (offset < image.data.size()) {
    check_cancelled();
    const auto count = std::min(chunk_size, image.data.size() - offset);
    std::vector<std::uint8_t> transfer{0x36, sequence};
    transfer.insert(
        transfer.end(),
        image.data.begin() + static_cast<std::ptrdiff_t>(offset),
        image.data.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const auto percent =
        begin_percent +
        static_cast<int>((end_percent - begin_percent) *
                         static_cast<double>(offset + count) /
                         static_cast<double>(image.data.size()));
    expect(physical_, transfer,
           std::array<std::uint8_t, 2>{0x76, sequence}, percent,
           "36 TransferData " + label);
    offset += count;
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }
  expect(physical_, std::array<std::uint8_t, 1>{0x37},
         std::array<std::uint8_t, 1>{0x77}, end_percent,
         "37 RequestTransferExit " + label);
}

void LingpaoRadarFlow::unlock() {
  const auto seed_subfunction = spec_.security.seed_subfunction;
  const auto key_subfunction =
      static_cast<std::uint8_t>(seed_subfunction + 1U);
  const std::array<std::uint8_t, 2> seed_request{0x27,
                                                seed_subfunction};
  const std::array<std::uint8_t, 2> seed_prefix{0x67,
                                               seed_subfunction};
  std::ostringstream seed_name;
  seed_name << "27 " << std::uppercase << std::hex << std::setw(2)
            << std::setfill('0') << static_cast<unsigned>(seed_subfunction)
            << " RequestSeed";
  const auto seed =
      expect(physical_, seed_request, seed_prefix, 14, seed_name.str());
  const auto actual_seed_length = seed.response.size() >= 2U
                                      ? seed.response.size() - 2U
                                      : 0U;
  if (actual_seed_length != spec_.security.seed_length) {
    throw std::runtime_error(
        spec_.name + " security seed must be " +
        std::to_string(spec_.security.seed_length) + " bytes (received " +
        std::to_string(actual_seed_length) + ")");
  }
  const auto key = key_generator_(std::span(seed.response).subspan(2),
                                  seed_subfunction);
  if (key.size() != spec_.security.key_length) {
    throw std::runtime_error(
        spec_.name + " security key must be " +
        std::to_string(spec_.security.key_length) + " bytes (generated " +
        std::to_string(key.size()) + ")");
  }
  std::vector<std::uint8_t> request{0x27, key_subfunction};
  request.insert(request.end(), key.begin(), key.end());
  const std::array<std::uint8_t, 2> key_prefix{0x67, key_subfunction};
  std::ostringstream key_name;
  key_name << "27 " << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<unsigned>(key_subfunction)
           << " SendKey";
  expect(physical_, request, key_prefix, 16, key_name.str());
  wait_for(timing_.security_settle);
}

void LingpaoRadarFlow::run_programming_body(
    const LingpaoRadarImages& images) {
  wait_for(timing_.programming_session_settle);
  wait_for(timing_.boot_before);
  if (spec_.send_raw_boot_transition) {
    const auto raw_transition_tx_id =
        spec_.raw_boot_transition_tx_id == 0
            ? spec_.app_tx_id
            : spec_.raw_boot_transition_tx_id;
    physical_transport_.send_raw(
        raw_transition_tx_id,
        std::array<std::uint8_t, 8>{
            0x03, 0xFB, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x00});
    if (log_) {
      std::ostringstream line;
      line << "TX [0x" << std::uppercase << std::hex << raw_transition_tx_id
           << "] 03 FB A5 00 00 00 00 00 (raw boot transition)";
      log_(11, line.str());
    }
  }
  wait_for(timing_.boot_after);
  unlock();

  std::vector<std::uint8_t> certificate{0x31, 0x01, 0x60, 0x00};
  certificate.insert(certificate.end(), images.certificate.begin(),
                     images.certificate.end());
  constexpr std::array<std::uint8_t, 4> certificate_verify{
      0x31, 0x01, 0x60, 0x01};
  if (spec_.certificate_response_policy ==
      CertificateResponsePolicy::require_positive) {
    expect(physical_, certificate,
           std::array<std::uint8_t, 5>{0x71, 0x01, 0x60, 0x00, 0x04}, 18,
           "31 01 60 00 CertificateDownload");
    expect(physical_, certificate_verify,
           std::array<std::uint8_t, 5>{0x71, 0x01, 0x60, 0x01, 0x04}, 20,
           "31 01 60 01 CertificateVerify");
  } else {
    observe_certificate_response(certificate, 18,
                                 "31 01 60 00 CertificateDownload");
    observe_certificate_response(certificate_verify, 20,
                                 "31 01 60 01 CertificateVerify");
  }

  std::vector<std::uint8_t> f198{0x2E, 0xF1, 0x98};
  f198.resize(19, 0x00);
  expect(physical_, f198,
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x98}, 21,
         "2E F198 Fingerprint");

  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  auto f199 = std::vector<std::uint8_t>{0x2E, 0xF1, 0x99};
  const auto date = lingpao_radar_programming_date(local);
  f199.insert(f199.end(), date.begin(), date.end());
  expect(physical_, f199,
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x99}, 22,
         "2E F199 ProgrammingDate");

  int erase_percent = 24;
  if (spec_.driver_address) {
    transfer_image(images.driver, 24, 32, "Driver");
    expect(physical_,
           lingpao_radar_driver_crc_request(
               lingpao_radar_crc32(images.driver.data)),
           std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x02, 0x04}, 34,
           "31 01 02 02 DriverCRC32");
    erase_percent = 36;
  }

  expect(physical_,
         lingpao_radar_erase_memory(
             images.app.address,
             static_cast<std::uint32_t>(images.app.data.size())),
         std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x00, 0x04},
         erase_percent, "31 01 FF 00 EraseAPP");
  transfer_image(images.app, erase_percent + 2, 90, "APP");
  expect(physical_,
         std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03},
         std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x03, 0x04}, 92,
         "31 01 02 03 APPVerify");
  expect(physical_,
         std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
         std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x01, 0x04}, 94,
         "31 01 FF 01 DependencyCheck");
  expect(physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 95,
         "11 01 ECUReset");
  core_programming_completed_ = true;
}

void LingpaoRadarFlow::run_cleanup() {
  wait_for(timing_.post_reset_settle);
  expect(app_functional_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 96,
         "Post-reset 10 03 ExtendedSession");
  expect(app_functional_,
         std::array<std::uint8_t, 3>{0x28, 0x00, 0x01},
         std::array<std::uint8_t, 2>{0x68, 0x00}, 97,
         "Post-reset 28 00 01 CommunicationControl");
  expect(app_functional_, std::array<std::uint8_t, 2>{0x85, 0x01},
         std::array<std::uint8_t, 2>{0xC5, 0x01}, 98,
         "Post-reset 85 01 ControlDTCSetting");
  expect(app_functional_,
         std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
         std::array<std::uint8_t, 1>{0x54}, 99,
         "Post-reset 14 FF FF FF ClearDTC");
  expect(app_functional_, std::array<std::uint8_t, 2>{0x10, 0x01},
         std::array<std::uint8_t, 2>{0x50, 0x01}, 100,
         "Post-reset 10 01 DefaultSession");
}

void LingpaoRadarFlow::run(const LingpaoRadarImages& images,
                           LingpaoRadarEntryMode entry_mode,
                           std::stop_token stop) {
  stop_ = stop;
  core_programming_completed_ = false;
  periodic_wakeup_failed_.store(false);
  check_cancelled();
  if (spec_.driver_address &&
      (images.driver.address != *spec_.driver_address ||
       images.driver.data.size() != *spec_.driver_length)) {
    throw std::runtime_error(spec_.name + " Driver layout mismatch");
  }
  if (images.app.address != spec_.app_address ||
      images.app.data.size() != spec_.app_length) {
    throw std::runtime_error(spec_.name + " APP layout mismatch");
  }
  if (images.certificate.size() != spec_.certificate_length &&
      !(spec_.allow_empty_certificate && images.certificate.empty())) {
    throw std::runtime_error(spec_.name + " certificate length mismatch");
  }

  std::jthread periodic_wakeup;
  if (spec_.periodic_wakeup_id) {
    const std::array<std::uint8_t, 8> frame{};
    physical_transport_.send_raw(*spec_.periodic_wakeup_id, frame);
    if (log_) {
      log_(0, "TX [0x520] 00 00 00 00 00 00 00 00 "
              "(initial periodic wake-up)");
    }
    periodic_wakeup = std::jthread(
        [this, frame](std::stop_token sender_stop) {
          ScopedHighResolutionTimer timer_resolution;
          auto next = std::chrono::steady_clock::now() +
                      spec_.periodic_wakeup_period;
          while (!sender_stop.stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next) {
              try {
                physical_transport_.send_raw(*spec_.periodic_wakeup_id, frame);
              } catch (...) {
                periodic_wakeup_failed_.store(true);
                return;
              }
              do {
                next += spec_.periodic_wakeup_period;
              } while (next <= now);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
    if (log_) {
      log_(0, spec_.name +
                  " periodic 0x520 wake-up active (" +
                  std::to_string(spec_.periodic_wakeup_period.count()) +
                  " ms)");
    }
  }

  std::jthread tester_present([this](std::stop_token sender_stop) {
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    const std::array<std::uint8_t, 8> frame{
        0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    while (!sender_stop.stop_requested()) {
      if (std::chrono::steady_clock::now() >= next) {
        try {
          functional_transport_.send_raw(spec_.functional_id, frame);
          if (log_) log_(0, "TX [0x7DF] 02 3E 80 00 00 00 00 00 (raw TesterPresent)");
        } catch (...) {
          if (log_) log_(0, "WARN: " + spec_.name + " TesterPresent sender failed");
        }
        next += std::chrono::seconds(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  wait_for(timing_.startup_settle);
  if (entry_mode == LingpaoRadarEntryMode::app_to_app) {
    enter_from_app();
  } else {
    if (!spec_.supports_pls_entry) {
      throw std::runtime_error(spec_.name + " does not support PLS entry");
    }
    enter_from_pls();
  }
  run_programming_body(images);
  run_cleanup();
}

bool LingpaoRadarFlow::core_programming_completed() const noexcept {
  return core_programming_completed_;
}

} // namespace uds
