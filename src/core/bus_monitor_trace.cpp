#include "core/bus_monitor_trace.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace uds {
namespace {

constexpr std::string_view kEndMarker{"End TriggerBlock\r\n"};
constexpr std::string_view kPartialSuffix{".asc.partial"};

bool has_suffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.ends_with(suffix);
}

std::string timestamp_with_milliseconds() {
  const auto now = std::chrono::system_clock::now();
  const auto value = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &value);
#else
  localtime_r(&value, &local);
#endif
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()) %
                            std::chrono::seconds(1);
  std::ostringstream output;
  output << std::put_time(&local, "%Y%m%d_%H%M%S_") << std::setw(3)
         << std::setfill('0') << milliseconds.count();
  return output.str();
}

} // namespace

BusMonitorTraceSession::BusMonitorTraceSession(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {}

BusMonitorTraceSession::~BusMonitorTraceSession() noexcept { stop(); }

std::filesystem::path BusMonitorTraceSession::make_partial_path(
    unsigned channel) const {
  auto path = directory_ /
              ("bus_monitor_" + timestamp_with_milliseconds() + "_CH" +
               std::to_string(std::max(channel, 1U)) + ".asc.partial");
  unsigned suffix = 2;
  while (std::filesystem::exists(path)) {
    path = directory_ /
           ("bus_monitor_" + timestamp_with_milliseconds() + "_CH" +
            std::to_string(std::max(channel, 1U)) + "_" +
            std::to_string(suffix++) + ".asc.partial");
  }
  return path;
}

std::filesystem::path BusMonitorTraceSession::completed_path(
    const std::filesystem::path& partial) {
  auto result = partial;
  result.replace_extension();
  return result;
}

bool BusMonitorTraceSession::has_end_marker(
    const std::filesystem::path& path) noexcept {
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto position = input.tellg();
    const auto size = static_cast<std::streamoff>(position);
    if (size <= 0) return false;
    constexpr std::streamoff kTailSize = 256;
    input.seekg(size > kTailSize ? size - kTailSize : 0);
    const std::string tail((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    return tail.find(kEndMarker) != std::string::npos;
  } catch (...) {
    return false;
  }
}

std::filesystem::path BusMonitorTraceSession::unique_recovery_path(
    const std::filesystem::path& preferred) {
  if (!std::filesystem::exists(preferred)) return preferred;
  const auto parent = preferred.parent_path();
  const auto stem = preferred.stem().string();
  const auto extension = preferred.extension().string();
  unsigned suffix = 2;
  auto candidate = parent / (stem + "_recovered_" +
                             std::to_string(suffix++) + extension);
  while (std::filesystem::exists(candidate)) {
    candidate = parent / (stem + "_recovered_" +
                          std::to_string(suffix++) + extension);
  }
  return candidate;
}

BusMonitorTraceRecovery BusMonitorTraceSession::recover_incomplete() noexcept {
  std::scoped_lock lock(mutex_);
  BusMonitorTraceRecovery result;
  try {
    std::filesystem::create_directories(directory_);
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_regular_file()) continue;
      const auto name = entry.path().filename().string();
      if (!has_suffix(name, kPartialSuffix)) continue;
      try {
        if (!has_end_marker(entry.path())) {
          std::ofstream output(entry.path(), std::ios::binary | std::ios::app);
          if (!output) throw std::runtime_error("could not append ASC end marker");
          output << kEndMarker;
          output.flush();
          if (!output) throw std::runtime_error("could not flush ASC end marker");
        }
        auto destination = unique_recovery_path(completed_path(entry.path()));
        std::filesystem::rename(entry.path(), destination);
        ++result.recovered;
      } catch (...) {
        ++result.failed;
      }
    }
  } catch (...) {
    ++result.failed;
  }
  return result;
}

bool BusMonitorTraceSession::start(unsigned channel) noexcept {
  stop();
  std::scoped_lock lock(mutex_);
  try {
    std::filesystem::create_directories(directory_);
    channel_ = std::max(channel, 1U);
    path_ = make_partial_path(channel_);
    stream_.open(path_, std::ios::binary | std::ios::trunc);
    if (!stream_) {
      fail("could not create passive monitor trace");
      return false;
    }
    stream_ << format_asc_header(std::time(nullptr));
    stream_.flush();
    if (!stream_) {
      fail("could not write passive monitor trace header");
      return false;
    }
    frame_count_ = 0;
    started_at_ = std::chrono::steady_clock::now();
    active_ = true;
    last_error_.clear();
    return true;
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor trace error");
  }
  return false;
}

