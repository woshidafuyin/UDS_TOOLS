#include "core/isotp.hpp"
#include "core/hex.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {
using namespace std::chrono_literals;

void throw_if_cancelled(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw std::runtime_error("operation cancelled by user");
  }
}

bool valid_can_data_length(std::size_t length) {
  constexpr std::array<std::size_t, 8> kLengths{
      8, 12, 16, 20, 24, 32, 48, 64};
  return std::find(kLengths.begin(), kLengths.end(), length) !=
         kLengths.end();
}

std::size_t rounded_can_fd_length(std::size_t required,
                                  std::size_t maximum) {
  constexpr std::array<std::size_t, 8> kLengths{
      8, 12, 16, 20, 24, 32, 48, 64};
  const auto selected =
      std::find_if(kLengths.begin(), kLengths.end(),
                   [required](std::size_t length) {
                     return length >= required;
                   });
  if (selected == kLengths.end() || *selected > maximum) {
    throw std::invalid_argument("ISO-TP payload exceeds configured CAN FD data length");
  }
  return *selected;
}
} // namespace

IsoTpSession::IsoTpSession(ICanBus& bus, IsoTpConfig config)
    : bus_(bus), config_(config) {}

std::array<std::uint8_t, 8> IsoTpSession::padded(std::span<const std::uint8_t> bytes) const {
  std::array<std::uint8_t, 8> frame{};
  frame.fill(config_.padding);
  std::copy(bytes.begin(), bytes.end(), frame.begin());
  return frame;
}

void IsoTpSession::send_raw(std::uint32_t can_id, std::span<const std::uint8_t> data) {
  send_formatted(can_id, data, config_.tx_fd, config_.tx_brs);
}

void IsoTpSession::send_formatted(std::uint32_t can_id,
                                  std::span<const std::uint8_t> data,
                                  bool fd, bool brs) {
  bus_.send(CanFrame{can_id, {data.begin(), data.end()}, config_.tx_extended,
                     fd, brs});
}

CanFrame IsoTpSession::wait_for_id(std::chrono::milliseconds timeout,
                                   std::stop_token stop) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    throw_if_cancelled(stop);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto receive_slice =
        std::min(std::max(remaining, 1ms), 20ms);
    auto frame = bus_.receive(receive_slice);
    if (frame &&
        (frame->id == config_.rx_id ||
         (config_.alternate_rx_id != 0 &&
          frame->id == config_.alternate_rx_id)) &&
        frame->extended == config_.rx_extended) {
      last_rx_id_ = frame->id;
      return *frame;
    }
  }
  throw_if_cancelled(stop);
  throw IsoTpReceiveTimeout();
}

std::chrono::microseconds IsoTpSession::st_min_delay(std::uint8_t value) {
  if (value <= 0x7F) return std::chrono::milliseconds(value);
  if (value >= 0xF1 && value <= 0xF9) return std::chrono::microseconds((value - 0xF0) * 100);
  return std::chrono::microseconds(0);
}

