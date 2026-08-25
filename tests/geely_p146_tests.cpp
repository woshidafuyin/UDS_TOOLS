#include "core/isotp.hpp"
#include "core/profile.hpp"
#include "core/uds_client.hpp"
#include "flash/flash_workflow.hpp"
#include "flash/geely_geea2_flow.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class ScriptedGeea2Bus final : public uds::ICanBus {
public:
  void open() override { open_ = true; }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }

  void send(const uds::CanFrame& frame) override {
    std::lock_guard lock(mutex_);
    if (frame.id != 0x7E3U || frame.data.empty()) {
      throw std::runtime_error("unexpected GEEA2 fake request frame");
    }
    const auto type = frame.data[0] >> 4U;
    if (type == 0U) {
      const auto length = static_cast<std::size_t>(frame.data[0] & 0x0FU);
      handle({frame.data.begin() + 1,
              frame.data.begin() + 1 + static_cast<std::ptrdiff_t>(length)});
    } else if (type == 1U) {
      const auto length = static_cast<std::size_t>(
          ((frame.data[0] & 0x0FU) << 8U) | frame.data[1]);
      transfer_ = {length, {frame.data.begin() + 2, frame.data.end()}};
      reply_raw({0x30, 0x00, 0x00});
    } else if (type == 2U) {
      transfer_.data.insert(transfer_.data.end(), frame.data.begin() + 1,
                            frame.data.end());
      if (transfer_.data.size() >= transfer_.length) {
        transfer_.data.resize(transfer_.length);
        auto complete = std::move(transfer_.data);
        transfer_ = {};
        handle(std::move(complete));
      }
    }
  }

  std::optional<uds::CanFrame> receive(std::chrono::milliseconds) override {
    std::lock_guard lock(mutex_);
    if (rx_.empty()) return std::nullopt;
    auto frame = std::move(rx_.front());
    rx_.pop_front();
    return frame;
  }

  const std::vector<std::vector<std::uint8_t>>& requests() const {
    return requests_;
  }

private:
  struct Transfer {
    std::size_t length{};
    std::vector<std::uint8_t> data;
  };

  void reply(std::initializer_list<std::uint8_t> payload) {
    std::vector<std::uint8_t> data(8U, 0x55);
    data[0] = static_cast<std::uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), data.begin() + 1);
    rx_.push_back({0x7EB, std::move(data), false, false, false});
  }

  void reply_raw(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::uint8_t> data(8U, 0x55);
    std::copy(bytes.begin(), bytes.end(), data.begin());
    rx_.push_back({0x7EB, std::move(data), false, false, false});
  }

  void handle(std::vector<std::uint8_t> payload) {
    requests_.push_back(payload);
    if (payload.size() == 2U && payload[0] == 0x10U) {
      reply({0x50, payload[1], 0x00, 0x32, 0x01, 0xF4});
    } else if (payload ==
               std::vector<std::uint8_t>{0x31, 0x01, 0x02, 0x06}) {
      reply({0x71, 0x01, 0x02, 0x06, 0x10, 0x00});
    } else if (payload == std::vector<std::uint8_t>{0x27, 0x01}) {
      reply({0x67, 0x01, 0x12, 0x34, 0x56});
    } else if (payload ==
               std::vector<std::uint8_t>{0x27, 0x02, 0xAA, 0xBB, 0xCC}) {
      reply({0x67, 0x02});
    } else if (payload.size() == 12U && payload[0] == 0x31U &&
               payload[2] == 0xFFU) {
      reply({0x71, 0x01, 0xFF, 0x00, 0x10});
    } else if (payload.size() == 11U && payload[0] == 0x34U) {
      reply({0x74, 0x20, 0x01, 0x00});
    } else if (payload.size() >= 2U && payload[0] == 0x36U) {
      reply({0x76, payload[1]});
    } else if (payload == std::vector<std::uint8_t>{0x37}) {
      reply({0x77});
    } else if (payload.size() >= 4U && payload[0] == 0x31U &&
               payload[2] == 0x02U && payload[3] == 0x12U) {
      reply({0x71, 0x01, 0x02, 0x12, 0x10, 0x00});
    } else if (payload.size() == 8U && payload[0] == 0x31U &&
               payload[2] == 0x03U && payload[3] == 0x01U) {
      reply({0x71, 0x01, 0x03, 0x01, 0x10});
    } else if (payload ==
               std::vector<std::uint8_t>{0x31, 0x01, 0x02, 0x05}) {
      reply({0x71, 0x01, 0x02, 0x05, 0x10, 0x00});
    } else if (payload == std::vector<std::uint8_t>{0x19, 0x02, 0x08}) {
      reply({0x59, 0x02, 0xFF});
    } else if (payload == std::vector<std::uint8_t>{0x11, 0x01}) {
      reply({0x51, 0x01});
    } else {
      throw std::runtime_error("unexpected GEEA2 fake UDS request");
    }
  }

  bool open_{true};
  mutable std::mutex mutex_;
  Transfer transfer_;
  std::deque<uds::CanFrame> rx_;
  std::vector<std::vector<std::uint8_t>> requests_;
};

uds::VbfFile make_sbl() {
  uds::VbfFile result;
  result.sw_part_type = "SBL";
  result.data_format_identifier = 0x00;
  result.has_call_address = true;
  result.call_address = 0x00430000;
  result.has_ecu_address = true;
  result.ecu_address = 0x1BB3;
  result.signature_prod = {0x10, 0x20};
  result.signature = result.signature_prod;
  result.blocks.push_back({0x00430000, {0x01, 0x02, 0x03}, 0});
  return result;
}

