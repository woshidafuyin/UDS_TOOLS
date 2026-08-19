#pragma once

#include "drivers/can/can_hardware_adapter.hpp"

#include <memory>
#include <mutex>

namespace uds {

class CanBusSession final : public ICanBus {
public:
  CanBusSession(std::unique_ptr<ICanHardwareAdapter> adapter,
                CanChannelConfig config);
  ~CanBusSession() override;

  CanBusSession(const CanBusSession&) = delete;
  CanBusSession& operator=(const CanBusSession&) = delete;

  void open() override;
  void close() noexcept override;
  [[nodiscard]] bool is_open() const noexcept override;
  void send(const CanFrame& frame) override;
  [[nodiscard]] bool supports_batch_transmit() const noexcept override;
  void send_batch(std::span<const CanFrame> frames) override;
  std::optional<CanFrame> receive(std::chrono::milliseconds timeout) override;

  [[nodiscard]] CanVendor vendor() const noexcept;
  [[nodiscard]] CanAdapterError last_error() const;

private:
  std::unique_ptr<ICanHardwareAdapter> adapter_;
  CanChannelConfig config_;
  mutable std::mutex lifecycle_mutex_;
  std::mutex transmit_mutex_;
};

} // namespace uds
