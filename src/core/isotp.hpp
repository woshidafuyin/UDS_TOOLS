#pragma once

#include "core/can_bus.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace uds {

struct IsoTpConfig {
  std::uint32_t tx_id{0x772};
  std::uint32_t rx_id{0x77A};
  std::uint8_t padding{0x55};
  std::uint8_t block_size{0};
  std::uint8_t st_min{10};
  std::chrono::milliseconds flow_control_timeout{1000};
  std::chrono::milliseconds consecutive_frame_timeout{1000};
  bool tx_extended{};
  bool rx_extended{};
  bool tx_fd{};
  bool tx_brs{};
  // CANoe's "adapt" mode may start a request as CAN FD+BRS and then switch
  // its consecutive frames to the format used by the ECU's FlowControl.
  bool adapt_consecutive_frames_to_flow_control{};
  // The same adaptation is needed in the opposite direction: when the ECU
  // starts a response with a Classic CAN FirstFrame, answer with a Classic
  // CAN FlowControl even though request Single/FirstFrames use CAN FD.
  bool adapt_flow_control_to_first_frame{};
  std::chrono::milliseconds flow_control_delay{};
  // Some adapters retain all traffic received during a wake-up settle. Drain
  // those stale frames immediately before a new ISO-TP request so they cannot
  // delay the response FlowControl path. Keep this opt-in because receive-only
  // sessions and preloaded test transports must preserve their queued frames.
  bool drain_receive_before_send{};
  // Maximum CAN data length used by this sender. The default preserves the
  // existing 8-byte ISO-TP behavior. CAN FD projects that were captured with
  // 64-byte First/Consecutive Frames opt in explicitly.
  std::size_t tx_data_length{8};
  // Batch transmit is efficient for existing flows. A project with a
  // concurrent high-rate wake-up frame can disable it so that frame may be
  // interleaved between ISO-TP Consecutive Frames.
  bool batch_consecutive_frames{true};
  // Some bootloader transitions keep FlowControl/NRC 0x78 on the APP
  // response ID but publish the final response on the new runtime ID.
  // Zero disables the alternate receive endpoint.
  std::uint32_t alternate_rx_id{};
};

class IsoTpSession {
public:
  IsoTpSession(ICanBus& bus, IsoTpConfig config);
  void send(std::span<const std::uint8_t> payload,
            std::stop_token stop = {});
  std::vector<std::uint8_t> receive(std::chrono::milliseconds timeout,
                                    std::stop_token stop = {});
  void send_raw(std::uint32_t can_id, std::span<const std::uint8_t> data);
  [[nodiscard]] std::uint32_t tx_id() const noexcept { return config_.tx_id; }
  [[nodiscard]] std::uint32_t rx_id() const noexcept { return config_.rx_id; }
  [[nodiscard]] std::uint32_t last_rx_id() const noexcept {
    return last_rx_id_;
  }

private:
  ICanBus& bus_;
  IsoTpConfig config_;
  std::uint32_t last_rx_id_{};
  CanFrame wait_for_id(std::chrono::milliseconds timeout,
                       std::stop_token stop = {});
  void send_formatted(std::uint32_t can_id,
                      std::span<const std::uint8_t> data,
                      bool fd, bool brs);
  std::array<std::uint8_t, 8> padded(std::span<const std::uint8_t> bytes) const;
  static std::chrono::microseconds st_min_delay(std::uint8_t value);
};

} // namespace uds
