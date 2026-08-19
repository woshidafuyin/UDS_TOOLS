#pragma once

#include "core/can_bus.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace uds {

enum class CanTraceDirection {
  transmit,
  receive,
};

class AscTraceWriter final {
public:
  explicit AscTraceWriter(std::filesystem::path path,
                          unsigned channel = 1) noexcept;
  ~AscTraceWriter() noexcept;

  AscTraceWriter(const AscTraceWriter&) = delete;
  AscTraceWriter& operator=(const AscTraceWriter&) = delete;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }
  void write(CanTraceDirection direction, const CanFrame& frame) noexcept;

private:
  struct Record {
    double timestamp_seconds{};
    CanTraceDirection direction{};
    CanFrame frame;
  };

  void write_header() noexcept;
  void write_record(const Record& record) noexcept;
  void writer_loop() noexcept;

  std::filesystem::path path_;
  unsigned channel_{1};
  std::chrono::steady_clock::time_point started_at_;
  std::atomic_bool open_{false};
  std::mutex queue_mutex_;
  std::condition_variable queue_changed_;
  std::deque<Record> queue_;
  bool stopping_{};
  std::ofstream stream_;
  std::thread writer_thread_;
};

class TracingCanBus final : public ICanBus {
public:
  TracingCanBus(std::unique_ptr<ICanBus> inner,
                std::shared_ptr<AscTraceWriter> trace);

  void open() override;
  void close() noexcept override;
  [[nodiscard]] bool is_open() const noexcept override;
  void send(const CanFrame& frame) override;
  [[nodiscard]] bool supports_batch_transmit() const noexcept override;
  void send_batch(std::span<const CanFrame> frames) override;
  std::optional<CanFrame> receive(std::chrono::milliseconds timeout) override;

private:
  std::unique_ptr<ICanBus> inner_;
  std::shared_ptr<AscTraceWriter> trace_;
};

[[nodiscard]] std::filesystem::path make_asc_trace_path(
    const std::filesystem::path& executable_directory,
    std::wstring_view profile_id, std::wstring_view target_id,
    std::wstring_view operation);

} // namespace uds
