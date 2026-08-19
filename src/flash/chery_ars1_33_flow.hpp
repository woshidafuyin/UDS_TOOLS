#pragma once

#include "core/can_bus.hpp"
#include "core/uds_client.hpp"

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

struct CheryArs133Layout {
  std::uint32_t driver0_start{0x00499000};
  std::uint32_t driver0_length{0x10};
  std::uint32_t driver_start{0x0049C038};
  std::uint32_t driver_length{0x1EB8};
  std::uint32_t app_start{0x000C1000};
  std::uint32_t app_length{0x7B000};
  std::uint32_t cal_start{0x000B0000};
  std::uint32_t cal_length{0x200};
};

struct CheryArs133Images {
  std::vector<std::uint8_t> driver0;
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> cal;
  std::vector<std::uint8_t> driver_verification;
  std::vector<std::uint8_t> app_verification;
  std::vector<std::uint8_t> cal_verification;
};

struct CheryArs133PreconditionFrame {
  CanFrame frame;
  std::chrono::milliseconds period;
};

enum class CheryArs133FlashMode {
  AppCal,
  AppOnly,
  CalOnly,
};

struct CheryArs133DownloadPlan {
  CheryArs133FlashMode mode;
  bool download_app{};
  bool download_cal{};
  bool periodic_tester_present{};
};

// CAPL TxMsgSrever() waits 50 ms while confirming that a suppressed-response
// request stayed silent and then waits another 50 ms before the next service.
inline constexpr std::chrono::milliseconds
    kCheryArs133SuppressedSessionSettle{100};
inline constexpr std::chrono::milliseconds kCheryArs133WakeupSettle{1000};

std::array<CheryArs133PreconditionFrame, 3>
chery_ars133_precondition_frames();
CanFrame chery_ars133_app_tester_present_frame(std::uint32_t functional_id);
CheryArs133DownloadPlan resolve_chery_ars133_download_plan(
    std::wstring_view entry_mode);

std::vector<std::uint8_t> chery_ars133_request_download(std::uint32_t address,
                                                        std::uint32_t length);
std::vector<std::uint8_t> chery_ars133_erase_memory(std::uint32_t address,
                                                    std::uint32_t length);
std::size_t chery_ars133_max_block_length(std::span<const std::uint8_t> response);

class CheryArs133Flow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator = std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;
  using HealthCheck = std::function<void()>;

  CheryArs133Flow(UdsClient& physical, UdsClient& functional,
                  CheryArs133Layout layout, Log log,
                  KeyGenerator key_generator, HealthCheck health_check = {});
  void run(const CheryArs133Images& images, CheryArs133FlashMode mode,
           std::stop_token stop = {});

private:
  UdsResponse expect(UdsClient& client, std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> prefix, int percent,
                     const std::string& name,
                     std::chrono::milliseconds p2 = std::chrono::milliseconds(2000),
                     std::chrono::milliseconds p2_star = std::chrono::milliseconds(5000));
  UdsResponse expect_routine(std::span<const std::uint8_t> request,
                             std::span<const std::uint8_t> prefix, int percent,
                             const std::string& name);
  void transfer_image(std::uint32_t address, std::span<const std::uint8_t> image,
                      int begin_percent, int end_percent, const std::string& label);
  void unlock_security(int begin_percent);
  void write_fingerprint(int percent);
  void transfer_driver(const CheryArs133Images& images, int begin_percent,
                       int end_percent);
  void verify_driver(const CheryArs133Images& images, int percent);
  void verify_app(const CheryArs133Images& images, int percent);
  void verify_cal(const CheryArs133Images& images, int percent);
  void check_programming_dependencies(int percent);
  void hard_reset(int percent, std::chrono::milliseconds post_reset_wait);
  void clear_dtc(int percent);
  void run_app_cal(const CheryArs133Images& images);
  void run_app_only(const CheryArs133Images& images);
  void run_cal_only(const CheryArs133Images& images);
  void wait_cancellable(std::chrono::milliseconds duration) const;
  void check_cancelled() const;
  static std::vector<std::uint8_t> fingerprint_f184();

  UdsClient& physical_;
  UdsClient& functional_;
  CheryArs133Layout layout_;
  Log log_;
  KeyGenerator key_generator_;
  HealthCheck health_check_;
  std::stop_token stop_;
};

} // namespace uds
