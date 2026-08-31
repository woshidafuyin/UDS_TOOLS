#pragma once

#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "core/vbf.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

inline constexpr std::uint32_t kGeelyP416AppTxId{0x716};
inline constexpr std::uint32_t kGeelyP416AppRxId{0x616};
inline constexpr std::uint32_t kGeelyP416SblTxId{0x716};
inline constexpr std::uint32_t kGeelyP416SblRxId{0x616};
inline constexpr std::uint32_t kGeelyP416AppFunctionalId{0x7FF};
inline constexpr std::uint32_t kGeelyP416PlsTxId{0x701};
inline constexpr std::uint32_t kGeelyP416PlsRxId{0x761};
inline constexpr std::uint32_t kGeelyP416PlsFunctionalId{0x7DF};
inline constexpr std::uint32_t kGeelyP416NmId{0x53F};
inline constexpr std::size_t kGeelyP416TransferBlockLength{0x800};

CanFrame geely_p416_nm_wakeup_frame();

enum class GeelyP416EntryMode {
  app_to_app,
  pls_to_app,
};

struct GeelyP416Images {
  VbfFile sbl;
  VbfFile ess;
  VbfFile app;
};

struct GeelyP416Timing {
  std::chrono::milliseconds transition_settle{3150};
  std::chrono::milliseconds post_reset_settle{4150};
  std::chrono::milliseconds wake_period{200};
};

GeelyP416EntryMode resolve_geely_p416_entry_mode(std::wstring_view entry_mode);
std::uint8_t geely_p416_family_ess_data_format_identifier(
    std::wstring_view profile_id, std::uint8_t vbf_value);
std::array<std::uint8_t, 3> geely_p416_seed_key(
    std::span<const std::uint8_t> seed);
std::vector<std::uint8_t> geely_p416_request_download(
    std::uint8_t data_format_identifier, std::uint32_t address,
    std::uint32_t length);
std::vector<std::uint8_t> geely_p416_erase_memory(std::uint32_t address,
                                                  std::uint32_t length);
std::vector<std::uint8_t> geely_p416_verify_signature(
    std::span<const std::uint8_t> signature);
std::vector<std::uint8_t> geely_p416_call(std::uint32_t address);
std::size_t geely_p416_transfer_chunk_size(
    std::span<const std::uint8_t> request_download_response);
class GeelyP416Flow {
public:
  using Log = std::function<void(int, const std::string&)>;

  GeelyP416Flow(UdsClient& app_physical, UdsClient& sbl_transition_physical,
                UdsClient& programming_physical, UdsClient& app_functional,
                UdsClient& pls_physical, UdsClient& pls_functional,
                IsoTpSession& raw_transport,
                IsoTpSession& sbl_transition_transport, Log log,
                GeelyP416Timing timing = {},
                std::uint32_t app_tx_id = kGeelyP416AppTxId,
                std::uint32_t app_rx_id = kGeelyP416AppRxId,
                std::uint32_t programming_tx_id = kGeelyP416SblTxId,
                std::uint32_t programming_rx_id = 0x617);

  void run(const GeelyP416Images& images, GeelyP416EntryMode entry_mode,
           std::stop_token stop = {});
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
  void unlock();
  void transfer_file(UdsClient& client, const VbfFile& file, int begin_percent,
                     int end_percent, const std::string& label);
  void verify_file(UdsClient& client, const VbfFile& file, int percent,
                   const std::string& label);
  void erase_file(UdsClient& client, const VbfFile& file, int begin_percent,
                  const std::string& label);
  void program(const GeelyP416Images& images);

  UdsClient& app_physical_;
  UdsClient& sbl_transition_physical_;
  UdsClient& programming_physical_;
  UdsClient& app_functional_;
  UdsClient& pls_physical_;
  UdsClient& pls_functional_;
  IsoTpSession& raw_transport_;
  IsoTpSession& sbl_transition_transport_;
  Log log_;
  GeelyP416Timing timing_;
  std::uint32_t app_tx_id_{};
  std::uint32_t app_rx_id_{};
  std::uint32_t programming_tx_id_{};
  std::uint32_t programming_rx_id_{};
  std::stop_token stop_;
  std::atomic_bool wake_failed_{};
  bool core_programming_completed_{};
};

} // namespace uds