void IsoTpSession::send(std::span<const std::uint8_t> payload,
                         std::stop_token stop) {
  throw_if_cancelled(stop);
  if (payload.empty() || payload.size() > 4095) throw std::invalid_argument("invalid ISO-TP payload size");
  const auto frame_length =
      config_.tx_fd ? config_.tx_data_length : std::size_t{8};
  if (!valid_can_data_length(frame_length)) {
    throw std::invalid_argument("invalid ISO-TP CAN data length");
  }
  if (config_.drain_receive_before_send) {
    constexpr std::size_t kMaximumStaleFrames = 4096;
    for (std::size_t drained = 0; drained < kMaximumStaleFrames; ++drained) {
      throw_if_cancelled(stop);
      if (!bus_.receive(0ms)) break;
    }
  }
  if (payload.size() <= 7) {
    std::array<std::uint8_t, 8> bytes{};
    bytes.fill(config_.padding);
    bytes[0] = static_cast<std::uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), bytes.begin() + 1);
    send_raw(config_.tx_id, bytes);
    return;
  }

  // ISO 15765-2 CAN FD Single Frames with more than seven payload bytes use
  // the escape-length form 00 LL. Round the actual CAN data length to a legal
  // CAN FD DLC, matching the captured Vector traffic.
  if (config_.tx_fd && frame_length > 8 &&
      payload.size() <= frame_length - 2U) {
    const auto data_length =
        rounded_can_fd_length(payload.size() + 2U, frame_length);
    std::vector<std::uint8_t> single(data_length, config_.padding);
    single[0] = 0x00;
    single[1] = static_cast<std::uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), single.begin() + 2);
    send_raw(config_.tx_id, single);
    return;
  }

  std::vector<std::uint8_t> first(frame_length, config_.padding);
  first[0] = static_cast<std::uint8_t>(0x10U | ((payload.size() >> 8U) & 0x0FU));
  first[1] = static_cast<std::uint8_t>(payload.size() & 0xFFU);
  const auto first_payload = frame_length - 2U;
  std::copy_n(payload.begin(), first_payload, first.begin() + 2);
  send_raw(config_.tx_id, first);

  CanFrame fc;
  try {
    fc = wait_for_id(config_.flow_control_timeout, stop);
  } catch (const std::exception&) {
    throw std::runtime_error("ISO-TP first flow-control timeout");
  }
  if (fc.data.size() < 3 || (fc.data[0] & 0xF0U) != 0x30U || (fc.data[0] & 0x0FU) != 0) {
    throw std::runtime_error("ISO-TP flow control rejected; received " +
                             to_hex(fc.data));
  }
  auto block_size = fc.data[1];
  auto separation = st_min_delay(fc.data[2]);
  auto consecutive_fd = config_.tx_fd;
  auto consecutive_brs = config_.tx_brs;
  if (config_.adapt_consecutive_frames_to_flow_control) {
    consecutive_fd = fc.fd;
    consecutive_brs = fc.brs;
  }
  std::size_t offset = first_payload;
  std::uint8_t sequence = 1;
  unsigned block_count = 0;
  const auto next_consecutive_frame = [&]() {
    const auto count =
        std::min(frame_length - 1U, payload.size() - offset);
    const auto data_length =
        config_.tx_fd && frame_length > 8
            ? rounded_can_fd_length(count + 1U, frame_length)
            : std::size_t{8};
    std::vector<std::uint8_t> bytes(data_length, config_.padding);
    bytes[0] = static_cast<std::uint8_t>(0x20U | (sequence & 0x0FU));
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), count,
                bytes.begin() + 1);
    offset += count;
    sequence = static_cast<std::uint8_t>((sequence + 1U) & 0x0FU);
    ++block_count;
    return CanFrame{config_.tx_id, {bytes.begin(), bytes.end()},
                    config_.tx_extended, consecutive_fd, consecutive_brs};
  };
  while (offset < payload.size()) {
    throw_if_cancelled(stop);
    if (separation.count() == 0) {
      std::vector<CanFrame> batch;
      const auto remaining_frames =
          (payload.size() - offset + (frame_length - 2U)) /
          (frame_length - 1U);
      const auto batch_limit = !config_.batch_consecutive_frames
                                   ? std::size_t{1}
                                   : block_size == 0
                                         ? remaining_frames
                                         : std::min<std::size_t>(
                                               remaining_frames,
                                               block_size - block_count);
      batch.reserve(batch_limit);
      while (batch.size() < batch_limit && offset < payload.size() &&
             (block_size == 0 || block_count < block_size)) {
        batch.push_back(next_consecutive_frame());
      }
      if (batch.size() == 1U) {
        bus_.send(batch.front());
      } else {
        bus_.send_batch(batch);
      }
    } else {
      bus_.send(next_consecutive_frame());
      std::this_thread::sleep_for(separation);
    }
    if (block_size != 0 && block_count >= block_size && offset < payload.size()) {
      try {
        fc = wait_for_id(config_.flow_control_timeout, stop);
      } catch (const std::exception&) {
        throw std::runtime_error("ISO-TP block flow-control timeout");
      }
      if (fc.data.size() < 3 || (fc.data[0] & 0xF0U) != 0x30U ||
          (fc.data[0] & 0x0FU) != 0) {
        throw std::runtime_error("ISO-TP block FC rejected; received " +
                                 to_hex(fc.data));
      }
      block_size = fc.data[1];
      separation = st_min_delay(fc.data[2]);
      if (config_.adapt_consecutive_frames_to_flow_control) {
        consecutive_fd = fc.fd;
        consecutive_brs = fc.brs;
      }
      block_count = 0;
    }
  }
}

