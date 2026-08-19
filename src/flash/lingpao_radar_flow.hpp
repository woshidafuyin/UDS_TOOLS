#pragma once

#include "core/flash_data.hpp"
#include "core/isotp.hpp"
#include "core/uds_client.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

enum class LingpaoRadarEntryMode {
  app_to_app,
  pls_to_app,
};

struct RadarSeedKeyKnownAnswer {
  std::vector<std::uint8_t> seed;
  std::vector<std::uint8_t> key;
};

struct RadarSecurityAccessSpec {
  std::uint8_t seed_subfunction{0x11};
  std::size_t seed_length{4};
  std::size_t key_length{4};
  std::vector<RadarSeedKeyKnownAnswer> known_answers;
  std::string self_test_description;
};

struct LingpaoRadarSpec {
  std::string name;
  std::uint32_t app_tx_id{};
  std::uint32_t app_rx_id{};
  std::uint32_t pls_tx_id{0x701};
  std::uint32_t pls_rx_id{0x761};
  std::uint32_t functional_id{0x7DF};
  std::uint32_t app_address{0x000C0000};
  std::uint32_t app_length{0x00180000};
  std::optional<std::uint32_t> driver_address;
  std::optional<std::uint32_t> driver_length;
  std::size_t block_length{0x802};
  std::size_t certificate_length{1322};
  bool pls_programming_final_on_app{true};
  bool send_raw_boot_transition{true};
  bool supports_pls_entry{true};
  std::uint32_t raw_boot_transition_tx_id{};
  std::optional<std::uint32_t> periodic_wakeup_id;
  std::chrono::milliseconds periodic_wakeup_period{500};
  RadarSecurityAccessSpec security;
};

struct LingpaoRadarImages {
  SRecordSegment driver;
  SRecordSegment app;
  std::vector<std::uint8_t> certificate;
};

struct LingpaoRadarTiming {
  std::chrono::milliseconds startup_settle{1000};
  std::chrono::milliseconds initial_session_settle{2000};
  std::chrono::milliseconds step_delay{100};
  std::chrono::milliseconds programming_session_settle{2000};
  std::chrono::milliseconds boot_before{10};
  std::chrono::milliseconds boot_after{30};
  std::chrono::milliseconds security_settle{500};
  std::chrono::milliseconds post_reset_settle{6000};
};

LingpaoRadarEntryMode resolve_lingpao_radar_entry_mode(
    std::wstring_view entry_mode, std::string_view project_name);
std::uint32_t lingpao_radar_crc32(
    std::span<const std::uint8_t> data) noexcept;
std::vector<std::uint8_t> lingpao_radar_request_download(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> lingpao_radar_erase_memory(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> lingpao_radar_driver_crc_request(
    std::uint32_t crc);
std::vector<std::uint8_t> lingpao_radar_programming_date(
    const std::tm& local_time);
std::size_t lingpao_radar_max_block_length(
    std::span<const std::uint8_t> response, std::size_t required_length,
    std::string_view project_name);

class LingpaoRadarFlow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator = std::function<std::vector<std::uint8_t>(
      std::span<const std::uint8_t>, unsigned)>;

  LingpaoRadarFlow(
      UdsClient& physical, UdsClient& app_functional,
      UdsClient& pls_functional, IsoTpSession& physical_transport,
      IsoTpSession& pls_transport, IsoTpSession& functional_transport,
      Log log, KeyGenerator key_generator, LingpaoRadarSpec spec,
      LingpaoRadarTiming timing = {});

  void run(const LingpaoRadarImages& images,
           LingpaoRadarEntryMode entry_mode, std::stop_token stop = {});
  [[nodiscard]] bool core_programming_completed() const noexcept;

private:
  UdsResponse expect(UdsClient& client,
                     std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> prefix, int percent,
                     const std::string& name);
  void check_cancelled() const;
  void wait_for(std::chrono::milliseconds duration) const;
  void enter_from_app();
  void enter_from_pls();
  void transfer_image(const SRecordSegment& image, int begin_percent,
                      int end_percent, const std::string& label);
  void unlock();
  void run_programming_body(const LingpaoRadarImages& images);
  void run_cleanup();
  [[nodiscard]] std::string endpoint(std::uint32_t tx,
                                     std::uint32_t rx) const;

  UdsClient& physical_;
  UdsClient& app_functional_;
  UdsClient& pls_functional_;
  IsoTpSession& physical_transport_;
  IsoTpSession& pls_transport_;
  IsoTpSession& functional_transport_;
  Log log_;
  KeyGenerator key_generator_;
  LingpaoRadarSpec spec_;
  LingpaoRadarTiming timing_;
  std::stop_token stop_;
  bool core_programming_completed_{};
  std::atomic_bool periodic_wakeup_failed_{};
};

} // namespace uds
