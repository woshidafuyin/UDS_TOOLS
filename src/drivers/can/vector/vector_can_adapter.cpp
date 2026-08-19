#include "drivers/can/vector/vector_can_adapter.hpp"

#include "core/vector_xl_bus.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace uds {

VectorCanAdapter::VectorCanAdapter() = default;
VectorCanAdapter::~VectorCanAdapter() { release(); }

CanVendor VectorCanAdapter::vendor() const noexcept { return CanVendor::Vector; }

void VectorCanAdapter::initialize() {
  std::scoped_lock lock(state_mutex_);
  if (status_.initialized) return;
  status_ = {CanVendor::Vector, CanAdapterState::Ready, true, false, false};
  last_error_ = {};
}

void VectorCanAdapter::release() noexcept {
  stop_channel();
  std::scoped_lock lock(state_mutex_);
  bus_.reset();
  status_ = {CanVendor::Vector, CanAdapterState::Uninitialized, false, false,
             false};
}

std::vector<CanDeviceInfo> VectorCanAdapter::enumerate_devices() {
  initialize();
  // VectorXlBus intentionally keeps the passing one-based physical-channel
  // mapping. Expose that driver endpoint without adding a second XL API path.
  return {{CanVendor::Vector, "vector-xl-driver", "Vector XL Driver", 64}};
}

void VectorCanAdapter::open_device(std::string_view device_id) {
  initialize();
  if (!device_id.empty() && device_id != "vector-xl-driver") {
    fail(CanAdapterErrorCode::DeviceNotFound,
         "Vector device id is not available: " + std::string(device_id));
  }
  std::scoped_lock lock(state_mutex_);
  status_.device_open = true;
  status_.state = CanAdapterState::DeviceOpen;
}

void VectorCanAdapter::close_device() noexcept {
  stop_channel();
  std::scoped_lock lock(state_mutex_);
  bus_.reset();
  status_.device_open = false;
  if (status_.initialized) status_.state = CanAdapterState::Ready;
}

void VectorCanAdapter::configure_channel(const CanChannelConfig& config) {
  if (!status().device_open) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Vector device must be opened before channel configuration");
  }
  if (config.channel < 1 || config.channel > 64 ||
      config.nominal_bitrate == 0 ||
      (config.can_fd && config.data_bitrate == 0)) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Vector CAN channel configuration is invalid");
  }
  std::scoped_lock lock(state_mutex_);
  bus_ = std::make_unique<VectorXlBus>(VectorBusConfig{
      config.channel, config.nominal_bitrate, config.application_name,
      config.can_fd, config.data_bitrate});
  status_.state = CanAdapterState::ChannelConfigured;
  status_.channel_started = false;
  last_error_ = {};
}

void VectorCanAdapter::start_channel() {
  VectorXlBus* bus{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) return;
    bus = bus_.get();
  }
  if (!bus) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Vector channel must be configured before start");
  }
  try {
    bus->open();
    std::scoped_lock lock(state_mutex_);
    status_.state = CanAdapterState::ChannelStarted;
    status_.channel_started = true;
    last_error_ = {};
  } catch (const std::exception& error) {
    remember_error(CanAdapterErrorCode::VendorError, error.what());
    throw;
  }
}

void VectorCanAdapter::stop_channel() noexcept {
  VectorXlBus* bus{};
  {
    std::scoped_lock lock(state_mutex_);
    bus = bus_.get();
  }
  if (bus) bus->close();
  std::scoped_lock lock(state_mutex_);
  status_.channel_started = false;
  if (status_.device_open) status_.state = CanAdapterState::ChannelConfigured;
}

void VectorCanAdapter::send(const CanFrame& frame) {
  VectorXlBus* bus{};
  {
    std::scoped_lock lock(state_mutex_);
    bus = bus_.get();
  }
  if (!bus || !status().channel_started) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Vector channel is not started");
  }
  try {
    bus->send(frame);
  } catch (const std::exception& error) {
    remember_error(CanAdapterErrorCode::VendorError, error.what());
    throw;
  }
}

std::optional<CanFrame> VectorCanAdapter::receive(
    std::chrono::milliseconds timeout) {
  VectorXlBus* bus{};
  {
    std::scoped_lock lock(state_mutex_);
    bus = bus_.get();
  }
  if (!bus || !status().channel_started) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Vector channel is not started");
  }
  try {
    return bus->receive(timeout);
  } catch (const std::exception& error) {
    remember_error(CanAdapterErrorCode::VendorError, error.what());
    throw;
  }
}

CanAdapterStatus VectorCanAdapter::status() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return status_;
}

CanAdapterError VectorCanAdapter::last_error() const {
  std::scoped_lock lock(state_mutex_);
  return last_error_;
}

[[noreturn]] void VectorCanAdapter::fail(CanAdapterErrorCode code,
                                         std::string message) {
  remember_error(code, message);
  throw CanAdapterException({code, std::move(message)});
}

void VectorCanAdapter::remember_error(CanAdapterErrorCode code,
                                      std::string message) noexcept {
  try {
    std::scoped_lock lock(state_mutex_);
    last_error_ = {code, std::move(message)};
    status_.state = CanAdapterState::Error;
  } catch (...) {
  }
}

} // namespace uds