std::vector<std::uint8_t> IsoTpSession::receive(
    std::chrono::milliseconds timeout, std::stop_token stop) {
  auto first = wait_for_id(timeout, stop);
  if (first.data.empty()) throw std::runtime_error("empty CAN frame");
  const auto type = static_cast<std::uint8_t>(first.data[0] >> 4U);
  if (type == 0) {
    const bool escape_length = first.data[0] == 0 && first.data.size() > 8;
    const auto prefix = escape_length ? 2U : 1U;
    if (escape_length && first.data.size() < 2U) {
      throw std::runtime_error("short ISO-TP CAN FD SF");
    }
    const auto length =
        escape_length ? static_cast<std::size_t>(first.data[1])
                      : static_cast<std::size_t>(first.data[0] & 0x0FU);
    if (first.data.size() < length + prefix) {
      throw std::runtime_error("short ISO-TP SF");
    }
    return {first.data.begin() + static_cast<std::ptrdiff_t>(prefix),
            first.data.begin() +
                static_cast<std::ptrdiff_t>(prefix + length)};
  }
  if (type != 1 || first.data.size() < 8) throw std::runtime_error("unexpected ISO-TP PCI");

  const auto total = static_cast<std::size_t>(((first.data[0] & 0x0FU) << 8U) | first.data[1]);
  std::vector<std::uint8_t> result(first.data.begin() + 2, first.data.end());
  const std::array<std::uint8_t, 8> fc{0x30, config_.block_size, config_.st_min, 0, 0, 0, 0, 0};
  if (config_.flow_control_delay.count() > 0) {
    std::this_thread::sleep_for(config_.flow_control_delay);
  }
  const auto flow_control_fd = config_.adapt_flow_control_to_first_frame
                                   ? first.fd
                                   : config_.tx_fd;
  const auto flow_control_brs = config_.adapt_flow_control_to_first_frame
                                    ? first.brs
                                    : config_.tx_brs;
  send_formatted(config_.tx_id, fc, flow_control_fd, flow_control_brs);
  std::uint8_t expected = 1;
  std::array<std::optional<CanFrame>, 16> pending;
  while (result.size() < total) {
    CanFrame cf;
    if (pending[expected]) {
      cf = std::move(*pending[expected]);
      pending[expected].reset();
    } else {
      cf = wait_for_id(config_.consecutive_frame_timeout, stop);
    }
    if (cf.data.empty() || (cf.data[0] >> 4U) != 2) {
      throw std::runtime_error("ISO-TP consecutive frame sequence error");
    }
    const auto sequence = static_cast<std::uint8_t>(cf.data[0] & 0x0FU);
    if (sequence != expected) {
      const auto forward_distance =
          static_cast<std::uint8_t>((sequence - expected) & 0x0FU);
      if (forward_distance == 0 || forward_distance > 8 ||
          pending[sequence].has_value()) {
        throw std::runtime_error("ISO-TP consecutive frame sequence error");
      }
      pending[sequence] = std::move(cf);
      continue;
    }
    result.insert(result.end(), cf.data.begin() + 1, cf.data.end());
    expected = static_cast<std::uint8_t>((expected + 1U) & 0x0FU);
  }
  result.resize(total);
  return result;
}

} // namespace uds
