#pragma once

#include "core/can_bus.hpp"
#include "core/profile.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace uds::app {

struct ProbeRequest {
  FlashProfile profile;
  std::wstring entry_mode{L"app"};
  unsigned channel{};
  std::uint32_t tx_id{};
  std::uint32_t rx_id{};
  unsigned nominal_bitrate{};
  unsigned data_bitrate{};
  std::uint8_t padding{};
  std::filesystem::path trace_file;
};

struct ProbeResult {
  bool success{};
  bool cancelled{};
  std::string message;
};

struct ProbeServiceCallbacks {
  std::function<void(const std::string&)> onLog;
  std::function<void(int, const std::string&)> onProgress;
};

class ProbeService {
public:
  using BusFactory =
      std::function<std::unique_ptr<ICanBus>(const ProbeRequest&)>;

  explicit ProbeService(BusFactory bus_factory = {});

  ProbeResult run(const ProbeRequest& request,
                  const ProbeServiceCallbacks& callbacks,
                  std::stop_token stop) const;

private:
  BusFactory bus_factory_;
};

} // namespace uds::app
