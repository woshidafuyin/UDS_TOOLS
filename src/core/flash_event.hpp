#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace uds {

// Stable machine-readable flash semantics. The human-readable step/detail
// strings remain audit evidence and are intentionally not used as identity.
enum class FlashStage {
  unspecified,
  configuration,
  pre_flash_check,
  programming_session,
  security_access,
  driver_request_download,
  driver_transfer,
  driver_transfer_exit,
  driver_verification,
  app_request_download,
  app_transfer,
  app_transfer_exit,
  app_verification,
  dependency_check,
  ecu_reset_recovery,
  post_flash_verification,
  trace_evidence,
  cycle_overview,
};

enum class FlashImageRole {
  none,
  driver,
  app,
  cal,
};

struct FlashEvent {
  std::chrono::system_clock::time_point timestamp{};
  unsigned cycle{};
  FlashStage stage{FlashStage::unspecified};
  std::optional<std::uint8_t> uds_service;
  FlashImageRole image_role{FlashImageRole::none};
  std::string step;
  std::string verdict;
  std::string detail;
};

} // namespace uds
