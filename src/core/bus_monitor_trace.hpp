#pragma once

#include "core/can_bus.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

struct BusMonitorTraceRecovery {
  std::size_t recovered{};
  std::size_t failed{};
};

// Owns one passive-monitor BLF trace from creation through finalization. The UI
// supplies observed frames and a destination sink; BLF serialization, file
// naming, flushing and interrupted-session recovery remain encapsulated here.
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

  // Streams a consistent, finalized BLF snapshot without loading a potentially
  // large trace into memory. Active buffered frames are flushed first.
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
  void append_blf_object(const CanFrame& frame);
  void flush_blf_container();
  void write_blf_header();
  void fail(std::string message) noexcept;

  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  unsigned channel_{1};
  std::chrono::steady_clock::time_point started_at_{};
  std::fstream stream_;
  std::vector<std::uint8_t> blf_buffer_;
  std::chrono::system_clock::time_point started_wall_at_{};
  std::chrono::system_clock::time_point stopped_wall_at_{};
  std::uint64_t uncompressed_size_{144};
  std::uint32_t object_count_{};
  std::size_t frame_count_{};
  bool active_{};
  std::string last_error_;
};

} // namespace uds