void BusMonitorTraceSession::append(const CanFrame& frame) noexcept {
  std::scoped_lock lock(mutex_);
  if (!active_) return;
  try {
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                     started_at_)
            .count();
    stream_ << format_asc_record(
        elapsed, channel_,
        frame.transmitted ? CanTraceDirection::transmit
                          : CanTraceDirection::receive,
        frame);
    if (!stream_) {
      fail("could not append passive monitor frame");
      return;
    }
    ++frame_count_;
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor append error");
  }
}

void BusMonitorTraceSession::flush() noexcept {
  std::scoped_lock lock(mutex_);
  if (!active_) return;
  try {
    stream_.flush();
    if (!stream_) fail("could not flush passive monitor trace");
  } catch (...) {
    fail("unknown passive monitor flush error");
  }
}

void BusMonitorTraceSession::stop() noexcept {
  std::scoped_lock lock(mutex_);
  if (!active_) {
    if (stream_.is_open()) stream_.close();
    return;
  }
  const auto partial = path_;
  try {
    stream_ << kEndMarker;
    stream_.flush();
    stream_.close();
    if (!stream_) {
      fail("could not finalize passive monitor trace");
      return;
    }
    const auto destination = unique_recovery_path(completed_path(partial));
    std::filesystem::rename(partial, destination);
    path_ = destination;
    active_ = false;
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor finalization error");
  }
}

bool BusMonitorTraceSession::export_snapshot(
    const SnapshotSink& sink) noexcept {
  if (!sink) return false;
  try {
    std::filesystem::path snapshot_path;
    std::uintmax_t snapshot_size{};
    bool append_end_marker{};
    {
      std::scoped_lock lock(mutex_);
      if (path_.empty()) return false;
      if (active_) {
        stream_.flush();
        if (!stream_) {
          fail("could not flush passive monitor trace snapshot");
          return false;
        }
      }
      snapshot_path = path_;
      snapshot_size = std::filesystem::file_size(snapshot_path);
      append_end_marker = active_;
    }

    std::ifstream input(snapshot_path, std::ios::binary);
    if (!input) {
      std::scoped_lock lock(mutex_);
      last_error_ = "could not open passive monitor trace snapshot";
      return false;
    }
    std::array<char, 64 * 1024> buffer{};
    auto remaining = snapshot_size;
    while (remaining > 0) {
      const auto requested = static_cast<std::streamsize>(
          std::min<std::uintmax_t>(remaining, buffer.size()));
      input.read(buffer.data(), requested);
      const auto count = input.gcount();
      if (count <= 0) {
        std::scoped_lock lock(mutex_);
        last_error_ = "could not read passive monitor trace snapshot";
        return false;
      }
      if (!sink(std::string_view(buffer.data(),
                                 static_cast<std::size_t>(count)))) {
        std::scoped_lock lock(mutex_);
        last_error_ = "snapshot destination rejected trace data";
        return false;
      }
      remaining -= static_cast<std::uintmax_t>(count);
    }
    if (append_end_marker && !sink(kEndMarker)) {
      std::scoped_lock lock(mutex_);
      last_error_ = "snapshot destination rejected ASC end marker";
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    std::scoped_lock lock(mutex_);
    last_error_ = error.what();
  } catch (...) {
    std::scoped_lock lock(mutex_);
    last_error_ = "unknown passive monitor snapshot error";
  }
  return false;
}

bool BusMonitorTraceSession::is_active() const noexcept {
  std::scoped_lock lock(mutex_);
  return active_;
}

std::size_t BusMonitorTraceSession::frame_count() const noexcept {
  std::scoped_lock lock(mutex_);
  return frame_count_;
}

std::filesystem::path BusMonitorTraceSession::path() const {
  std::scoped_lock lock(mutex_);
  return path_;
}

std::string BusMonitorTraceSession::last_error() const {
  std::scoped_lock lock(mutex_);
  return last_error_;
}

void BusMonitorTraceSession::fail(std::string message) noexcept {
  last_error_ = std::move(message);
  active_ = false;
  if (stream_.is_open()) stream_.close();
}

} // namespace uds
