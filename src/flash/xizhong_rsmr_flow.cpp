#include "flash/xizhong_rsmr_flow.hpp"

#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {
using namespace std::chrono_literals;

void append_u32_to(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}
} // namespace

std::vector<std::uint8_t> xizhong_rsmr_f184_data(const std::tm& local_time) {
  const auto year = local_time.tm_year + 1900;
  const auto bcd = [](int value) -> std::uint8_t {
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
  };
  std::vector<std::uint8_t> data(16, 0x00);
  data.push_back(bcd(year / 100));
  data.push_back(bcd(year % 100));
  data.push_back(bcd(local_time.tm_mon + 1));
  data.push_back(bcd(local_time.tm_mday));
  return data;
}

std::array<XizhongRsmrTesterPresentFrame, 2>
xizhong_rsmr_tester_present_frames() {
  return xizhong_tester_present_frames(0x18DBFFF1);
}

std::array<XizhongRsmrTesterPresentFrame, 2>
xizhong_tester_present_frames(std::uint32_t functional_id) {
  const std::vector<std::uint8_t> data{
      0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
  return {{
      {CanFrame{functional_id, data, true, false, false},
       kXizhongTesterPresentPeriod},
      {CanFrame{functional_id, data, true, true, true},
       kXizhongTesterPresentPeriod},
  }};
}

CanFrame xizhong_rsmr_nm_wakeup_frame() {
  return xizhong_nm_wakeup_frame(0x18FFA025);
}

CanFrame xizhong_nm_wakeup_frame(std::uint32_t nm_id) {
  return CanFrame{nm_id,
                  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                  true, false, false};
}

bool xizhong_supported_flow(std::wstring_view flow) noexcept {
  return flow == L"xizhong_rsmr" || flow == L"xizhong_lsmr";
}

std::optional<CanFrame>
xizhong_nm_wakeup_frame_for_flow(std::wstring_view flow) noexcept {
  if (flow == L"xizhong_rsmr") return xizhong_rsmr_nm_wakeup_frame();
  if (flow == L"xizhong_lsmr") return xizhong_nm_wakeup_frame(0x18FFA0B6);
  return std::nullopt;
}

bool xizhong_rsmr_optional_f189_nrc(const UdsResponse& response) noexcept {
  constexpr std::array<std::uint8_t, 3> kRequestOutOfRange{0x7F, 0x22, 0x31};
  return !response.success && response.nrc == 0x31 &&
         std::equal(kRequestOutOfRange.begin(), kRequestOutOfRange.end(),
                    response.response.begin(), response.response.end());
}

XizhongRsmrFlow::XizhongRsmrFlow(
    UdsClient& app_physical, UdsClient& app_functional,
    UdsClient& ft_physical, Log log, Log progress,
    KeyGenerator key_generator, HealthCheck health_check)
    : XizhongRsmrFlow(app_physical, app_functional, ft_physical,
                      std::move(log), std::move(progress),
                      std::move(key_generator), "RSMR_AA",
                      std::move(health_check)) {}

XizhongRsmrFlow::XizhongRsmrFlow(
    UdsClient& app_physical, UdsClient& app_functional,
    UdsClient& ft_physical, Log log, Log progress,
    KeyGenerator key_generator,
    std::string target_identity,
    HealthCheck health_check)
    : app_physical_(app_physical), app_functional_(app_functional),
       ft_physical_(ft_physical), log_(std::move(log)),
       progress_(std::move(progress)),
       key_generator_(std::move(key_generator)),
       target_identity_(std::move(target_identity)),
       health_check_(std::move(health_check)) {}

void XizhongRsmrFlow::check_cancelled() const {
  if (stop_.stop_requested()) throw std::runtime_error("operation cancelled by user");
  if (health_check_) health_check_();
}

void XizhongRsmrFlow::settle_for(std::chrono::milliseconds duration,
                                 int percent,
                                 const std::string& name) const {
  if (log_) {
    log_(percent, name + ": fixed settle " +
                      std::to_string(duration.count()) + " ms");
  }
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    check_cancelled();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(remaining, 10ms));
  }
  check_cancelled();
}

