#pragma once

#include "core/asc_trace.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace uds {

struct BusMonitorTraceRecovery {
  std::size_t recovered{};
  std::size_t failed{};
};

// Owns one passive-monitor trace from creation through finalization. The UI
// supplies observed frames and a destination sink; file naming, flushing,
// recovery and ASC document integrity remain encapsulated here.
class BusMonitorTraceSession final {
public:
  using SnapshotSink = std::function<bool(std::string_view)>;

  explicit BusMonitorTraceSession(std::filesystem::path directory);
  ~BusMonitorTraceSession() noexcept;

  BusMonitorTraceSession(const BusMonitorTraceSession&) = delete;
  BusMonitorTraceSession& operator=(const BusMonitorTraceSession&) = delete;

  [[nodiscard]] BusMonitorTraceRecovery recover_incomplete() noexcept;
  [[nodiscard]] bool start(unsigned channel) noexcept;
  void append(const CanFrame& frame) noexcept;
  void flush() noexcept;
  void stop() noexcept;

  // Streams a consistent snapshot without loading a potentially large trace
  // into memory. An active partial file is sealed only in the exported copy.
  [[nodiscard]] bool export_snapshot(const SnapshotSink& sink) noexcept;

  [[nodiscard]] bool is_active() const noexcept;
  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] std::filesystem::path path() const;
  [[nodiscard]] std::string last_error() const;

private:
  [[nodiscard]] std::filesystem::path make_partial_path(unsigned channel) const;
  [[nodiscard]] static std::filesystem::path completed_path(
      const std::filesystem::path& partial);
  [[nodiscard]] static bool has_end_marker(
      const std::filesystem::path& path) noexcept;
  [[nodiscard]] static std::filesystem::path unique_recovery_path(
      const std::filesystem::path& preferred);
  void fail(std::string message) noexcept;

  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  unsigned channel_{1};
  std::chrono::steady_clock::time_point started_at_{};
  std::ofstream stream_;
  std::size_t frame_count_{};
  bool active_{};
  std::string last_error_;
};

} // namespace uds
