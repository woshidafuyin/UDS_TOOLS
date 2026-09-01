#include "drivers/can/shared_can_bus_provider.hpp"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uds {
namespace {
using namespace std::chrono_literals;

std::string channelKey(const CanChannelConfig& config) {
  return config.device_id + "|" + std::to_string(config.channel) + "|" +
         std::to_string(config.nominal_bitrate) + "|" +
         std::to_string(config.data_bitrate) + "|" +
         std::to_string(config.can_fd);
}

struct Subscriber {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<CanFrame> frames;
  std::exception_ptr receive_failure;
  bool open{};
};

class SharedChannel final : public std::enable_shared_from_this<SharedChannel> {
public:
  SharedChannel(std::shared_ptr<ICanBusProvider> inner, CanChannelConfig config)
      : inner_(std::move(inner)), config_(std::move(config)) {}

  ~SharedChannel() { stop(); }

  std::shared_ptr<Subscriber> subscribe() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    ensureStartedLocked();
    auto subscriber = std::make_shared<Subscriber>();
    subscriber->open = true;
    {
      std::scoped_lock lock(mutex_);
      subscribers_.push_back(subscriber);
    }
    return subscriber;
  }

  void unsubscribe(const std::shared_ptr<Subscriber>& subscriber) noexcept {
    if (!subscriber) return;
    {
      std::scoped_lock subscriber_lock(subscriber->mutex);
      subscriber->open = false;
      subscriber->frames.clear();
    }
    subscriber->condition.notify_all();
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    bool stop_channel{};
    {
      std::scoped_lock lock(mutex_);
      subscribers_.erase(
          std::remove_if(subscribers_.begin(), subscribers_.end(),
                         [&subscriber](const std::weak_ptr<Subscriber>& item) {
                           const auto active = item.lock();
                           return !active || active == subscriber;
                         }),
          subscribers_.end());
      stop_channel = subscribers_.empty();
    }
    if (stop_channel) stopLocked();
  }

  void send(const CanFrame& frame) {
    std::scoped_lock tx_lock(transmit_mutex_);
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    try {
      ensureStartedLocked();
      bus_->send(frame);
    } catch (const CanAdapterException& error) {
      if (error.error().code !=
          CanAdapterErrorCode::TransmitFailedNoFrames) {
        throw;
      }
      // A passive subscriber deliberately keeps this SharedChannel alive.
      // Recreate the underlying vendor channel here so an error-passive ZLG
      // controller is not reused forever.  The adapter confirmed zero frames,
      // therefore retrying this one frame once cannot duplicate a successful
      // transmission.  Never repeat an uncertain or partially sent batch.
      stopLocked();
      ensureStartedLocked();
      bus_->send(frame);
    }
    publishTransmitted(frame);
  }

  void sendBatch(std::span<const CanFrame> frames) {
    if (frames.empty()) return;
    std::scoped_lock tx_lock(transmit_mutex_);
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    try {
      ensureStartedLocked();
      if (bus_->supports_batch_transmit()) bus_->send_batch(frames);
      else for (const auto& frame : frames) bus_->send(frame);
    } catch (const CanAdapterException& error) {
      if (error.error().code ==
          CanAdapterErrorCode::TransmitFailedNoFrames) {
        // The failing position of a multi-frame transfer is not a safe retry
        // boundary.  Reset for the next explicit operation, then preserve the
        // failure for the workflow/report instead of replaying the batch.
        stopLocked();
      }
      throw;
    }
    for (const auto& frame : frames) publishTransmitted(frame);
  }

  bool supportsBatchTransmit() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    ensureStartedLocked();
    return bus_ && bus_->supports_batch_transmit();
  }

  bool isOpen() const noexcept {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::scoped_lock lock(mutex_);
    return !receive_failure_ && bus_ && bus_->is_open();
  }

  void prepareReceive() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    ensureStartedLocked();
  }