UdsResponse XizhongRsmrFlow::expect(
    UdsClient& client, std::span<const std::uint8_t> request,
    std::span<const std::uint8_t> expected, int percent,
    const std::string& name, bool exact,
    std::chrono::milliseconds p2, std::chrono::milliseconds p2_star,
    bool emit_log) {
  check_cancelled();
  if (log_ && emit_log) log_(percent, name);
  UdsResponse result;
  try {
    result = client.request(request, p2, p2_star);
  } catch (const std::exception& error) {
    throw std::runtime_error(name + ": " + error.what());
  }
  check_cancelled();
  if (!result.success) throw std::runtime_error(name + ": NRC/timeout " + to_hex(result.response));
  const bool prefix = result.response.size() >= expected.size() &&
                      std::equal(expected.begin(), expected.end(), result.response.begin());
  if (!prefix || (exact && result.response.size() != expected.size())) {
    throw std::runtime_error(name + ": response mismatch, got " + to_hex(result.response));
  }
  if (log_ && emit_log) {
    log_(percent, name + " PASS: " + to_hex(result.response));
  }
  return result;
}

std::vector<std::uint8_t> XizhongRsmrFlow::append_u32(
    std::vector<std::uint8_t> value, std::uint32_t number) {
  append_u32_to(value, number);
  return value;
}

std::vector<std::uint8_t> XizhongRsmrFlow::date_f184() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  return xizhong_rsmr_f184_data(local);
}

void XizhongRsmrFlow::unlock(UdsClient& client) {
  const auto seed = expect(client, std::array<std::uint8_t, 2>{0x27, 0x11},
                           std::array<std::uint8_t, 2>{0x67, 0x11}, 22,
                           "27 11 RequestSeed");
  if (seed.response.size() != 6) {
    throw std::runtime_error("犀重 Seed length must be 4 bytes");
  }
  const auto key = key_generator_(std::span(seed.response).subspan(2, 4));
  if (key.size() != 4) throw std::runtime_error("犀重 Key length must be 4 bytes");
  std::vector<std::uint8_t> request{0x27, 0x12};
  request.insert(request.end(), key.begin(), key.end());
  expect(client, request, std::array<std::uint8_t, 2>{0x67, 0x12}, 24,
         "27 12 SendKey", true);
}

