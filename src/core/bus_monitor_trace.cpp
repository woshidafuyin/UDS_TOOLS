#include "core/bus_monitor_trace.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace uds {
namespace {

constexpr std::string_view kLegacyAscEndMarker{"End TriggerBlock\r\n"};
constexpr std::string_view kLegacyAscPartialSuffix{".asc.partial"};
constexpr std::string_view kBlfPartialSuffix{".blf.partial"};
constexpr std::size_t kBlfFileHeaderSize = 144;
constexpr std::uint32_t kBlfCanMessage = 1;
constexpr std::uint32_t kBlfLogContainer = 10;
constexpr std::uint32_t kBlfCanFdMessage = 100;
constexpr std::uint32_t kBlfTimeOneNanosecond = 2;
constexpr std::uint32_t kBlfExtendedId = 0x80000000U;

bool has_suffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.ends_with(suffix);
}

std::optional<std::uint64_t> declared_blf_file_size(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<std::uint8_t, 24> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
      header[0] != 'L' || header[1] != 'O' || header[2] != 'G' ||
      header[3] != 'G') {
    return std::nullopt;
  }
  const auto u32 = [&header](std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      value |= static_cast<std::uint32_t>(header[offset + index])
               << (index * 8U);
    }
    return value;
  };
  const auto u64 = [&header](std::size_t offset) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      value |= static_cast<std::uint64_t>(header[offset + index])
               << (index * 8U);
    }
    return value;
  };
  const auto declared_size = u64(16);
  const auto physical_size = std::filesystem::file_size(path);
  if (u32(4) != kBlfFileHeaderSize ||
      declared_size < kBlfFileHeaderSize || declared_size > physical_size) {
    return std::nullopt;
  }
  return declared_size;
}

template <typename T>
void append_little_endian(std::vector<std::uint8_t>& output, T value) {
  static_assert(std::is_unsigned_v<T>);
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    value >>= 8U;
  }
}

void append_bytes(std::vector<std::uint8_t>& output,
                  std::string_view bytes) {
  output.insert(output.end(), bytes.begin(), bytes.end());
}

std::uint8_t fd_dlc(std::size_t length) {
  if (length <= 8) return static_cast<std::uint8_t>(length);
  if (length <= 12) return 9;
  if (length <= 16) return 10;
  if (length <= 20) return 11;
  if (length <= 24) return 12;
  if (length <= 32) return 13;
  if (length <= 48) return 14;
  return 15;
}

std::array<std::uint16_t, 8> blf_system_time(
    std::chrono::system_clock::time_point time) {
  if (time.time_since_epoch().count() == 0) return {};
  const auto seconds = std::chrono::system_clock::to_time_t(time);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                time.time_since_epoch()) %
                            std::chrono::seconds(1);
  return {static_cast<std::uint16_t>(utc.tm_year + 1900),
          static_cast<std::uint16_t>(utc.tm_mon + 1),
          static_cast<std::uint16_t>(utc.tm_wday),
          static_cast<std::uint16_t>(utc.tm_mday),
          static_cast<std::uint16_t>(utc.tm_hour),
          static_cast<std::uint16_t>(utc.tm_min),
          static_cast<std::uint16_t>(utc.tm_sec),
          static_cast<std::uint16_t>(milliseconds.count())};
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

BusMonitorTraceSession::BusMonitorTraceSession(
    std::filesystem::path directory, std::filesystem::path completed_path)
    : directory_(std::move(directory)),
      fixed_completed_path_(std::move(completed_path)) {}

BusMonitorTraceSession::~BusMonitorTraceSession() noexcept { stop(); }

