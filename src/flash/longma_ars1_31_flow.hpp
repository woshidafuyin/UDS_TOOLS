#pragma once

#include "core/uds_client.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

struct LongmaArs131Layout {
  std::uint32_t driver_start{0x08000000};
  std::uint32_t driver_length{0x400};
  std::uint32_t app_start{0xC0080000};
  std::uint32_t app_length{0xF5000};
  std::uint32_t cal_start{0xC0180000};
  std::uint32_t cal_length{0x270};
};

struct LongmaArs131Images {
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> cal;
};

// The UI exposes one operation token, while the flow keeps ECU entry and
// downloaded content as separate decisions. This prevents CAL modes from
// leaking into endpoint selection, probing, or unrelated workflows.
struct LongmaArs131DownloadPlan {
  bool ft_entry{};
  bool download_app{};
  bool download_cal{};
  std::string_view display_name;
};

LongmaArs131DownloadPlan resolve_longma_ars131_download_plan(
    std::wstring_view operation_mode);

inline constexpr std::uint16_t kLongmaArs131ReferenceDriverCrc = 0xCC3C;
inline constexpr std::uint16_t kLongmaArs131ReferenceAppCrc = 0xCB08;
inline constexpr std::uint32_t kLongmaArs131MainTxId = 0x744;
inline constexpr std::uint32_t kLongmaArs131MainRxId = 0x74C;
inline constexpr std::uint32_t kLongmaArs131SecondaryTxId = 0x760;
inline constexpr std::uint32_t kLongmaArs131SecondaryRxId = 0x768;

constexpr bool longma_ars131_endpoint_supported(std::uint32_t tx_id,
                                                std::uint32_t rx_id) {
  return (tx_id == kLongmaArs131MainTxId &&
          rx_id == kLongmaArs131MainRxId) ||
         (tx_id == kLongmaArs131SecondaryTxId &&
          rx_id == kLongmaArs131SecondaryRxId);
}

constexpr bool longma_ars131_secondary_endpoint(std::uint32_t tx_id,
                                                std::uint32_t rx_id) {
  return tx_id == kLongmaArs131SecondaryTxId &&
         rx_id == kLongmaArs131SecondaryRxId;
}

std::vector<std::uint8_t> longma_ars131_request_download(std::uint32_t address,
                                                         std::uint32_t length);
std::vector<std::uint8_t> longma_ars131_erase_memory(std::uint32_t address,
                                                     std::uint32_t length);
std::size_t longma_ars131_max_block_length(
    std::span<const std::uint8_t> response);
std::uint16_t longma_ars131_crc16_ccitt_false(
    std::span<const std::uint8_t> data);

class LongmaArs131Flow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator =
      std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;
  using HealthCheck = std::function<void()>;

  LongmaArs131Flow(UdsClient& physical, UdsClient& functional,
                   LongmaArs131Layout layout, Log log,
                   KeyGenerator key_generator,
                   HealthCheck health_check = {},
                   UdsClient* ft_physical = nullptr);
  void run(const LongmaArs131Images& images,
           std::wstring_view entry_mode = L"app",
           std::stop_token stop = {});

private:
  UdsResponse expect(
      UdsClient& client, std::span<const std::uint8_t> request,
      std::span<const std::uint8_t> expected, int percent,
      const std::string& name, bool exact_response = false,
      std::chrono::milliseconds p2 = std::chrono::milliseconds(2000),
      std::chrono::milliseconds p2_star = std::chrono::milliseconds(5000));
  void send_suppressed(std::span<const std::uint8_t> request, int percent,
                       const std::string& name);
  void unlock_security(int begin_percent, const std::string& label);
  void transfer_image(std::uint32_t address,
                      std::span<const std::uint8_t> image,
                      int begin_percent, int end_percent,
                      const std::string& label);
  void wait_cancellable(std::chrono::milliseconds duration) const;
  void check_cancelled() const;
  static std::vector<std::uint8_t> fingerprint_f184();

  UdsClient& physical_;
  UdsClient& functional_;
  UdsClient* ft_physical_{};
  LongmaArs131Layout layout_;
  Log log_;
  KeyGenerator key_generator_;
  HealthCheck health_check_;
  std::stop_token stop_;
};

} // namespace uds
