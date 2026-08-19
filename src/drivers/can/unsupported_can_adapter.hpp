#pragma once

#include "drivers/can/can_hardware_adapter.hpp"

#include <utility>

namespace uds {

class UnsupportedCanAdapter : public ICanHardwareAdapter {
public:
  UnsupportedCanAdapter(CanVendor vendor, std::string name,
                        CanAdapterErrorCode code =
                            CanAdapterErrorCode::NotImplemented)
      : vendor_(vendor), name_(std::move(name)),
        error_{code, name_ + " CAN adapter is not implemented"} {}

  [[nodiscard]] CanVendor vendor() const noexcept override { return vendor_; }
  void initialize() override { fail(); }
  void release() noexcept override {}
  std::vector<CanDeviceInfo> enumerate_devices() override { fail(); }
  void open_device(std::string_view) override { fail(); }
  void close_device() noexcept override {}
  void configure_channel(const CanChannelConfig&) override { fail(); }
  void start_channel() override { fail(); }
  void stop_channel() noexcept override {}
  void send(const CanFrame&) override { fail(); }
  std::optional<CanFrame> receive(std::chrono::milliseconds) override {
    fail();
  }
  [[nodiscard]] CanAdapterStatus status() const noexcept override {
    return {vendor_, CanAdapterState::Unsupported, false, false, false};
  }
  [[nodiscard]] CanAdapterError last_error() const override { return error_; }

protected:
  [[noreturn]] void fail() const { throw CanAdapterException(error_); }

private:
  CanVendor vendor_;
  std::string name_;
  CanAdapterError error_;
};

} // namespace uds