private:
  void ensureStartedLocked() {
    bool receive_failed{};
    {
      std::scoped_lock lock(mutex_);
      receive_failed = static_cast<bool>(receive_failure_);
    }
    if (receive_failed) {
      // The reader has already exited. Join it and recreate the vendor channel
      // before accepting another receive or transmit operation.
      stopLocked();
    }
    if (bus_ && bus_->is_open() && reader_.joinable()) return;
    if (!inner_) throw std::runtime_error("shared CAN provider has no inner provider");
    bus_ = inner_->create(config_);
    if (!bus_) throw std::runtime_error("inner CAN provider returned null");
    bus_->open();
    {
      std::scoped_lock lock(mutex_);
      receive_failure_ = nullptr;
    }
    reader_ = std::jthread([this](std::stop_token stop) { receiveLoop(stop); });
  }

  void stop() noexcept {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    stopLocked();
  }

  // lifecycle_mutex_ must be held. Do not hold mutex_ while joining because
  // the reader may be finishing a publish operation that needs mutex_.
  void stopLocked() noexcept {
    if (reader_.joinable()) {
      reader_.request_stop();
      reader_.join();
    }
    if (bus_) bus_->close();
    bus_.reset();
  }

  void receiveLoop(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
      try {
        auto frame = bus_->receive(20ms);
        if (!frame) continue;
        publish(*frame);
      } catch (...) {
        if (!stop.stop_requested()) publishReceiveFailure(std::current_exception());
        return;
      }
    }
  }

  void publishTransmitted(CanFrame frame) {
    frame.transmitted = true;
    publish(frame);
  }

  void publishReceiveFailure(std::exception_ptr failure) noexcept {
    std::vector<std::shared_ptr<Subscriber>> targets;
    {
      std::scoped_lock lock(mutex_);
      receive_failure_ = failure;
      for (auto iterator = subscribers_.begin(); iterator != subscribers_.end();) {
        if (auto subscriber = iterator->lock()) {
          targets.push_back(std::move(subscriber));
          ++iterator;
        } else {
          iterator = subscribers_.erase(iterator);
        }
      }
    }
    for (const auto& subscriber : targets) {
      {
        std::scoped_lock lock(subscriber->mutex);
        if (!subscriber->open) continue;
        subscriber->receive_failure = failure;
      }
      subscriber->condition.notify_all();
    }
  }

  void publish(const CanFrame& frame) {
    std::vector<std::shared_ptr<Subscriber>> targets;
    {
      std::scoped_lock lock(mutex_);
      for (auto iterator = subscribers_.begin(); iterator != subscribers_.end();) {
        if (auto subscriber = iterator->lock()) {
          targets.push_back(std::move(subscriber));
          ++iterator;
        } else {
          iterator = subscribers_.erase(iterator);
        }
      }
    }
    for (const auto& subscriber : targets) {
      std::scoped_lock lock(subscriber->mutex);
      if (!subscriber->open) continue;
      subscriber->frames.push_back(frame);
      constexpr std::size_t kMaximumQueuedFrames = 8192;
      if (subscriber->frames.size() > kMaximumQueuedFrames) {
        subscriber->frames.pop_front();
      }
      subscriber->condition.notify_one();
    }
  }

  std::shared_ptr<ICanBusProvider> inner_;
  CanChannelConfig config_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::mutex transmit_mutex_;
  std::unique_ptr<ICanBus> bus_;
  std::jthread reader_;
  std::exception_ptr receive_failure_;
  std::vector<std::weak_ptr<Subscriber>> subscribers_;
};

class SharedClientBus final : public ICanBus {
public:
  explicit SharedClientBus(std::shared_ptr<SharedChannel> channel)
      : channel_(std::move(channel)) {}
  ~SharedClientBus() override { close(); }

  void open() override {
    if (!subscriber_) subscriber_ = channel_->subscribe();
  }
  void close() noexcept override {
    if (!subscriber_) return;
    channel_->unsubscribe(subscriber_);
    subscriber_.reset();
  }
  bool is_open() const noexcept override {
    return subscriber_ && channel_->isOpen();
  }
  void send(const CanFrame& frame) override { open(); channel_->send(frame); }
  bool supports_batch_transmit() const noexcept override { return false; }
  void send_batch(std::span<const CanFrame> frames) override { open(); channel_->sendBatch(frames); }
  std::optional<CanFrame> receive(std::chrono::milliseconds timeout) override {
    open();
    channel_->prepareReceive();
    std::unique_lock lock(subscriber_->mutex);
    if (!subscriber_->condition.wait_for(lock, timeout, [this] {
          return !subscriber_->frames.empty() || subscriber_->receive_failure ||
                 !subscriber_->open;
        })) return std::nullopt;
    if (subscriber_->receive_failure) {
      const auto failure = subscriber_->receive_failure;
      subscriber_->receive_failure = nullptr;
      lock.unlock();
      std::rethrow_exception(failure);
    }
    if (subscriber_->frames.empty()) return std::nullopt;
    auto frame = std::move(subscriber_->frames.front());
    subscriber_->frames.pop_front();
    return frame;
  }

private:
  std::shared_ptr<SharedChannel> channel_;
  std::shared_ptr<Subscriber> subscriber_;
};
} // namespace

class SharedCanBusProvider::State {
public:
  explicit State(std::shared_ptr<ICanBusProvider> inner) : inner_(std::move(inner)) {}

  std::shared_ptr<SharedChannel> channel(const CanChannelConfig& config) {
    const auto key = channelKey(config);
    std::scoped_lock lock(mutex_);
    if (auto existing = channels_[key].lock()) return existing;
    auto created = std::make_shared<SharedChannel>(inner_, config);
    channels_[key] = created;
    return created;
  }

private:
  std::shared_ptr<ICanBusProvider> inner_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::weak_ptr<SharedChannel>> channels_;
};

SharedCanBusProvider::SharedCanBusProvider(std::shared_ptr<ICanBusProvider> inner)
    : state_(std::make_shared<State>(std::move(inner))) {}
SharedCanBusProvider::~SharedCanBusProvider() = default;

std::unique_ptr<ICanBus> SharedCanBusProvider::create(CanChannelConfig config) const {
  return std::make_unique<SharedClientBus>(state_->channel(config));
}

} // namespace uds
