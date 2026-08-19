#include "app/probe_service.hpp"
#include "core/hex.hpp"
#include "core/profile.hpp"
#include "drivers/can/can_adapter_factory.hpp"
#include "drivers/can/can_bus_provider.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace std::chrono_literals;

struct Options {
  uds::CanVendor vendor{uds::CanVendor::Vector};
  std::filesystem::path profile;
  std::filesystem::path trace;
  unsigned channel{};
  unsigned passive_ms{2000};
  bool active{};
};

uds::CanVendor parse_vendor(std::string_view value) {
  if (value == "vector") return uds::CanVendor::Vector;
  if (value == "tosun" || value == "tsmaster") return uds::CanVendor::Tosun;
  if (value == "zlg" || value == "zcan" || value == "zcanpro") {
    return uds::CanVendor::Zlg;
  }
  if (value == "kvaser" || value == "canlib") {
    return uds::CanVendor::Kvaser;
  }
  throw std::invalid_argument(
      "vendor must be vector, zlg, tosun, or kvaser");
}

unsigned parse_unsigned(std::string_view value, const char* option) {
  std::size_t consumed{};
  const auto parsed = std::stoul(std::string(value), &consumed, 10);
  if (consumed != value.size() || parsed > 0xFFFFFFFFUL) {
    throw std::invalid_argument(std::string("invalid ") + option);
  }
  return static_cast<unsigned>(parsed);
}

Options parse_options(int argc, char* argv[]) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    const auto value = [&](const char* name) -> std::string_view {
      if (++index >= argc) {
        throw std::invalid_argument(std::string(name) + " requires a value");
      }
      return argv[index];
    };
    if (option == "--vendor") {
      options.vendor = parse_vendor(value("--vendor"));
    } else if (option == "--profile") {
      options.profile = std::filesystem::path(std::string(value("--profile")));
    } else if (option == "--channel") {
      options.channel = parse_unsigned(value("--channel"), "--channel");
    } else if (option == "--passive-ms") {
      options.passive_ms =
          parse_unsigned(value("--passive-ms"), "--passive-ms");
    } else if (option == "--trace") {
      options.trace = std::filesystem::path(std::string(value("--trace")));
    } else if (option == "--active") {
      options.active = true;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  if (options.profile.empty()) {
    throw std::invalid_argument("--profile is required");
  }
  return options;
}

void print_frame(const uds::CanFrame& frame) {
  std::cout << "RX id=0x" << std::hex << std::uppercase << frame.id << std::dec
            << (frame.extended ? " EXT" : " STD")
            << (frame.fd ? " FD" : " CAN") << (frame.brs ? " BRS" : "")
            << " len=" << frame.data.size()
            << " data=" << uds::to_hex(frame.data) << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    uds::set_default_can_vendor(options.vendor);
    auto profile = uds::load_profile_ini(options.profile);
    const auto channel = options.channel == 0 ? profile.channel : options.channel;
    std::cout << "BACKEND=" << uds::can_vendor_name(options.vendor) << '\n'
              << "PROFILE=" << options.profile.string() << '\n'
              << "CHANNEL=" << channel << '\n'
              << "BITRATE=" << profile.nominal_bitrate << '/'
              << profile.data_bitrate << '\n';

    auto adapter = uds::CanAdapterFactory::create(options.vendor);
    adapter->initialize();
    const auto devices = adapter->enumerate_devices();
    std::cout << "DEVICE_COUNT=" << devices.size() << '\n';
    for (const auto& device : devices) {
      std::cout << "DEVICE id=" << device.id << " name=" << device.display_name
                << " channels=" << device.channel_count << '\n';
    }
    adapter->release();
    if (devices.empty()) {
      std::cerr << "ERROR=no matching CAN device found\n";
      return 3;
    }

    const auto provider =
        std::make_shared<uds::DefaultCanBusProvider>(options.vendor);
    if (options.passive_ms > 0) {
      auto bus = provider->create(
          {"", channel, profile.nominal_bitrate, profile.data_bitrate,
           profile.can_fd, L"UDSToolHardwareProbe"});
      bus->open();
      std::cout << "PASSIVE_LISTEN_MS=" << options.passive_ms << '\n';
      const auto passive_deadline =
          std::chrono::steady_clock::now() +
          std::chrono::milliseconds(options.passive_ms);
      unsigned received{};
      while (std::chrono::steady_clock::now() < passive_deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                passive_deadline - std::chrono::steady_clock::now());
        const auto frame = bus->receive(std::min(remaining, 20ms));
        if (!frame) continue;
        ++received;
        if (received <= 50) print_frame(*frame);
      }
      bus->close();
      std::cout << "PASSIVE_RX_COUNT=" << received << '\n';
    } else {
      std::cout << "PASSIVE_LISTEN=SKIPPED\n";
    }
    if (!options.active) {
      std::cout << "ACTIVE_PROBE=SKIPPED\n";
      return 0;
    }

    uds::app::ProbeService service(
        [provider](const uds::app::ProbeRequest& request) {
          return provider->create(
              {"", request.channel, request.nominal_bitrate,
               request.data_bitrate, request.profile.can_fd,
               L"UDSToolHardwareProbe"});
        });
    uds::app::ProbeRequest request;
    request.profile = profile;
    request.entry_mode = L"app";
    request.channel = channel;
    request.tx_id = profile.tx_id;
    request.rx_id = profile.rx_id;
    request.nominal_bitrate = profile.nominal_bitrate;
    request.data_bitrate = profile.data_bitrate;
    request.padding = profile.padding;
    request.trace_file = options.trace;
    const auto result = service.run(
        request,
        {[](const std::string& line) { std::cout << "LOG=" << line << '\n'; },
         [](int percent, const std::string& line) {
           std::cout << "PROGRESS=" << percent << ';' << line << '\n';
         }},
        std::stop_token{});
    std::cout << "ACTIVE_PROBE=" << (result.success ? "PASS" : "FAIL") << '\n'
              << "RESULT=" << result.message << '\n';
    return result.success ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "ERROR=" << error.what() << '\n';
    return 4;
  }
}
