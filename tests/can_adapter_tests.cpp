#include "drivers/can/can_adapter_factory.hpp"
#include "drivers/can/can_bus_provider.hpp"
#include "drivers/can/can_bus_session.hpp"
#include "drivers/can/shared_can_bus_provider.hpp"
#include "drivers/can/kvaser/kvaser_channel_catalog.hpp"
#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "flash/xizhong_rsmr_flow.hpp"

#include <chrono>
#include <condition_variable>
#include <array>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Trace {
  std::vector<std::string> calls;
  uds::CanChannelConfig config;
  std::vector<uds::CanFrame> sent;
  std::size_t batch_count{};
  std::deque<uds::CanFrame> received;
};

class FakeAdapter final : public uds::ICanHardwareAdapter {
public:
  explicit FakeAdapter(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}

  uds::CanVendor vendor() const noexcept override { return uds::CanVendor::Other; }
  void initialize() override {
    trace_->calls.emplace_back("initialize");
    status_.initialized = true;
    status_.state = uds::CanAdapterState::Ready;
  }
  void release() noexcept override {
    trace_->calls.emplace_back("release");
    status_ = {uds::CanVendor::Other,
               uds::CanAdapterState::Uninitialized};
  }
  std::vector<uds::CanDeviceInfo> enumerate_devices() override {
    trace_->calls.emplace_back("enumerate");
    return {{uds::CanVendor::Other, "fake", "Fake CAN", 1}};
  }
  void open_device(std::string_view) override {
    trace_->calls.emplace_back("open_device");
    status_.device_open = true;
    status_.state = uds::CanAdapterState::DeviceOpen;
  }
  void close_device() noexcept override {
    trace_->calls.emplace_back("close_device");
    status_.device_open = false;
  }
  void configure_channel(const uds::CanChannelConfig& config) override {
    trace_->calls.emplace_back("configure_channel");
    trace_->config = config;
    status_.state = uds::CanAdapterState::ChannelConfigured;
  }
  void start_channel() override {
    trace_->calls.emplace_back("start_channel");
    status_.channel_started = true;
    status_.state = uds::CanAdapterState::ChannelStarted;
  }
  void stop_channel() noexcept override {
    trace_->calls.emplace_back("stop_channel");
    status_.channel_started = false;
  }
  void send(const uds::CanFrame& frame) override {
    trace_->calls.emplace_back("send");
    trace_->sent.push_back(frame);
  }
  void send_batch(std::span<const uds::CanFrame> frames) override {
    trace_->calls.emplace_back("send_batch");
    ++trace_->batch_count;
    trace_->sent.insert(trace_->sent.end(), frames.begin(), frames.end());
  }
  std::optional<uds::CanFrame> receive(std::chrono::milliseconds) override {
    trace_->calls.emplace_back("receive");
    if (trace_->received.empty()) return std::nullopt;
    auto frame = std::move(trace_->received.front());
    trace_->received.pop_front();
    return frame;
  }
  uds::CanAdapterStatus status() const noexcept override { return status_; }
  uds::CanAdapterError last_error() const override { return {}; }

private:
  std::shared_ptr<Trace> trace_;
  uds::CanAdapterStatus status_{uds::CanVendor::Other,
                                uds::CanAdapterState::Uninitialized};
};

void test_session_lifecycle() {
  const auto trace = std::make_shared<Trace>();
  trace->received.push_back(
      {0x456, {0x50, 0x01}, false, false, false});
  uds::CanBusSession session(
      std::make_unique<FakeAdapter>(trace),
      {"fake", 3, 500000, 2000000, true, L"AdapterTest"});
  session.open();
  session.send({0x123, {1, 2, 3}, false, true, true});
  const std::array batch{
      uds::CanFrame{0x124, {4, 5}, false, false, false},
      uds::CanFrame{0x125, {6, 7}, false, false, false},
  };
  session.send_batch(batch);
  const auto received = session.receive(std::chrono::milliseconds(1));
  check(session.is_open() && received && received->id == 0x456,
        "CAN bus session did not delegate send/receive");
  check(trace->config.channel == 3 && trace->config.can_fd &&
            trace->config.application_name == L"AdapterTest" &&
            trace->sent.size() == 3 && trace->batch_count == 1,
        "CAN bus session changed channel configuration or frame data");
  check(trace->calls.size() >= 6 && trace->calls[0] == "initialize" &&
            trace->calls[1] == "open_device" &&
            trace->calls[2] == "configure_channel" &&
            trace->calls[3] == "start_channel",
        "CAN adapter lifecycle order is wrong");
  session.close();
  check(!session.is_open(), "CAN bus session did not close");
}