void XizhongRsmrFlow::transfer(
    UdsClient& client, std::uint32_t address, std::span<const std::uint8_t> image,
    int begin_percent, int end_percent, const std::string& name) {
  std::vector<std::uint8_t> request{0x34, 0x00, 0x44};
  append_u32_to(request, address);
  append_u32_to(request, static_cast<std::uint32_t>(image.size()));
  const auto response = expect(client, request, std::array<std::uint8_t, 1>{0x74},
                               begin_percent, "34 " + name);
  if (response.response.size() < 4) throw std::runtime_error("34 response is too short");
  const auto length_bytes = static_cast<std::size_t>((response.response[1] >> 4U) & 0x0FU);
  if (length_bytes == 0 || response.response.size() < 2 + length_bytes) {
    throw std::runtime_error("34 response has no block length");
  }
  std::size_t max_block = 0;
  for (std::size_t i = 0; i < length_bytes; ++i) max_block = (max_block << 8U) | response.response[2 + i];
  if (max_block <= 2 || max_block > 4095) throw std::runtime_error("invalid 34 max block length");
  const auto chunk_size = max_block - 2;
  const auto block_count =
      (image.size() + chunk_size - 1U) / chunk_size;
  settle_for(kXizhongRequestDownloadSettle, begin_percent,
              "34 " + name + " -> first 36");
  std::size_t offset = 0;
  std::size_t block_index = 0;
  std::uint8_t sequence = 1;
  int last_reported_percent = begin_percent;
  while (offset < image.size()) {
    check_cancelled();
    const auto count = std::min(chunk_size, image.size() - offset);
    std::vector<std::uint8_t> block{0x36, sequence};
    block.insert(block.end(), image.begin() + static_cast<std::ptrdiff_t>(offset),
                 image.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const auto percent = begin_percent + static_cast<int>((end_percent - begin_percent) *
                          static_cast<double>(offset + count) / static_cast<double>(image.size()));
    ++block_index;
    expect(client, block, std::array<std::uint8_t, 2>{0x76, sequence}, percent,
           "36 " + name + " block " + std::to_string(block_index) + "/" +
               std::to_string(block_count),
           true, kXizhongTransferDataP2, kXizhongP2Star, false);
    offset += count;
    const auto progress_line =
        "36 " + name + " progress: " + std::to_string(block_index) + "/" +
        std::to_string(block_count) + " blocks, " + std::to_string(offset) +
        "/" + std::to_string(image.size()) + " bytes";
    // Update the live status after every acknowledged block.  Persisted logs
    // remain percent-throttled below so UI navigation and report size stay
    // manageable.
    if (progress_) progress_(percent, progress_line);
    if (log_ && (percent > last_reported_percent || offset == image.size())) {
      log_(percent, progress_line);
      last_reported_percent = percent;
    }
    sequence = static_cast<std::uint8_t>(sequence + 1U);
  }
  if (log_) {
    log_(end_percent, "TransferData (0x36) " + name + " PASS: blocks=" +
                          std::to_string(block_count) + ", bytes=" +
                          std::to_string(image.size()) +
                          ", max-data-per-block=" +
                          std::to_string(chunk_size));
  }
  expect(client, std::array<std::uint8_t, 1>{0x37}, std::array<std::uint8_t, 1>{0x77},
         end_percent, "37 " + name, true);
}

void XizhongRsmrFlow::run(const XizhongRsmrImages& images,
                          std::wstring entry_mode, std::stop_token stop) {
  stop_ = stop;
  if (images.driver.empty() || images.app.empty() || images.app_hash.size() != 32) {
    throw std::runtime_error("犀重 Driver、APP或Hash文件内容无效");
  }
  if (entry_mode != L"app" && entry_mode != L"ft" && entry_mode != L"auto") {
    throw std::runtime_error("犀重入口模式无效，只允许 APP、FT 或 auto");
  }

  bool use_ft = entry_mode == L"ft";
  if (entry_mode == L"auto") {
    try {
      const std::array<std::uint8_t, 2> request{0x10, 0x03};
      const auto response = app_physical_.request(request, 700ms, 1500ms);
      use_ft = !response.success;
      if (!use_ft && log_) log_(0, "自动探测：29位APP端点响应50 03，选择APP入口");
    } catch (...) {
      use_ft = true;
    }
    if (use_ft && log_) log_(0, "自动探测：APP端点无响应，尝试0x701/0x761 FT入口");
  }

  if (use_ft) {
    if (log_) log_(2, "FT 10 03：按CAPL raw分支发送后固定等待200 ms");
    ft_physical_.send_only(std::array<std::uint8_t, 2>{0x10, 0x03});
    settle_for(kXizhongSessionSettle, 2, "FT 10 03 -> 10 02");
    if (log_) log_(5, "FT 10 02：请求ECU切换至29位RSMR编程端点");
    ft_physical_.send_only(std::array<std::uint8_t, 2>{0x10, 0x02});
    // 2026-07-22 Passed BLF没有走FT；这里仅静态复现Flash.can分支：
    // 不要求0x761正响应，发送后固定等待2秒再切换到29位端点。
    settle_for(kXizhongFtEndpointSettle, 5,
               "FT 10 02 -> 29-bit endpoint");
    if (log_) log_(6, "FT入口切换等待完成，后续使用29位RSMR端点");
  } else {
    expect(app_physical_, std::array<std::uint8_t, 3>{0x22, 0xF1, 0x87},
           std::array<std::uint8_t, 3>{0x62, 0xF1, 0x87}, 1, "22 F187");
    const auto f150 = expect(
        app_physical_, std::array<std::uint8_t, 3>{0x22, 0xF1, 0x50},
        std::array<std::uint8_t, 3>{0x62, 0xF1, 0x50}, 1, "22 F150");
    const std::vector<std::uint8_t> target_identity(
        target_identity_.begin(), target_identity_.end());
    if (std::search(f150.response.begin(), f150.response.end(),
                    target_identity.begin(), target_identity.end()) == f150.response.end()) {
      throw std::runtime_error("22 F150 identity mismatch: expected " + target_identity_ + " target");
    }
    check_cancelled();
    if (log_) log_(1, "22 F189");
    const auto f189 = app_physical_.request(
        std::array<std::uint8_t, 3>{0x22, 0xF1, 0x89}, kXizhongP2,
        kXizhongP2Star);
    check_cancelled();
    if (xizhong_rsmr_optional_f189_nrc(f189)) {
      if (log_) {
        log_(1, "22 F189 WARN: ECU returned 7F 22 31; F150 already confirmed " +
                    target_identity_ +
                    ", continue like CAPL's non-aborting DID read");
      }
    } else if (!f189.success) {
      throw std::runtime_error("22 F189: NRC/timeout " +
                               to_hex(f189.response));
    } else {
      constexpr std::array<std::uint8_t, 3> kF189Positive{0x62, 0xF1, 0x89};
      const auto positive = f189.response.size() >= kF189Positive.size() &&
                            std::equal(kF189Positive.begin(),
                                       kF189Positive.end(),
                                       f189.response.begin());
      if (!positive ||
          std::search(f189.response.begin(), f189.response.end(),
                      target_identity.begin(), target_identity.end()) == f189.response.end()) {
        throw std::runtime_error(
            "22 F189 identity mismatch: expected " + target_identity_ + " target");
      }
      if (log_) log_(1, "22 F189 PASS: " + to_hex(f189.response));
    }
    expect(app_physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
           std::array<std::uint8_t, 2>{0x50, 0x03}, 5, "APP 10 03");
    expect(app_functional_, kXizhongDisableDtc,
           std::array<std::uint8_t, 2>{0xC5, 0x02}, 6, "FUN 85 02", true);
    expect(app_functional_, kXizhongDisableCommunication,
           std::array<std::uint8_t, 2>{0x68, 0x03}, 7, "FUN 28 03 01", true);
    expect(app_physical_, std::array<std::uint8_t, 2>{0x10, 0x02},
           std::array<std::uint8_t, 2>{0x50, 0x02}, 8, "APP 10 02");
    settle_for(kXizhongSessionSettle, 8, "APP 10 02 -> SecurityAccess");
  }

  unlock(app_physical_);
  expect(app_physical_, [&] { auto v = std::vector<std::uint8_t>{0x2E, 0xF1, 0x84};
                              const auto date = date_f184(); v.insert(v.end(), date.begin(), date.end()); return v; }(),
         std::array<std::uint8_t, 3>{0x6E, 0xF1, 0x84}, 28, "2E F184", true);
  transfer(app_physical_, 0x00080000, images.driver, 32, 43, "Driver");
  std::vector<std::uint8_t> driver_check{0x31, 0x01, 0x02, 0x02};
  driver_check.insert(driver_check.end(), kXizhongDriverHash.begin(),
                      kXizhongDriverHash.end());
  expect(app_physical_, driver_check,
         std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x02, 0x04}, 46,
         "31 0202 Driver", true);
  settle_for(kXizhongRoutineSettle, 46, "Driver verify -> APP erase");
  std::vector<std::uint8_t> erase{0x31, 0x01, 0xFF, 0x00, 0x44};
  append_u32_to(erase, 0x000C0000); append_u32_to(erase, 0x00300000);
  expect(app_physical_, erase,
         std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x00, 0x04}, 50,
         "31 FF00 APP", true);
  settle_for(kXizhongRoutineSettle, 50, "APP erase -> APP 34");
  transfer(app_physical_, 0x000C0000, images.app, 52, 88, "APP");
  settle_for(kXizhongAppHashSettle, 88, "APP 37 -> APP verify");
  std::vector<std::uint8_t> app_check{0x31, 0x01, 0x02, 0x02};
  app_check.insert(app_check.end(), images.app_hash.begin(), images.app_hash.end());
  expect(app_physical_, app_check,
         std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x02, 0x04}, 92,
         "31 0202 APP", true);
  settle_for(kXizhongRoutineSettle, 92, "APP verify -> dependency check");
  expect(app_physical_, std::array<std::uint8_t, 4>{0x31, 0x01, 0xFF, 0x01},
         std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x01, 0x04}, 95,
         "31 FF01", true);
  settle_for(kXizhongRoutineSettle, 95, "Dependency check -> ECU reset");
  expect(app_physical_, std::array<std::uint8_t, 2>{0x11, 0x01},
         std::array<std::uint8_t, 2>{0x51, 0x01}, 97, "11 01");
  settle_for(kXizhongResetSettle, 97, "ECU reset -> extended session");
  expect(app_physical_, std::array<std::uint8_t, 2>{0x10, 0x03},
         std::array<std::uint8_t, 2>{0x50, 0x03}, 98, "APP 10 03 after reset");
  expect(app_functional_, kXizhongEnableCommunication,
         std::array<std::uint8_t, 2>{0x68, 0x00}, 99,
         "FUN 28 00 01", true);
  expect(app_functional_, kXizhongEnableDtc,
         std::array<std::uint8_t, 2>{0xC5, 0x01}, 99,
         "FUN 85 01", true);
  expect(app_functional_, kXizhongDefaultSession,
         std::array<std::uint8_t, 2>{0x50, 0x01}, 100,
         "FUN 10 01");
  if (log_) log_(100, "犀重 " + target_identity_ + " 刷写流程完成");
}

} // namespace uds
