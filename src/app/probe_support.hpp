#pragma once

#include "app/probe_service.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace uds::app::probe_detail {

inline constexpr std::string_view kCancelled =
    "operation cancelled by user";

inline std::string utf8(std::wstring_view text) {
  const auto encoded = std::filesystem::path(std::wstring(text)).u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

inline void log(const ProbeServiceCallbacks& callbacks,
                const std::string& line) {
  if (callbacks.onLog) callbacks.onLog(line);
}

inline void progress(const ProbeServiceCallbacks& callbacks, int value,
                     const std::string& line) {
  if (callbacks.onProgress) callbacks.onProgress(value, line);
}

inline void check_stop(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error(kCancelled.data());
}

std::string concise_probe_failure(std::string_view detail);

} // namespace uds::app::probe_detail