class BatchObservingBus final : public uds::ICanBus {
public:
  void open() override { open_ = true; }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }
  void send(const uds::CanFrame& frame) override {
    individual_frames.push_back(frame);
  }
  void send_batch(std::span<const uds::CanFrame> frames) override {
    batches.emplace_back(frames.begin(), frames.end());
  }
  std::optional<uds::CanFrame> receive(std::chrono::milliseconds) override {
    if (received.empty()) return std::nullopt;
    auto frame = std::move(received.front());
    received.pop_front();
    return frame;
  }

  bool open_{true};
  std::vector<uds::CanFrame> individual_frames;
  std::vector<std::vector<uds::CanFrame>> batches;
  std::deque<uds::CanFrame> received;
};

void test_isotp_zero_stmin_batches_consecutive_frames() {
  BatchObservingBus bus;
  bus.received.push_back(
      {0x18DAF1B7, {0x30, 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
       true, false, false});
  uds::IsoTpSession transport(
      bus,
      {0x18DAB7F1, 0x18DAF1B7, 0xCC, 0, 0,
       std::chrono::milliseconds(1000), std::chrono::milliseconds(1000),
       true, true, true, true, true, true});
  std::vector<std::uint8_t> payload(1026);
  payload[0] = 0x36;
  payload[1] = 0x01;
  for (std::size_t index = 2; index < payload.size(); ++index) {
    payload[index] = static_cast<std::uint8_t>(index & 0xFFU);
  }

  transport.send(payload);

  check(bus.individual_frames.size() == 1 &&
            bus.individual_frames.front().fd &&
            bus.individual_frames.front().brs &&
            bus.individual_frames.front().data ==
                std::vector<std::uint8_t>(
                    {0x14, 0x02, 0x36, 0x01, 0x02, 0x03, 0x04, 0x05}),
        "ISO-TP batched transfer changed the CAN FD first frame");
  check(bus.batches.size() == 1 && bus.batches.front().size() == 146,
        "ISO-TP STmin=0 transfer was not submitted as one CF batch");
  const auto& first_cf = bus.batches.front().front();
  const auto& last_cf = bus.batches.front().back();
  check(!first_cf.fd && !first_cf.brs && first_cf.data[0] == 0x21 &&
            last_cf.data[0] == 0x22,
        "ISO-TP batch lost FlowControl format adaptation or sequence order");

  std::vector<std::uint8_t> rebuilt(
      bus.individual_frames.front().data.begin() + 2,
      bus.individual_frames.front().data.end());
  for (const auto& frame : bus.batches.front()) {
    rebuilt.insert(rebuilt.end(), frame.data.begin() + 1, frame.data.end());
  }
  rebuilt.resize(payload.size());
  check(rebuilt == payload,
        "ISO-TP batched consecutive frames changed the UDS payload");
}

void test_xizhong_probe_through_adapter() {
  const auto trace = std::make_shared<Trace>();
  trace->received.push_back(
      {0x18DAF1B7, {0x02, 0x50, 0x01, 0, 0, 0, 0, 0},
       true, false, false});
  uds::CanBusSession session(
      std::make_unique<FakeAdapter>(trace),
      {"fake", 1, 500000, 2000000, true, L"XizhongParityTest"});
  uds::IsoTpSession transport(
      session,
      {0x18DAB7F1, 0x18DAF1B7, uds::kXizhongPhysicalPadding, 0, 0,
       std::chrono::milliseconds(1000), std::chrono::milliseconds(1000),
       true, true, true, true});
  uds::UdsClient client(transport);
  const auto result = client.request(
      std::array<std::uint8_t, 2>{0x10, 0x01},
      std::chrono::milliseconds(10));
  check(result.success && result.response ==
                                std::vector<std::uint8_t>({0x50, 0x01}),
        "Xizhong successful probe response was not replayed through adapter");
  check(trace->sent.size() == 1 && trace->sent[0].id == 0x18DAB7F1 &&
            trace->sent[0].extended && trace->sent[0].fd &&
            trace->sent[0].brs &&
            trace->sent[0].data ==
                std::vector<std::uint8_t>({0x02, 0x10, 0x01, 0xCC,
                                           0xCC, 0xCC, 0xCC, 0xCC}),
        "Xizhong adapter path changed ID, FD/BRS or physical padding");
}

void test_factory_contract() {
  auto other = uds::CanAdapterFactory::create(uds::CanVendor::Other);
  check(other &&
            other->status().state == uds::CanAdapterState::Unsupported,
        "Other adapter did not report Unsupported");

  auto tosun = uds::CanAdapterFactory::create(uds::CanVendor::Tosun);
  check(tosun && tosun->vendor() == uds::CanVendor::Tosun,
        "TOSUN factory selection failed");
#if UDS_ENABLE_TOSUN
  check(tosun->status().state == uds::CanAdapterState::Uninitialized,
        "Enabled TOSUN adapter did not expose its runtime implementation");
#else
  check(tosun->status().state == uds::CanAdapterState::Unsupported,
        "Disabled TOSUN adapter did not report Unsupported");
#endif

  auto zlg = uds::CanAdapterFactory::create(uds::CanVendor::Zlg);
  check(zlg && zlg->vendor() == uds::CanVendor::Zlg,
        "ZLG factory selection failed");
#if UDS_ENABLE_ZLG
  check(zlg->status().state == uds::CanAdapterState::Uninitialized,
        "Enabled ZLG adapter did not expose its runtime implementation");
#else
  check(zlg->status().state == uds::CanAdapterState::Unsupported,
        "Disabled ZLG adapter did not report Unsupported");
#endif

  auto kvaser = uds::CanAdapterFactory::create(uds::CanVendor::Kvaser);
  check(kvaser && kvaser->vendor() == uds::CanVendor::Kvaser,
        "Kvaser factory selection failed");
#if UDS_ENABLE_KVASER
  check(kvaser->status().state == uds::CanAdapterState::Uninitialized,
        "Enabled Kvaser adapter did not expose its runtime implementation");
#else
  check(kvaser->status().state == uds::CanAdapterState::Unsupported,
        "Disabled Kvaser adapter did not report Unsupported");
#endif

  auto vector = uds::CanAdapterFactory::create(uds::CanVendor::Vector);
  check(vector && vector->vendor() == uds::CanVendor::Vector,
        "Vector factory selection failed");
#if UDS_ENABLE_VECTOR
  vector->initialize();
  const auto devices = vector->enumerate_devices();
  check(devices.size() == 1 && devices[0].id == "vector-xl-driver",
        "Vector adapter did not expose its compatibility driver endpoint");
  vector->release();
#else
  check(vector->status().state == uds::CanAdapterState::Unsupported,
        "Disabled Vector adapter did not report Unsupported");
#endif

  uds::set_default_can_vendor(uds::CanVendor::Zlg);
  check(uds::default_can_vendor() == uds::CanVendor::Zlg &&
            uds::can_vendor_name(uds::CanVendor::Zlg).find("ZCAN") !=
                std::string_view::npos,
        "Selected CAN backend did not retain the ZLG setting");
  uds::set_default_can_vendor(uds::CanVendor::Vector);
  check(uds::can_vendor_name(uds::CanVendor::Kvaser).find("CANlib") !=
            std::string_view::npos,
        "Kvaser backend display name lost its CANlib identity");
}

void test_kvaser_physical_channels_precede_virtual_channels() {
  std::vector<uds::detail::KvaserChannelCatalogEntry> channels(4);
  channels[0].api_index = 0;
  channels[0].virtual_channel = true;
  channels[1].api_index = 1;
  channels[1].virtual_channel = true;
  channels[2].api_index = 2;
  channels[2].virtual_channel = false;
  channels[3].api_index = 3;
  channels[3].virtual_channel = false;

  uds::detail::order_kvaser_channels(channels);
  check(channels[0].api_index == 2 && channels[1].api_index == 3 &&
            channels[2].api_index == 0 && channels[3].api_index == 1,
        "Kvaser logical channel order did not put physical hardware first");
}

struct SharedBusTrace {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<uds::CanFrame> incoming;
  std::vector<uds::CanFrame> sent;
  unsigned created{};
  unsigned closed{};
  bool fail_first_send_without_frames{};
  bool fail_next_receive{};
  bool block_receive{};
  bool reader_waiting{};
  bool release_receive{};
};

class SharedTestBus final : public uds::ICanBus {
public:
  explicit SharedTestBus(std::shared_ptr<SharedBusTrace> trace)
      : trace_(std::move(trace)) {}
  void open() override { open_ = true; }
  void close() noexcept override {
    open_ = false;
    std::scoped_lock lock(trace_->mutex);
    ++trace_->closed;
  }
  bool is_open() const noexcept override { return open_; }
  void send(const uds::CanFrame& frame) override {
    std::scoped_lock lock(trace_->mutex);
    if (trace_->fail_first_send_without_frames) {
      trace_->fail_first_send_without_frames = false;
      throw uds::CanAdapterException(
          {uds::CanAdapterErrorCode::TransmitFailedNoFrames,
           "fake adapter transmitted zero CAN frames"});
    }
    trace_->sent.push_back(frame);
  }
  std::optional<uds::CanFrame> receive(std::chrono::milliseconds timeout) override {
    std::unique_lock lock(trace_->mutex);
    if (trace_->block_receive && !trace_->release_receive) {
      trace_->reader_waiting = true;
      trace_->condition.notify_all();
      trace_->condition.wait(lock,
                             [this] { return trace_->release_receive; });
    }
    if (trace_->fail_next_receive) {
      trace_->fail_next_receive = false;
      throw std::runtime_error("fake CAN receive failure");
    }
    trace_->condition.wait_for(lock, timeout,
                               [this] { return !trace_->incoming.empty(); });
    if (trace_->incoming.empty()) return std::nullopt;
    auto frame = std::move(trace_->incoming.front());
    trace_->incoming.pop_front();
    return frame;
  }

private:
  std::shared_ptr<SharedBusTrace> trace_;
  bool open_{};
};

class SharedTestProvider final : public uds::ICanBusProvider {
public:
  explicit SharedTestProvider(std::shared_ptr<SharedBusTrace> trace)
      : trace_(std::move(trace)) {}
  std::unique_ptr<uds::ICanBus> create(uds::CanChannelConfig) const override {
    std::scoped_lock lock(trace_->mutex);
    ++trace_->created;
    return std::make_unique<SharedTestBus>(trace_);
  }

private:
  std::shared_ptr<SharedBusTrace> trace_;
};

void test_shared_provider_fans_out_without_consuming_diagnostic_frames() {
  const auto trace = std::make_shared<SharedBusTrace>();
  auto provider = std::make_shared<uds::SharedCanBusProvider>(
      std::make_shared<SharedTestProvider>(trace));
  const uds::CanChannelConfig config{"", 1, 500000, 2000000, true, L"Test"};
  auto trace_subscriber = provider->create(config);
  auto diagnostic_session = provider->create(config);
  trace_subscriber->open();
  diagnostic_session->open();
  const uds::CanFrame expected{0x77A, {0x02, 0x50, 0x01, 0, 0, 0, 0, 0}, false, false, false};
  {
    std::scoped_lock lock(trace->mutex);
    trace->incoming.push_back(expected);
  }
  trace->condition.notify_one();
  const auto trace_frame = trace_subscriber->receive(std::chrono::milliseconds(300));
  const auto diagnostic_frame = diagnostic_session->receive(std::chrono::milliseconds(300));
  check(trace_frame && diagnostic_frame && trace_frame->data == expected.data &&
            diagnostic_frame->data == expected.data && trace->created == 1,
        "shared provider did not fan out one hardware frame to trace and diagnostics");
  diagnostic_session->send({0x772, {0x02, 0x10, 0x01}, false, false, false});
  const auto transmitted_frame = trace_subscriber->receive(std::chrono::milliseconds(300));
  check(trace->sent.size() == 1 && trace->sent.front().id == 0x772 &&
            transmitted_frame && transmitted_frame->id == 0x772 &&
            transmitted_frame->transmitted,
        "shared provider did not serialize and publish diagnostic transmit");
}

void test_shared_provider_recovers_zero_frame_transmit_with_listener_alive() {
  const auto trace = std::make_shared<SharedBusTrace>();
  trace->fail_first_send_without_frames = true;
  auto provider = std::make_shared<uds::SharedCanBusProvider>(
      std::make_shared<SharedTestProvider>(trace));
  const uds::CanChannelConfig config{"", 2, 500000, 2000000, true,
                                     L"RecoveryTest"};
  auto passive_listener = provider->create(config);
  auto diagnostic_session = provider->create(config);
  passive_listener->open();
  diagnostic_session->open();

  diagnostic_session->send(
      {0x72E, {0x02, 0x10, 0x03}, false, false, false});
  const auto observed =
      passive_listener->receive(std::chrono::milliseconds(300));

  std::scoped_lock lock(trace->mutex);
  check(trace->created == 2 && trace->closed >= 1,
        "shared provider did not recreate a zero-frame faulted channel");
  check(trace->sent.size() == 1 && trace->sent.front().id == 0x72E &&
            observed && observed->transmitted && observed->id == 0x72E,
        "shared provider recovery duplicated or lost the retried frame");
}

void test_shared_provider_surfaces_receive_failure_and_recovers() {
  const auto trace = std::make_shared<SharedBusTrace>();
  auto provider = std::make_shared<uds::SharedCanBusProvider>(
      std::make_shared<SharedTestProvider>(trace));
  const uds::CanChannelConfig config{"", 3, 500000, 2000000, true,
                                     L"ReceiveRecoveryTest"};
  auto session = provider->create(config);
  session->open();
  {
    std::scoped_lock lock(trace->mutex);
    trace->fail_next_receive = true;
  }
  trace->condition.notify_all();

  bool failure_observed{};
  try {
    (void)session->receive(std::chrono::milliseconds(300));
  } catch (const std::runtime_error& error) {
    failure_observed = std::string(error.what()) == "fake CAN receive failure";
  }
  check(failure_observed,
        "shared provider hid the hardware receive-loop failure");

  const uds::CanFrame expected{
      0x77A, {0x02, 0x50, 0x03, 0, 0, 0, 0, 0}, false, false, false};
  {
    std::scoped_lock lock(trace->mutex);
    trace->incoming.push_back(expected);
  }
  trace->condition.notify_all();
  const auto recovered = session->receive(std::chrono::milliseconds(300));
  {
    std::scoped_lock lock(trace->mutex);
    check(trace->created == 2 && trace->closed >= 1,
          "shared provider did not recreate a receive-faulted channel");
  }
  check(recovered && recovered->id == expected.id &&
            recovered->data == expected.data,
        "shared provider did not receive after channel recreation");
}

void test_shared_provider_does_not_close_a_concurrent_new_subscriber() {
  const auto trace = std::make_shared<SharedBusTrace>();
  trace->block_receive = true;
  auto provider = std::make_shared<uds::SharedCanBusProvider>(
      std::make_shared<SharedTestProvider>(trace));
  const uds::CanChannelConfig config{"", 4, 500000, 2000000, true,
                                     L"ConcurrentSubscriberTest"};
  auto first = provider->create(config);
  auto second = provider->create(config);
  first->open();
  {
    std::unique_lock lock(trace->mutex);
    check(trace->condition.wait_for(
              lock, std::chrono::milliseconds(300),
              [&trace] { return trace->reader_waiting; }),
          "fake receive loop did not enter its blocking point");
  }

  std::thread closing([&first] { first->close(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::thread opening([&second] { second->open(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::scoped_lock lock(trace->mutex);
    trace->release_receive = true;
  }
  trace->condition.notify_all();
  closing.join();
  opening.join();

  check(second->is_open(),
        "last unsubscribe closed a concurrently added subscriber");
  std::scoped_lock lock(trace->mutex);
  check(trace->created == 2 && trace->closed >= 1,
        "concurrent subscriber did not reopen after the old channel stopped");
}

} // namespace

int main() {
  try {
    const auto run = [](const char* name, auto test) {
      std::cout << "RUN " << name << std::endl;
      test();
    };
    run("session_lifecycle", test_session_lifecycle);
    run("isotp_zero_stmin_batches_consecutive_frames",
        test_isotp_zero_stmin_batches_consecutive_frames);
    run("xizhong_probe_through_adapter",
        test_xizhong_probe_through_adapter);
    run("factory_contract", test_factory_contract);
    run("kvaser_physical_channels_precede_virtual_channels",
        test_kvaser_physical_channels_precede_virtual_channels);
    run("shared_provider_fans_out_without_consuming_diagnostic_frames",
        test_shared_provider_fans_out_without_consuming_diagnostic_frames);
    run("shared_provider_recovers_zero_frame_transmit_with_listener_alive",
        test_shared_provider_recovers_zero_frame_transmit_with_listener_alive);
    run("shared_provider_surfaces_receive_failure_and_recovers",
        test_shared_provider_surfaces_receive_failure_and_recovers);
    run("shared_provider_does_not_close_a_concurrent_new_subscriber",
        test_shared_provider_does_not_close_a_concurrent_new_subscriber);
    std::cout << "can_adapter_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "can_adapter_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
