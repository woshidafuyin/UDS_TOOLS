#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uds {

struct CanFrame {
  std::uint32_t id{};
  std::vector<std::uint8_t> data;
  bool extended{};
  bool fd{};
  bool brs{};
  bool transmitted{};
};

class ICanBus {
public:
  virtual ~ICanBus() = default;
  virtual void open() = 0;
  virtual void close() noexcept = 0;
  virtual bool is_open() const noexcept = 0;
  virtual void send(const CanFrame& frame) = 0;
  [[nodiscard]] virtual bool supports_batch_transmit() const noexcept {
    return false;
  }
  virtual void send_batch(std::span<const CanFrame> frames) {
    for (const auto& frame : frames) send(frame);
  }
  virtual std::optional<CanFrame> receive(std::chrono::milliseconds timeout) = 0;
};

} // namespace uds