std::filesystem::path BusMonitorTraceSession::make_partial_path(
    unsigned channel) const {
  if (!fixed_completed_path_.empty()) {
    auto partial = fixed_completed_path_;
    partial += L".partial";
    return partial;
  }
  const auto stem = "bus_monitor_" + timestamp_with_milliseconds() + "_CH" +
                    std::to_string(std::max(channel, 1U));
  auto path = directory_ / (stem + ".blf.partial");
  unsigned suffix = 2;
  while (std::filesystem::exists(path)) {
    path = directory_ /
           (stem + "_" + std::to_string(suffix++) + ".blf.partial");
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
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size <= 0) return false;
    constexpr std::streamoff kTailSize = 256;
    input.seekg(size > kTailSize ? size - kTailSize : 0);
    const std::string tail((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    return tail.find(kLegacyAscEndMarker) != std::string::npos;
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
      const auto legacy_asc = has_suffix(name, kLegacyAscPartialSuffix);
      const auto blf = has_suffix(name, kBlfPartialSuffix);
      if (!legacy_asc && !blf) continue;
      try {
        if (legacy_asc && !has_end_marker(entry.path())) {
          std::ofstream output(entry.path(), std::ios::binary | std::ios::app);
          if (!output) {
            throw std::runtime_error("could not append ASC end marker");
          }
          output << kLegacyAscEndMarker;
          output.flush();
          if (!output) {
            throw std::runtime_error("could not flush ASC end marker");
          }
        }
        if (blf) {
          const auto declared_size = declared_blf_file_size(entry.path());
          if (!declared_size) {
            throw std::runtime_error("invalid interrupted BLF header");
          }
          // The header is updated only after a complete container is flushed.
          // Discard a torn container left after the last committed file size.
          std::filesystem::resize_file(entry.path(), *declared_size);
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
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out |
                            std::ios::trunc);
    if (!stream_) {
      fail("could not create passive monitor BLF");
      return false;
    }
    frame_count_ = 0;
    object_count_ = 0;
    uncompressed_size_ = kBlfFileHeaderSize;
    blf_buffer_.clear();
    started_at_ = std::chrono::steady_clock::now();
    started_wall_at_ = std::chrono::system_clock::now();
    stopped_wall_at_ = started_wall_at_;
    write_blf_header();
    if (!stream_) {
      fail("could not write passive monitor BLF header");
      return false;
    }
    active_ = true;
    last_error_.clear();
    return true;
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor BLF error");
  }
  return false;
}

void BusMonitorTraceSession::append(const CanFrame& frame) noexcept {
  std::scoped_lock lock(mutex_);
  if (!active_) return;
  try {
    append_blf_object(frame);
    ++frame_count_;
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor BLF append error");
  }
}

void BusMonitorTraceSession::write(CanTraceDirection direction,
                                   const CanFrame& frame) noexcept {
  auto recorded = frame;
  recorded.transmitted = direction == CanTraceDirection::transmit;
  append(recorded);
}

void BusMonitorTraceSession::flush() noexcept {
  std::scoped_lock lock(mutex_);
  if (!active_) return;
  try {
    flush_blf_container();
    if (!stream_) fail("could not flush passive monitor BLF");
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor BLF flush error");
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
    flush_blf_container();
    stream_.close();
    if (!stream_) {
      fail("could not finalize passive monitor BLF");
      return;
    }
    const auto destination = unique_recovery_path(completed_path(partial));
    std::filesystem::rename(partial, destination);
    path_ = destination;
    active_ = false;
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("unknown passive monitor BLF finalization error");
  }
}

bool BusMonitorTraceSession::export_snapshot(
    const SnapshotSink& sink) noexcept {
  if (!sink) return false;
  try {
    std::filesystem::path snapshot_path;
    std::uintmax_t snapshot_size{};
    {
      std::scoped_lock lock(mutex_);
      if (path_.empty()) return false;
      if (active_) flush_blf_container();
      snapshot_path = path_;
      snapshot_size = std::filesystem::file_size(snapshot_path);
    }

    std::ifstream input(snapshot_path, std::ios::binary);
    if (!input) {
      std::scoped_lock lock(mutex_);
      last_error_ = "could not open passive monitor BLF snapshot";
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
        last_error_ = "could not read passive monitor BLF snapshot";
        return false;
      }
      if (!sink(std::string_view(buffer.data(),
                                 static_cast<std::size_t>(count)))) {
        std::scoped_lock lock(mutex_);
        last_error_ = "snapshot destination rejected BLF data";
        return false;
      }
      remaining -= static_cast<std::uintmax_t>(count);
    }
    return true;
  } catch (const std::exception& error) {
    std::scoped_lock lock(mutex_);
    last_error_ = error.what();
  } catch (...) {
    std::scoped_lock lock(mutex_);
    last_error_ = "unknown passive monitor BLF snapshot error";
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

void BusMonitorTraceSession::append_blf_object(const CanFrame& frame) {
  if ((!frame.fd && frame.data.size() > 8) || frame.data.size() > 64) {
    throw std::runtime_error("CAN frame payload exceeds BLF object capacity");
  }

  std::vector<std::uint8_t> payload;
  const auto can_id = frame.id | (frame.extended ? kBlfExtendedId : 0U);
  const auto direction = static_cast<std::uint8_t>(frame.transmitted ? 1U : 0U);
  std::uint32_t object_type = kBlfCanMessage;
  if (frame.fd) {
    object_type = kBlfCanFdMessage;
    append_little_endian(payload, static_cast<std::uint16_t>(channel_));
    payload.push_back(direction);
    payload.push_back(fd_dlc(frame.data.size()));
    append_little_endian(payload, can_id);
    append_little_endian(payload, std::uint32_t{0});
    payload.push_back(0);
    payload.push_back(static_cast<std::uint8_t>(1U | (frame.brs ? 2U : 0U)));
    payload.push_back(static_cast<std::uint8_t>(frame.data.size()));
    payload.insert(payload.end(), 5, 0);
    payload.insert(payload.end(), frame.data.begin(), frame.data.end());
    payload.resize(84, 0);
  } else {
    append_little_endian(payload, static_cast<std::uint16_t>(channel_));
    payload.push_back(direction);
    payload.push_back(static_cast<std::uint8_t>(frame.data.size()));
    append_little_endian(payload, can_id);
    payload.insert(payload.end(), frame.data.begin(), frame.data.end());
    payload.resize(16, 0);
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started_at_)
                           .count();
  const auto object_size = static_cast<std::uint32_t>(32U + payload.size());
  append_bytes(blf_buffer_, "LOBJ");
  append_little_endian(blf_buffer_, std::uint16_t{32});
  append_little_endian(blf_buffer_, std::uint16_t{1});
  append_little_endian(blf_buffer_, object_size);
  append_little_endian(blf_buffer_, object_type);
  append_little_endian(blf_buffer_, kBlfTimeOneNanosecond);
  append_little_endian(blf_buffer_, std::uint16_t{0});
  append_little_endian(blf_buffer_, std::uint16_t{0});
  append_little_endian(
      blf_buffer_, static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)));
  blf_buffer_.insert(blf_buffer_.end(), payload.begin(), payload.end());
  while (blf_buffer_.size() % 4U != 0U) blf_buffer_.push_back(0);
  ++object_count_;
  stopped_wall_at_ = std::chrono::system_clock::now();
}

void BusMonitorTraceSession::flush_blf_container() {
  if (!stream_) {
    throw std::runtime_error("passive monitor BLF stream is closed");
  }
  if (!blf_buffer_.empty()) {
    const auto object_size = static_cast<std::uint32_t>(
        16U + 16U + blf_buffer_.size());
    std::vector<std::uint8_t> header;
    append_bytes(header, "LOBJ");
    append_little_endian(header, std::uint16_t{16});
    append_little_endian(header, std::uint16_t{1});
    append_little_endian(header, object_size);
    append_little_endian(header, kBlfLogContainer);
    append_little_endian(header, std::uint16_t{0});
    header.insert(header.end(), 6, 0);
    append_little_endian(header,
                         static_cast<std::uint32_t>(blf_buffer_.size()));
    header.insert(header.end(), 4, 0);
    stream_.seekp(0, std::ios::end);
    stream_.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
    stream_.write(reinterpret_cast<const char*>(blf_buffer_.data()),
                  static_cast<std::streamsize>(blf_buffer_.size()));
    uncompressed_size_ += header.size() + blf_buffer_.size();
    blf_buffer_.clear();
  }
  write_blf_header();
}

void BusMonitorTraceSession::write_blf_header() {
  stream_.seekp(0, std::ios::end);
  const auto end = stream_.tellp();
  const auto file_size = end < 0 ? std::uint64_t{kBlfFileHeaderSize}
                                 : std::max<std::uint64_t>(
                                       static_cast<std::uint64_t>(end),
                                       kBlfFileHeaderSize);
  std::vector<std::uint8_t> header;
  append_bytes(header, "LOGG");
  append_little_endian(header,
                       static_cast<std::uint32_t>(kBlfFileHeaderSize));
  for (const auto value : {5U, 0U, 0U, 0U, 2U, 6U, 8U, 1U}) {
    header.push_back(static_cast<std::uint8_t>(value));
  }
  append_little_endian(header, file_size);
  append_little_endian(header, uncompressed_size_);
  append_little_endian(header, object_count_);
  append_little_endian(header, std::uint32_t{0});
  for (const auto value : blf_system_time(started_wall_at_)) {
    append_little_endian(header, value);
  }
  for (const auto value : blf_system_time(stopped_wall_at_)) {
    append_little_endian(header, value);
  }
  header.resize(kBlfFileHeaderSize, 0);
  stream_.seekp(0, std::ios::beg);
  stream_.write(reinterpret_cast<const char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
  stream_.seekp(0, std::ios::end);
  stream_.flush();
}

void BusMonitorTraceSession::fail(std::string message) noexcept {
  last_error_ = std::move(message);
  active_ = false;
  blf_buffer_.clear();
  if (stream_.is_open()) stream_.close();
}

} // namespace uds
