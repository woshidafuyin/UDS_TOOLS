#pragma once

#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "flash/chuneng_331_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

struct Chuneng331Images {
  std::uint32_t driver_address{};
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> driver_verification;
  std::vector<std::uint8_t> app_verification;
};

struct Chuneng331EntryPlan {
  bool use_ft_endpoint{};
  bool run_standard_preprogramming{true};
  bool boot_only_to_app{};
};

inline constexpr std::size_t kChuneng331BlockLength = 0x802;
inline constexpr std::size_t kChuneng331TransferDataLength =
    kChuneng331BlockLength - 2U;

Chuneng331EntryPlan resolve_chuneng_331_entry_plan(std::wstring_view entry_mode);
std::vector<std::uint8_t> chuneng_331_fingerprint_f184(
    const std::tm& local_time);
std::size_t chuneng_331_transfer_block_length(
    std::span<const std::uint8_t> request_download_response);

class Chuneng331Flow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator = std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;

  Chuneng331Flow(UdsClient& physical, UdsClient& functional, IsoTpSession& physical_transport,
                 IsoTpSession& functional_transport,
                 Log log, KeyGenerator key_generator);
  Chuneng331Flow(UdsClient& physical, UdsClient& functional, UdsClient& ft_physical,
                 IsoTpSession& physical_transport, IsoTpSession& functional_transport,
                 Log log, KeyGenerator key_generator);
  void run(const Chuneng331Images& images, std::stop_token stop = {});
  void run(const Chuneng331Images& images, std::wstring_view entry_mode,
           std::stop_token stop = {});

private:
  UdsResponse expect(UdsClient& client, std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> prefix, int percent, const std::string& name);
  UdsResponse expect_routine(UdsClient& client,
                             std::span<const std::uint8_t> request,
                             std::uint16_t routine_id, int percent,
                             const std::string& name);
  void send_functional_no_response(
      std::span<const std::uint8_t> request,
      std::chrono::milliseconds delay, int percent,
      std::string_view name);
  void enter_programming_session(const Chuneng331EntryPlan& entry);
  void restore_after_reset();
  void transfer_image(std::uint32_t address, std::span<const std::uint8_t> image,
                      int begin_percent, int end_percent, const std::string& label);
  static std::vector<std::uint8_t> fingerprint_f184();
  void check_cancelled() const;

  UdsClient& physical_;
  UdsClient& functional_;
  UdsClient* ft_physical_{};
  IsoTpSession& physical_transport_;
  IsoTpSession& functional_transport_;
  Log log_;
  KeyGenerator key_generator_;
  std::stop_token stop_;
};

} // namespace uds
