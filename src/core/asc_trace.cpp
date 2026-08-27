#include "core/asc_trace.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace uds {
namespace {

std::string asc_date(std::time_t value) {
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &value);
#else
  localtime_r(&value, &local);
#endif
  std::ostringstream output;
  output << std::put_time(&local, "%a %b %d %I:%M:%S %p %Y");
  return output.str();
}

unsigned fd_dlc(std::size_t length) {
  if (length <= 8) return static_cast<unsigned>(length);
  if (length <= 12) return 9;
  if (length <= 16) return 10;
  if (length <= 20) return 11;
  if (length <= 24) return 12;
  if (length <= 32) return 13;
  if (length <= 48) return 14;
  return 15;
}

std::wstring slug(std::wstring_view value, std::wstring_view fallback) {
  std::wstring result;
  result.reserve(value.size());
  bool separator = false;
  for (const auto character : value) {
    const auto ascii = character >= L'A' && character <= L'Z'
                           ? static_cast<wchar_t>(character - L'A' + L'a')
                           : character;
    if ((ascii >= L'a' && ascii <= L'z') ||
        (ascii >= L'0' && ascii <= L'9')) {
      result.push_back(ascii);
      separator = false;
    } else if (!result.empty() && !separator) {
      result.push_back(L'_');
      separator = true;
    }
  }
  while (!result.empty() && result.back() == L'_') result.pop_back();
  return result.empty() ? std::wstring(fallback) : result;
}

std::wstring timestamp() {
  const auto now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  std::wostringstream output;
  output << std::put_time(&local, L"%Y%m%d_%H%M%S");
  return output.str();
}

std::wstring_view trace_category(std::wstring_view operation) {
  if (operation == L"probe") return L"probe";
  if (operation == L"version") return L"version";
  if (operation == L"diagnostic") return L"diagnostic";
  if (operation == L"app" || operation == L"ft" ||
      operation == L"cal" || operation == L"app_cal" ||
      operation == L"boot") {
    return L"flash";
  }
  return L"other";
}

} // namespace

std::string format_asc_header(std::time_t wall_time) {
  std::ostringstream output;
  output << "date " << asc_date(wall_time) << "\r\n"
         << "base hex  timestamps absolute\r\n"
         << "no internal events logged\r\n"
         << "Begin Triggerblock " << asc_date(wall_time) << "\r\n";
  return output.str();
}

std::string format_asc_record(double timestamp_seconds, unsigned channel,
                              CanTraceDirection direction,
                              const CanFrame& frame) {
  std::ostringstream output;
  const auto id_suffix = frame.extended ? "x" : "";
  const auto direction_text =
      direction == CanTraceDirection::transmit ? "Tx" : "Rx";

  output << std::fixed << std::setprecision(6) << std::setw(12)
         << timestamp_seconds << ' ';
  if (frame.fd) {
    output << "CANFD " << std::max(channel, 1U) << ' ' << direction_text << ' '
           << std::hex << frame.id << id_suffix << std::dec << " 1 "
           << (frame.brs ? 1 : 0) << ' ' << std::hex
           << fd_dlc(frame.data.size()) << ' ' << frame.data.size()
           << std::dec;
  } else {
    output << std::max(channel, 1U) << ' ' << std::hex << frame.id << id_suffix
           << std::dec << ' ' << direction_text << " d "
           << frame.data.size();
  }
  for (const auto byte : frame.data) {
    output << ' ' << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<unsigned>(byte)
           << std::nouppercase << std::dec << std::setfill(' ');
  }
  if (frame.fd) output << " 0 0 0 0 0";
  output << "\r\n";
  return output.str();
}

AscTraceWriter::AscTraceWriter(std::filesystem::path path,
                               unsigned channel) noexcept
    : path_(std::move(path)), channel_(std::max(channel, 1U)),
      started_at_(std::chrono::steady_clock::now()) {
  try {
    if (path_.empty()) return;
    const auto parent = path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    stream_.open(path_, std::ios::binary | std::ios::trunc);
    if (stream_) {
      write_header();
      open_.store(true);
      writer_thread_ = std::thread([this] { writer_loop(); });
    }
  } catch (...) {
    open_.store(false);
    stream_.close();
  }
}

