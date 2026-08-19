#pragma once

#include "drivers/can/can_hardware_adapter.hpp"

#include <memory>
#include <mutex>

namespace uds {

class TosunCanAdapter final : public ICanHardwareAdapter {
public:
  TosunCanAdapter();
  ~TosunCanAdapter() override;

  [[nodiscard]] CanVendor vendor() const noexcept override;
  void initialize() override;
  void release() noexcept override;
  std::vector<CanDeviceInfo> enumerate_devices() override;
  void open_device(std::string_view device_id) override;
  void close_device() noexcept override;
  void configure_channel(const CanChannelConfig& config) override;
  void start_channel() override;
  void stop_channel() noexcept override;
  void send(const CanFrame& frame) override;
  [[nodiscard]] bool supports_batch_transmit() const noexcept override;
  void send_batch(std::span<const CanFrame> frames) override;
  std::optional<CanFrame> receive(
      std::chrono::milliseconds timeout) override;
  [[nodiscard]] CanAdapterStatus status() const noexcept override;
  [[nodiscard]] CanAdapterError last_error() const override;

private:
  struct Impl;

  [[noreturn]] void fail(CanAdapterErrorCode code, std::string message);
  void remember_error(CanAdapterErrorCode code, std::string message) noexcept;

  mutable std::mutex state_mutex_;
  std::mutex transmit_mutex_;
  std::unique_ptr<Impl> impl_;
  CanAdapterStatus status_{CanVendor::Tosun,
                           CanAdapterState::Uninitialized};
  CanAdapterError last_error_;
};

} // namespace uds
