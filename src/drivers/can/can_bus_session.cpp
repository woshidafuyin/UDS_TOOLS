#include "drivers/can/can_bus_session.hpp"

#include <stdexcept>
#include <utility>

namespace uds {

CanBusSession::CanBusSession(std::unique_ptr<ICanHardwareAdapter> adapter,
                             CanChannelConfig config)
    : adapter_(std::move(adapter)), config_(std::move(config)) {
  if (!adapter_) throw std::invalid_argument("CAN adapter must not be null");
}

CanBusSession::~CanBusSession() { close(); }

void CanBusSession::open() {
  std::scoped_lock lock(lifecycle_mutex_);
  if (adapter_->status().channel_started) return;
  try {
    adapter_->initialize();
    adapter_->open_device(config_.device_id);
    adapter_->configure_channel(config_);
    adapter_->start_channel();
  } catch (...) {
    adapter_->stop_channel();
    adapter_->close_device();
    adapter_->release();
    throw;
  }
}

void CanBusSession::close() noexcept {
  std::scoped_lock lock(lifecycle_mutex_);
  if (!adapter_) return;
  adapter_->stop_channel();
  adapter_->close_device();
  adapter_->release();
}

bool CanBusSession::is_open() const noexcept {
  std::scoped_lock lock(lifecycle_mutex_);
  return adapter_ && adapter_->status().channel_started;
}

void CanBusSession::send(const CanFrame& frame) {
  std::scoped_lock transmit_lock(transmit_mutex_);
  open();
  adapter_->send(frame);
}

bool CanBusSession::supports_batch_transmit() const noexcept {
  return adapter_ && adapter_->supports_batch_transmit();
}

void CanBusSession::send_batch(std::span<const CanFrame> frames) {
  if (frames.empty()) return;
  std::scoped_lock transmit_lock(transmit_mutex_);
  open();
  adapter_->send_batch(frames);
}

std::optional<CanFrame> CanBusSession::receive(
    std::chrono::milliseconds timeout) {
  open();
  return adapter_->receive(timeout);
}

CanVendor CanBusSession::vendor() const noexcept { return adapter_->vendor(); }

CanAdapterError CanBusSession::last_error() const {
  return adapter_->last_error();
}

} // namespace uds
