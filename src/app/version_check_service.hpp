#pragma once

#include "core/can_bus.hpp"
#include "core/profile.hpp"
#include "core/version_check_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace uds::app {

enum class VersionCheckStatus {
  pass,
  fail,
  error,
  warning,
  info,
};

struct VersionCheckRequest {
  FlashProfile profile;
  std::filesystem::path profile_path;
  std::wstring target_id;
  unsigned channel{};
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  std::filesystem::path trace_file;
};

struct VersionCheckItemResult {
  VersionCheckStatus status{VersionCheckStatus::error};
  std::wstring name;
  std::wstring expected;
  std::wstring actual;
  std::string request_hex;
  std::string response_hex;
  std::string detail;
  unsigned elapsed_ms{};
  bool required{};
};

struct VersionCheckResult {
  bool success{};
  bool cancelled{};
  std::string message;
  std::vector<VersionCheckItemResult> items;
};

struct VersionCheckCallbacks {
  std::function<void(const std::string&)> onLog;
  std::function<void(int, const std::string&)> onProgress;
};

class VersionCheckService {
public:
  using BusFactory =
      std::function<std::unique_ptr<ICanBus>(const VersionCheckRequest&)>;

  explicit VersionCheckService(BusFactory bus_factory = {});

  [[nodiscard]] VersionCheckResult run(
      const VersionCheckRequest& request,
      const VersionCheckCallbacks& callbacks, std::stop_token stop) const;

private:
  BusFactory bus_factory_;
};

} // namespace uds::app
