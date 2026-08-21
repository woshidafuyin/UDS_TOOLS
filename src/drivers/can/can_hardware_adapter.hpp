#pragma once

#include "core/can_bus.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

// Keep the stable UI/default order aligned with the product contract:
// Vector, ZLG, TOSUN, Kvaser, followed by the unsupported extension slot.
enum class CanVendor { Vector, Zlg, Tosun, Kvaser, Other };

enum class CanAdapterState {
  Uninitialized,
  Ready,
  DeviceOpen,
  ChannelConfigured,
  ChannelStarted,
  Unsupported,
  Error,
};

enum class CanAdapterErrorCode {
  None,
  Unsupported,
  NotImplemented,
  DriverMissing,
  DeviceNotFound,
  InvalidConfiguration,
  // The vendor API confirmed that none of the requested CAN frames reached
  // the controller.  A shared channel may safely recreate the adapter and
  // retry a single frame once without duplicating a successful transmission.
  TransmitFailedNoFrames,
  VendorError,
};

struct CanAdapterError {
  CanAdapterErrorCode code{CanAdapterErrorCode::None};
  std::string message;
};

struct CanAdapterStatus {
  CanVendor vendor{CanVendor::Other};
  CanAdapterState state{CanAdapterState::Uninitialized};
  bool initialized{};
  bool device_open{};
  bool channel_started{};
};

struct CanDeviceInfo {
  CanVendor vendor{CanVendor::Other};
  std::string id;
  std::string display_name;
  unsigned channel_count{};
};

struct CanChannelConfig {
  std::string device_id;
  unsigned channel{1};
  unsigned nominal_bitrate{500000};
  unsigned data_bitrate{2000000};
  bool can_fd{true};
  std::wstring application_name{L"UDSToolCpp"};
};

class CanAdapterException : public std::runtime_error {
public:
  explicit CanAdapterException(CanAdapterError error)
      : std::runtime_error(error.message), error_(std::move(error)) {}

  [[nodiscard]] const CanAdapterError& error() const noexcept { return error_; }

private:
  CanAdapterError error_;
};

class ICanHardwareAdapter {
public:
  virtual ~ICanHardwareAdapter() = default;

  [[nodiscard]] virtual CanVendor vendor() const noexcept = 0;
  virtual void initialize() = 0;
  virtual void release() noexcept = 0;
  virtual std::vector<CanDeviceInfo> enumerate_devices() = 0;
  virtual void open_device(std::string_view device_id) = 0;
  virtual void close_device() noexcept = 0;
  virtual void configure_channel(const CanChannelConfig& config) = 0;
  virtual void start_channel() = 0;
  virtual void stop_channel() noexcept = 0;
  virtual void send(const CanFrame& frame) = 0;
  [[nodiscard]] virtual bool supports_batch_transmit() const noexcept {
    return false;
  }
  virtual void send_batch(std::span<const CanFrame> frames) {
    for (const auto& frame : frames) send(frame);
  }
  virtual std::optional<CanFrame> receive(
      std::chrono::milliseconds timeout) = 0;
  [[nodiscard]] virtual CanAdapterStatus status() const noexcept = 0;
  [[nodiscard]] virtual CanAdapterError last_error() const = 0;
};

} // namespace uds
