#pragma once

#include "core/can_bus.hpp"
#include "core/uds_client.hpp"

#include <array>
#include <cstddef>
#include <chrono>
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

inline constexpr std::array<std::uint8_t, 2> kXizhongDisableDtc{0x85, 0x02};
inline constexpr std::array<std::uint8_t, 3> kXizhongDisableCommunication{0x28, 0x03, 0x01};
inline constexpr std::array<std::uint8_t, 3> kXizhongEnableCommunication{0x28, 0x00, 0x01};
inline constexpr std::array<std::uint8_t, 2> kXizhongEnableDtc{0x85, 0x01};
inline constexpr std::array<std::uint8_t, 2> kXizhongDefaultSession{0x10, 0x01};
inline constexpr std::array<std::uint8_t, 2> kXizhongTesterPresent{0x3E, 0x80};
inline constexpr std::uint8_t kXizhongPhysicalPadding{0xCC};
inline constexpr std::uint8_t kXizhongFunctionalPadding{0x00};
inline constexpr std::chrono::milliseconds kXizhongP2{100};
inline constexpr std::chrono::milliseconds kXizhongTransferDataP2{2000};
inline constexpr std::chrono::milliseconds kXizhongP2Star{5000};
inline constexpr std::chrono::milliseconds kXizhongFlowControlDelay{10};
inline constexpr std::chrono::milliseconds kXizhongTesterPresentPeriod{4000};
inline constexpr std::chrono::milliseconds kXizhongNmPeriod{200};
inline constexpr std::size_t kXizhongNmMaxConsecutiveFailures{5};
inline constexpr std::chrono::milliseconds kXizhongNmWakeupSettle{1000};
inline constexpr std::chrono::milliseconds kXizhongSessionSettle{200};
inline constexpr std::chrono::milliseconds kXizhongRequestDownloadSettle{50};
inline constexpr std::chrono::milliseconds kXizhongRoutineSettle{50};
inline constexpr std::chrono::milliseconds kXizhongAppHashSettle{1000};
inline constexpr std::chrono::milliseconds kXizhongResetSettle{1000};
inline constexpr std::chrono::milliseconds kXizhongFtEndpointSettle{2000};
inline constexpr std::array<std::uint8_t, 32> kXizhongDriverHash{
    0x6A, 0xAB, 0x53, 0x98, 0x12, 0xCF, 0x22, 0xB0,
    0x35, 0x8B, 0x84, 0xE2, 0x09, 0x62, 0x45, 0x89,
    0xD2, 0x1D, 0xDF, 0xD1, 0x93, 0x5F, 0x38, 0xFD,
    0x89, 0x69, 0xE8, 0x2A, 0xFB, 0xB9, 0x50, 0xC6};

std::vector<std::uint8_t> xizhong_rsmr_f184_data(const std::tm& local_time);

struct XizhongRsmrTesterPresentFrame {
  CanFrame frame;
  std::chrono::milliseconds period;
};

std::array<XizhongRsmrTesterPresentFrame, 2>
xizhong_rsmr_tester_present_frames();

std::array<XizhongRsmrTesterPresentFrame, 2>
xizhong_tester_present_frames(std::uint32_t functional_id);

CanFrame xizhong_rsmr_nm_wakeup_frame();

CanFrame xizhong_nm_wakeup_frame(std::uint32_t nm_id);

bool xizhong_supported_flow(std::wstring_view flow) noexcept;

std::optional<CanFrame>
xizhong_nm_wakeup_frame_for_flow(std::wstring_view flow) noexcept;

bool xizhong_rsmr_optional_f189_nrc(const UdsResponse& response) noexcept;

struct XizhongRsmrImages {
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> app_hash;
};

class XizhongRsmrFlow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator =
      std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;
  using HealthCheck = std::function<void()>;

  XizhongRsmrFlow(UdsClient& app_physical, UdsClient& app_functional,
                  UdsClient& ft_physical, Log log, Log progress,
                  KeyGenerator key_generator,
                   HealthCheck health_check = {});
  XizhongRsmrFlow(UdsClient& app_physical, UdsClient& app_functional,
                  UdsClient& ft_physical, Log log, Log progress,
                  KeyGenerator key_generator, std::string target_identity,
                  HealthCheck health_check = {});
  void run(const XizhongRsmrImages& images, std::wstring entry_mode,
           std::stop_token stop = {});

private:
  UdsResponse expect(UdsClient& client, std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> expected, int percent,
                      const std::string& name, bool exact = false,
                      std::chrono::milliseconds p2 = kXizhongP2,
                      std::chrono::milliseconds p2_star = kXizhongP2Star,
                      bool emit_log = true);
  void check_cancelled() const;
  void settle_for(std::chrono::milliseconds duration, int percent,
                  const std::string& name) const;
  void transfer(UdsClient& client, std::uint32_t address,
                std::span<const std::uint8_t> image, int begin_percent,
                int end_percent, const std::string& name);
  void unlock(UdsClient& client);
  static std::vector<std::uint8_t> date_f184();
  static std::vector<std::uint8_t> append_u32(std::vector<std::uint8_t> value,
                                               std::uint32_t number);

  UdsClient& app_physical_;
  UdsClient& app_functional_;
  UdsClient& ft_physical_;
  Log log_;
  Log progress_;
  KeyGenerator key_generator_;
  std::string target_identity_;
  HealthCheck health_check_;
  std::stop_token stop_;
};

} // namespace uds