uds::VbfFile make_app() {
  uds::VbfFile result;
  result.sw_part_type = "DATA";
  result.data_format_identifier = 0x10;
  result.has_ecu_address = true;
  result.ecu_address = 0x1BB3;
  result.erase_ranges.push_back({0x000C0000, 0x1000});
  result.signature_dev = {0x30, 0x40};
  result.signature = result.signature_dev;
  result.blocks.push_back({0x000C0000, {0x11, 0x22, 0x33, 0x44}, 0});
  return result;
}

void test_profile_and_registry() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile =
      uds::load_profile_ini(source / "profiles" / "geely_p146.ini");
  check(profile.id == L"geely_p146" && profile.flow == L"geely_p146" &&
            profile.vendor_name == L"吉利" && !profile.placeholder &&
            !profile.power_control && !profile.supports_ft_entry &&
            profile.supports_cal_download && !profile.lock_diagnostic_ids &&
            profile.tx_id == 0U && profile.rx_id == 0U &&
            profile.security_level == 0x01U &&
            profile.vbf_signature_policy == L"auto",
        "Geely P146 safe configurable Profile mismatch");
  const auto workflow = uds::create_flash_workflow(L"geely_p146");
  check(workflow && workflow->id() == L"geely_p146" &&
            workflow->report_title(profile).find("P146") != std::string::npos,
        "Geely P146 workflow registry mapping mismatch");
  check(std::filesystem::is_regular_file(
            source / "resources" / "geely_p146" / "SOURCE_MANIFEST.md"),
        "Geely P146 source manifest is missing");
}

void test_protocol_builders_and_validation() {
  check(uds::geely_geea2_request_download(0x10, 0x000C0000, 0x1000) ==
            std::vector<std::uint8_t>{0x34, 0x10, 0x44, 0x00, 0x0C, 0x00,
                                      0x00, 0x00, 0x00, 0x10, 0x00},
        "GEEA2 RequestDownload builder mismatch");
  check(uds::geely_geea2_erase_memory(0x000C0000, 0x1000) ==
            std::vector<std::uint8_t>{0x31, 0x01, 0xFF, 0x00, 0x00, 0x0C,
                                      0x00, 0x00, 0x00, 0x00, 0x10, 0x00},
        "GEEA2 EraseMemory builder mismatch");
  check(uds::geely_geea2_transfer_chunk_size(
            std::vector<std::uint8_t>{0x74, 0x20, 0x10, 0x00}) == 0x0FFEU,
        "GEEA2 transfer chunk limit mismatch");
  uds::validate_geely_geea2_images({
      {"SBL", make_sbl(), true}, {"APP", make_app(), false}});
  auto wrong = make_app();
  wrong.ecu_address = 0x2222;
  bool rejected{};
  try {
    uds::validate_geely_geea2_images({
        {"SBL", make_sbl(), true}, {"APP", wrong, false}});
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "GEEA2 mixed ecu_address VBF set was not rejected");
}

void test_normal_download_sequence() {
  ScriptedGeea2Bus bus;
  uds::IsoTpConfig config;
  config.tx_id = 0x7E3;
  config.rx_id = 0x7EB;
  config.padding = 0x55;
  config.st_min = 0;
  uds::IsoTpSession transport(bus, config);
  uds::UdsClient client(transport);
  uds::GeelyGeea2Flow flow(
      client, {},
      [](std::span<const std::uint8_t> seed, unsigned level) {
        const std::vector<std::uint8_t> expected_seed{0x12, 0x34, 0x56};
        check(seed.size() == expected_seed.size() &&
                  std::equal(seed.begin(), seed.end(), expected_seed.begin()) &&
                  level == 0x01U,
              "GEEA2 SeedKey callback input mismatch");
        return std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC};
      });
  flow.run({{"SBL", make_sbl(), true}, {"APP", make_app(), false}});
  check(flow.core_programming_completed(),
        "GEEA2 fake normal Download did not complete");
  const auto& requests = bus.requests();
  check(requests.size() >= 16U &&
            requests[0] == std::vector<std::uint8_t>({0x10, 0x03}) &&
            requests[1] ==
                std::vector<std::uint8_t>({0x31, 0x01, 0x02, 0x06}) &&
            requests[2] == std::vector<std::uint8_t>({0x10, 0x02}) &&
            requests[3] == std::vector<std::uint8_t>({0x27, 0x01}) &&
            requests[4] ==
                std::vector<std::uint8_t>({0x27, 0x02, 0xAA, 0xBB, 0xCC}) &&
            requests[requests.size() - 3U] ==
                std::vector<std::uint8_t>({0x31, 0x01, 0x02, 0x05}) &&
            requests[requests.size() - 2U] ==
                std::vector<std::uint8_t>({0x19, 0x02, 0x08}) &&
            requests.back() == std::vector<std::uint8_t>({0x11, 0x01}),
        "GEEA2 normal Download sequence mismatch");
}

} // namespace

int main() {
  try {
    test_profile_and_registry();
    test_protocol_builders_and_validation();
    test_normal_download_sequence();
    std::cout << "geely_p146_tests PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "geely_p146_tests FAIL: " << error.what() << '\n';
    return 1;
  }
}
