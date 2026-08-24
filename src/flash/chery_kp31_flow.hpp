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

struct CheryKp31Layout {
  std::uint32_t driver_start{0x08000000};
  std::uint32_t driver_length{0x400};
  std::uint32_t app_start{0xC0080000};
  std::uint32_t app_length{0xF5000};
  std::uint32_t cal_start{0xC0180000};
  std::uint32_t cal_length{0xC8};
};

struct CheryKp31Images {
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> cal;
  std::vector<std::uint8_t> driver_verification;
  std::vector<std::uint8_t> app_verification;
  std::vector<std::uint8_t> cal_verification;
};

enum class CheryKp31FlashMode {
  AppOnly,
  CalOnly,
  AppCal,
};

struct CheryKp31DownloadPlan {
  CheryKp31FlashMode mode;
  bool download_app{};
  bool download_cal{};
};

CheryKp31DownloadPlan resolve_chery_kp31_download_plan(
    std::wstring_view entry_mode);

std::vector<std::uint8_t> chery_kp31_request_download(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> chery_kp31_erase_memory(
    std::uint32_t address, std::uint32_t length);
std::size_t chery_kp31_max_block_length(
    std::span<const std::uint8_t> response);

class CheryKp31Flow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator =
      std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;

  CheryKp31Flow(UdsClient& physical, UdsClient& functional,
                CheryKp31Layout layout, Log log,
                KeyGenerator key_generator);

  void run(const CheryKp31Images& images, CheryKp31FlashMode mode,
           std::stop_token stop = {});

private:
  UdsResponse expect(
      UdsClient& client, std::span<const std::uint8_t> request,
      std::span<const std::uint8_t> expected_prefix, int percent,
      const std::string& name,
      std::chrono::milliseconds p2 = std::chrono::milliseconds(2000),
      std::chrono::milliseconds p2_star = std::chrono::milliseconds(5000));
  UdsResponse expect_routine(std::span<const std::uint8_t> request,
                             std::span<const std::uint8_t> expected_prefix,
                             int percent, const std::string& name);
  void send_functional_suppressed(std::span<const std::uint8_t> request,
                                  int percent, const std::string& name);
  void unlock_security(int percent);
  void write_fingerprint(int percent);
  void verify_rsa(std::uint16_t routine_id,
                  std::span<const std::uint8_t> verification,
                  int percent, const std::string& label);
  void check_dependencies(int percent);
  void hard_reset_and_clear_dtc(int percent, bool restore_default_session);
  void run_app_only(const CheryKp31Images& images);
  void run_app_cal(const CheryKp31Images& images);
  void run_cal_only(const CheryKp31Images& images);
  void transfer_image(std::uint32_t address,
                      std::span<const std::uint8_t> image,
                      int begin_percent, int end_percent,
                      const std::string& label,
                      bool send_transfer_exit,
                      std::size_t forced_data_size = 0);
  void wait_cancellable(std::chrono::milliseconds duration) const;
  void check_cancelled() const;
  static std::vector<std::uint8_t> fingerprint_f184();

  UdsClient& physical_;
  UdsClient& functional_;
  CheryKp31Layout layout_;
  Log log_;
  KeyGenerator key_generator_;
  std::stop_token stop_;
};

} // namespace uds
