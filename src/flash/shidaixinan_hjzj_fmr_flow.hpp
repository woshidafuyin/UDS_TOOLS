#pragma once

#include "core/can_bus.hpp"
#include "core/flash_data.hpp"
#include "core/uds_client.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace uds {

inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjWakeupPeriod{10};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjWakeupSettle{1000};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjTesterPresentInitialDelay{600};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjTesterPresentPeriod{3000};
inline constexpr std::chrono::milliseconds kShidaixinanHjzjP2{500};
inline constexpr std::chrono::milliseconds kShidaixinanHjzjP2Star{5000};
inline constexpr std::chrono::milliseconds kShidaixinanHjzjStepDelay{100};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjFtStepDelay{500};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjFtEntryWindow{4000};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjFtRetryDelay{100};
inline constexpr std::chrono::milliseconds
    kShidaixinanHjzjFtPostResetReadyWindow{3000};
inline constexpr std::size_t kShidaixinanHjzjMaximumWakeFailures{5};
inline constexpr std::uint32_t kShidaixinanReferenceDriverCrc32{
    0x47456D8F};
inline constexpr std::uint32_t kShidaixinanReferenceAppCrc32{
    0x4AF3F59F};
inline constexpr std::uint32_t kShidaixinanHjzjFtFunctionalTxId{
    0x7DF};
inline constexpr std::uint32_t kShidaixinanHjzjFtFunctionalRxId{
    0x761};

CanFrame shidaixinan_hjzj_wakeup_frame();
CanFrame shidaixinan_hjzj_tester_present_frame();

std::uint32_t shidaixinan_hjzj_crc32(
    std::span<const std::uint8_t> data) noexcept;
std::vector<std::uint8_t> shidaixinan_hjzj_request_download(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> shidaixinan_hjzj_erase_memory(
    std::uint32_t address, std::uint32_t length);
std::size_t shidaixinan_hjzj_max_block_length(
    std::span<const std::uint8_t> response);

struct ShidaixinanHjzjFmrImages {
  SRecordSegment driver;
  SRecordSegment app;
};

enum class ShidaixinanHjzjFmrEntryMode {
  app,
  ft,
};

struct ShidaixinanHjzjFmrTiming {
  std::chrono::milliseconds p2{kShidaixinanHjzjP2};
  std::chrono::milliseconds p2_star{kShidaixinanHjzjP2Star};
  std::chrono::milliseconds app_step_delay{
      kShidaixinanHjzjStepDelay};
  std::chrono::milliseconds ft_step_delay{
      kShidaixinanHjzjFtStepDelay};
  std::chrono::milliseconds ft_entry_window{
      kShidaixinanHjzjFtEntryWindow};
  std::chrono::milliseconds ft_retry_delay{
      kShidaixinanHjzjFtRetryDelay};
  std::chrono::milliseconds ft_post_reset_ready_window{
      kShidaixinanHjzjFtPostResetReadyWindow};
};

class ShidaixinanHjzjFmrFlow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator = std::function<std::vector<std::uint8_t>(
      std::span<const std::uint8_t>, unsigned)>;
  using HealthCheck = std::function<void()>;

  ShidaixinanHjzjFmrFlow(
      UdsClient& physical, UdsClient& functional,
      UdsClient& ft_functional, Log log, Log progress,
      KeyGenerator key_generator, HealthCheck health_check = {},
      ShidaixinanHjzjFmrTiming timing = {});

  void run(const ShidaixinanHjzjFmrImages& images,
           ShidaixinanHjzjFmrEntryMode entry_mode,
           std::stop_token stop = {});
  [[nodiscard]] bool core_programming_completed() const noexcept;

private:
  UdsResponse expect(UdsClient& client,
                     std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> expected, int percent,
                     const std::string& name, bool exact = false,
                     bool settle_after = true,
                     bool emit_log = true);
  void check_cancelled() const;
  void settle_for(std::chrono::milliseconds duration, int percent,
                  const std::string& name) const;
  void unlock(std::uint8_t seed_subfunction,
              std::uint8_t key_subfunction, unsigned algorithm_level,
              int percent);
  void transfer(const SRecordSegment& image, int begin_percent,
                int end_percent, const std::string& name);
  void run_app_preamble();
  void run_ft_preamble();
  void run_programming_body(
      const ShidaixinanHjzjFmrImages& images);
  void run_ft_cleanup();
  void wait_for_ft_physical_programming_session();
  void wait_for_ft_post_reset_session();
  [[nodiscard]] std::chrono::milliseconds step_delay() const noexcept;

  UdsClient& physical_;
  UdsClient& functional_;
  UdsClient& ft_functional_;
  Log log_;
  Log progress_;
  KeyGenerator key_generator_;
  HealthCheck health_check_;
  ShidaixinanHjzjFmrTiming timing_;
  ShidaixinanHjzjFmrEntryMode entry_mode_{
      ShidaixinanHjzjFmrEntryMode::app};
  bool core_programming_completed_{};
  std::stop_token stop_;
};

} // namespace uds
