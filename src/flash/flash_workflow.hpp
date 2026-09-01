#pragma once

#include "core/flash_event.hpp"
#include "core/profile.hpp"
#include "drivers/can/can_bus_provider.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

struct FlashJob {
  FlashProfile profile;
  std::wstring entry_mode{L"app"};
  std::filesystem::path executable_directory;
  std::filesystem::path driver_file;
  std::filesystem::path app_file;
  std::filesystem::path cal_file;
  std::filesystem::path driver_verify_file;
  std::filesystem::path app_verify_file;
  std::filesystem::path cal_verify_file;
  std::filesystem::path security_dll;
  std::shared_ptr<ICanBusProvider> can_bus_provider{
      default_can_bus_provider()};
};

struct FlashWorkflowCallbacks {
  std::function<void(const std::string&)> log;
  std::function<void(int, const std::string&)> progress;
  std::function<void(std::string, std::string, std::string)> report;
  // Optional structured report channel. Existing workflows and integrations
  // may keep using report; migrated workflows prefer event when it is set.
  std::function<void(FlashEvent)> event;
};

class FlashWorkflow {
public:
  virtual ~FlashWorkflow() = default;
  virtual std::wstring_view id() const noexcept = 0;
  virtual std::string report_title(const FlashProfile& profile) const = 0;
  virtual void run(const FlashJob& job, const FlashWorkflowCallbacks& callbacks,
                   std::stop_token stop) = 0;
};

std::unique_ptr<FlashWorkflow> create_flash_workflow(std::wstring_view flow_id);
bool is_flash_workflow_registered(std::wstring_view flow_id) noexcept;
std::vector<std::wstring> registered_flash_workflows();

} // namespace uds