AscTraceWriter::~AscTraceWriter() noexcept {
  try {
    {
      std::scoped_lock lock(queue_mutex_);
      stopping_ = true;
    }
    queue_changed_.notify_one();
    if (writer_thread_.joinable()) writer_thread_.join();
    if (stream_) {
      stream_ << "End TriggerBlock\r\n";
      stream_.flush();
    }
    open_.store(false);
  } catch (...) {
  }
}

bool AscTraceWriter::is_open() const noexcept {
  return open_.load();
}

void AscTraceWriter::write_header() noexcept {
  try {
    stream_ << format_asc_header(std::time(nullptr));
    stream_.flush();
  } catch (...) {
  }
}

void AscTraceWriter::write(CanTraceDirection direction,
                           const CanFrame& frame) noexcept {
  try {
    if (!open_.load()) return;
    Record record;
    record.timestamp_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                     started_at_)
            .count();
    record.direction = direction;
    record.frame = frame;
    {
      std::scoped_lock lock(queue_mutex_);
      if (stopping_) return;
      queue_.push_back(std::move(record));
    }
    queue_changed_.notify_one();
  } catch (...) {
    // Trace persistence must never alter CAN communication or flashing.
  }
}

void AscTraceWriter::write_record(const Record& record) noexcept {
  try {
    stream_ << format_asc_record(record.timestamp_seconds, channel_,
                                 record.direction, record.frame);
  } catch (...) {
    open_.store(false);
  }
}

void AscTraceWriter::writer_loop() noexcept {
  try {
    std::size_t records_since_flush{};
    for (;;) {
      std::deque<Record> pending;
      {
        std::unique_lock lock(queue_mutex_);
        queue_changed_.wait(lock,
                            [this] { return stopping_ || !queue_.empty(); });
        pending.swap(queue_);
        if (pending.empty() && stopping_) break;
      }
      for (const auto& record : pending) {
        write_record(record);
        if (++records_since_flush >= 1024) {
          stream_.flush();
          records_since_flush = 0;
        }
      }
      {
        std::scoped_lock lock(queue_mutex_);
        if (stopping_ && queue_.empty()) break;
      }
    }
    stream_.flush();
  } catch (...) {
    open_.store(false);
  }
}

TracingCanBus::TracingCanBus(std::unique_ptr<ICanBus> inner,
                             std::shared_ptr<AscTraceWriter> trace)
    : inner_(std::move(inner)), trace_(std::move(trace)) {
  if (!inner_) throw std::invalid_argument("tracing CAN bus requires an inner bus");
}

void TracingCanBus::open() { inner_->open(); }

void TracingCanBus::close() noexcept { inner_->close(); }

bool TracingCanBus::is_open() const noexcept { return inner_->is_open(); }

void TracingCanBus::send(const CanFrame& frame) {
  inner_->send(frame);
  if (trace_) trace_->write(CanTraceDirection::transmit, frame);
}

bool TracingCanBus::supports_batch_transmit() const noexcept {
  return inner_->supports_batch_transmit();
}

void TracingCanBus::send_batch(std::span<const CanFrame> frames) {
  if (!inner_->supports_batch_transmit()) {
    for (const auto& frame : frames) send(frame);
    return;
  }
  inner_->send_batch(frames);
  if (!trace_) return;
  for (const auto& frame : frames) {
    trace_->write(CanTraceDirection::transmit, frame);
  }
}

std::optional<CanFrame> TracingCanBus::receive(
    std::chrono::milliseconds timeout) {
  auto frame = inner_->receive(timeout);
  if (frame && trace_) trace_->write(CanTraceDirection::receive, *frame);
  return frame;
}

std::filesystem::path make_asc_trace_path(
    const std::filesystem::path& executable_directory,
    std::wstring_view profile_id, std::wstring_view target_id,
    std::wstring_view operation) {
  auto file_name = std::wstring(L"trace_") + timestamp() + L"_" +
                   slug(profile_id, L"unknown") + L"_" +
                   slug(target_id, L"default") + L"_" +
                   slug(operation, L"operation") + L".asc";
  const auto directory = executable_directory / L"logs" / L"traces" /
                         trace_category(operation);
  auto path = directory / file_name;
  unsigned suffix = 2;
  while (std::filesystem::exists(path)) {
    file_name = std::wstring(L"trace_") + timestamp() + L"_" +
                slug(profile_id, L"unknown") + L"_" +
                slug(target_id, L"default") + L"_" +
                slug(operation, L"operation") + L"_" +
                std::to_wstring(suffix++) + L".asc";
    path = directory / file_name;
  }
  return path;
}

} // namespace uds
