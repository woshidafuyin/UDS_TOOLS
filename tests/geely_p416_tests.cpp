#include "core/isotp.hpp"
#include "core/profile.hpp"
#include "core/sha256.hpp"
#include "core/uds_client.hpp"
#include "core/vbf.hpp"
#include "flash/flash_workflow.hpp"
#include "flash/geely_p416_flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Request {
  std::uint32_t id{};
  std::vector<std::uint8_t> payload;
};

class ScriptedGeelyBus final : public uds::ICanBus {
public:
  void open() override { open_ = true; }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }

  void send(const uds::CanFrame& frame) override {
    std::lock_guard lock(mutex_);
    if (frame.id == uds::kGeelyP416NmId) {
      ++wake_count;
      const std::vector<std::uint8_t> expected{
          0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      if (frame.data != expected || frame.extended || frame.fd || frame.brs) {
        invalid_wake = true;
      }
      return;
    }
    if (frame.data.empty()) return;
    const auto type = frame.data[0] >> 4U;
    if (type == 0U) {
      const auto length = static_cast<std::size_t>(frame.data[0] & 0x0FU);
      if (length == 0U || frame.data.size() < 1U + length) {
        throw std::runtime_error("invalid fake-ECU SingleFrame");
      }
      handle(frame.id, {frame.data.begin() + 1,
                        frame.data.begin() + 1 +
                            static_cast<std::ptrdiff_t>(length)});
      return;
    }
    if (type == 1U) {
      const auto length =
          static_cast<std::size_t>(((frame.data[0] & 0x0FU) << 8U) |
                                   frame.data[1]);
      transfers_[frame.id] =
          {length, std::vector<std::uint8_t>(frame.data.begin() + 2,
                                             frame.data.end())};
      reply_frame(response_id(frame.id), {0x30, 0x00, 0x00}, false);
      return;
    }
    if (type == 2U) {
      auto item = transfers_.find(frame.id);
      if (item == transfers_.end()) {
        throw std::runtime_error("fake ECU received orphan ConsecutiveFrame");
      }
      item->second.data.insert(item->second.data.end(), frame.data.begin() + 1,
                               frame.data.end());
      if (item->second.data.size() >= item->second.length) {
        auto payload = std::move(item->second.data);
        payload.resize(item->second.length);
        transfers_.erase(item);
        handle(frame.id, std::move(payload));
      }
    }
  }

  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds) override {
    std::lock_guard lock(mutex_);
    if (rx_.empty()) return std::nullopt;
    auto result = std::move(rx_.front());
    rx_.pop_front();
    return result;
  }

  std::vector<Request> snapshot_requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  std::size_t wake_count{};
  bool invalid_wake{};

private:
  struct Transfer {
    std::size_t length{};
    std::vector<std::uint8_t> data;
  };

  static std::uint32_t response_id(std::uint32_t request_id) {
    if (request_id == uds::kGeelyP416AppTxId ||
        request_id == uds::kGeelyP416AppFunctionalId) {
      return uds::kGeelyP416AppRxId;
    }
    if (request_id == uds::kGeelyP416PlsTxId ||
        request_id == uds::kGeelyP416PlsFunctionalId) {
      return uds::kGeelyP416PlsRxId;
    }
    throw std::runtime_error("unexpected fake-ECU request ID");
  }

  void reply_frame(std::uint32_t id,
                   std::initializer_list<std::uint8_t> bytes,
                   bool is_uds = true) {
    std::vector<std::uint8_t> data(8U, 0x55);
    if (is_uds) {
      data[0] = static_cast<std::uint8_t>(bytes.size());
      std::copy(bytes.begin(), bytes.end(), data.begin() + 1);
    } else {
      std::copy(bytes.begin(), bytes.end(), data.begin());
    }
    rx_.push_back({id, std::move(data), false, false, false});
  }

  void handle(std::uint32_t id, std::vector<std::uint8_t> payload) {
    requests_.push_back({id, payload});
    const auto response = response_id(id);
    if (payload == std::vector<std::uint8_t>({0x3E, 0x00})) {
      reply_frame(response, {0x7E, 0x00});
    } else if (payload.size() == 2U && payload[0] == 0x10U) {
      reply_frame(response, {0x50, payload[1], 0x00, 0x19, 0x01, 0xF4});
    } else if (payload == std::vector<std::uint8_t>({0x27, 0x01})) {
      reply_frame(response, {0x67, 0x01, 0x41, 0x01, 0x4D});
    } else if (payload.size() == 5U && payload[0] == 0x27U &&
               payload[1] == 0x02U) {
      check(payload == std::vector<std::uint8_t>(
                           {0x27, 0x02, 0xFA, 0xE1, 0x9E}),
            "flow sent wrong Geely P416 security key");
      reply_frame(response, {0x67, 0x02});
    } else if (payload.size() == 11U && payload[0] == 0x34U) {
      reply_frame(response, {0x74, 0x20, 0x08, 0x00});
    } else if (payload.size() >= 2U && payload[0] == 0x36U) {
      reply_frame(response, {0x7F, 0x36, 0x78});
      reply_frame(response, {0x76, payload[1]});
    } else if (payload == std::vector<std::uint8_t>({0x37})) {
      reply_frame(response, {0x77});
    } else if (payload.size() == 260U && payload[0] == 0x31U &&
               payload[2] == 0x02U && payload[3] == 0x12U) {
      reply_frame(response, {0x7F, 0x31, 0x78});
      reply_frame(response, {0x71, 0x01, 0x02, 0x12, 0x10, 0x00});
    } else if (payload.size() == 8U && payload[0] == 0x31U &&
               payload[2] == 0x03U && payload[3] == 0x01U) {
      reply_frame(response, {0x7F, 0x31, 0x78});
      reply_frame(response, {0x71, 0x01, 0x03, 0x01, 0x10});
    } else if (payload.size() == 12U && payload[0] == 0x31U &&
               payload[2] == 0xFFU && payload[3] == 0x00U) {
      reply_frame(response, {0x7F, 0x31, 0x78});
      reply_frame(response, {0x71, 0x01, 0xFF, 0x00, 0x10});
    } else if (payload ==
               std::vector<std::uint8_t>({0x31, 0x01, 0x02, 0x05})) {
      reply_frame(response, {0x71, 0x01, 0x02, 0x05, 0x10, 0x00});
    } else if (payload == std::vector<std::uint8_t>({0x11, 0x01})) {
      reply_frame(response, {0x51, 0x01});
    } else {
      throw std::runtime_error("unexpected fake-ECU UDS request");
    }
  }

  bool open_{true};
  mutable std::mutex mutex_;
  std::deque<uds::CanFrame> rx_;
  std::map<std::uint32_t, Transfer> transfers_;
  std::vector<Request> requests_;
};

std::string compact_hash(std::span<const std::uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : uds::sha256(bytes)) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0FU]);
  }
  return result;
}

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(input)),
          std::istreambuf_iterator<char>()};
}

uds::GeelyP416Images load_images(const std::filesystem::path& root) {
  uds::GeelyP416Images images{
      uds::load_vbf(root / "SBL" / "80048576AA.vbf"),
      uds::load_vbf(root / "ESS" / "ess_out.VBF"),
      uds::load_vbf(root / "APP" / "80078428AA.vbf"),
  };
  uds::validate_geely_p416_images(images);
  check(!images.sbl.block_crc16_verified &&
            images.ess.block_crc16_verified &&
            !images.app.block_crc16_verified,
        "supplier VBF processed-domain CRC classification mismatch");
  return images;
}

void test_profile_parser_key_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile =
      uds::load_profile_ini(source / "profiles" / "geely_p416.ini");
  check(profile.id == L"geely_p416" && profile.flow == L"geely_p416" &&
            profile.can_fd && !profile.uds_fd && !profile.uds_brs &&
            profile.supports_ft_entry && !profile.supports_cal_download &&
            profile.lock_diagnostic_ids &&
            profile.tx_id == uds::kGeelyP416AppTxId &&
            profile.rx_id == uds::kGeelyP416AppRxId &&
            profile.functional_id == uds::kGeelyP416AppFunctionalId &&
            profile.ft_tx_id == uds::kGeelyP416PlsTxId &&
            profile.ft_rx_id == uds::kGeelyP416PlsRxId &&
            profile.padding == 0x55 && profile.isotp_st_min == 0,
        "Geely P416 packaged profile mismatch");

  constexpr std::array seeds{
      std::array<std::uint8_t, 3>{0x41, 0x01, 0x4D},
      std::array<std::uint8_t, 3>{0x8B, 0x7F, 0x99},
      std::array<std::uint8_t, 3>{0xCB, 0x6C, 0xA9},
      std::array<std::uint8_t, 3>{0x6A, 0x33, 0x20},
      std::array<std::uint8_t, 3>{0xE6, 0x32, 0x07},
  };
  constexpr std::array keys{
      std::array<std::uint8_t, 3>{0xFA, 0xE1, 0x9E},
      std::array<std::uint8_t, 3>{0x27, 0xAB, 0x0B},
      std::array<std::uint8_t, 3>{0x24, 0x04, 0x51},
      std::array<std::uint8_t, 3>{0x41, 0x0A, 0x97},
      std::array<std::uint8_t, 3>{0x16, 0xE1, 0xCA},
  };
  for (std::size_t index = 0; index < seeds.size(); ++index) {
    check(uds::geely_p416_seed_key(seeds[index]) == keys[index],
          "Geely P416 captured SeedKey vector mismatch");
  }
  check(uds::resolve_geely_p416_entry_mode(L"app") ==
                uds::GeelyP416EntryMode::app_to_app &&
            uds::resolve_geely_p416_entry_mode(L"ft") ==
                uds::GeelyP416EntryMode::pls_to_app,
        "Geely P416 entry mode mapping mismatch");
  check(uds::geely_p416_request_download(0x10, 0x00430000, 0x48) ==
                std::vector<std::uint8_t>(
                    {0x34, 0x10, 0x44, 0x00, 0x43, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x48}) &&
            uds::geely_p416_erase_memory(0x0013C000, 0x2C) ==
                std::vector<std::uint8_t>(
                    {0x31, 0x01, 0xFF, 0x00, 0x00, 0x13, 0xC0, 0x00,
                     0x00, 0x00, 0x00, 0x2C}) &&
            uds::geely_p416_transfer_chunk_size(
                std::array<std::uint8_t, 4>{0x74, 0x20, 0x08, 0x00}) ==
                0x7FE,
        "Geely P416 request builders differ from Golden Trace");

  const auto root = source / "resources" / "geely_p416";
  const uds::GeelyP416Images reconstructed{
      uds::load_vbf(root / "SBL" / "P416_SBL_reconstructed.vbf"),
      uds::load_vbf(root / "ESS" / "P416_ESS_reconstructed.vbf"),
      uds::load_vbf(root / "APP" / "P416_APP_reconstructed.vbf")};
  check(compact_hash(reconstructed.sbl.signature) ==
                "14b5eb5bf3cb42219e5ab02c685471d8f99110ba702b0bffadcedd3c1784feb0" &&
            compact_hash(reconstructed.ess.signature) ==
                "90e2c9ecde20d4b29069173b075ae9c25ce0c16c2f0c748aea13f71f9d94d257" &&
            compact_hash(reconstructed.app.signature) ==
                "da91cfacefc669dd7f00c772f7bf334a615678127971588222c5608e84f9c16d",
        "Geely P416 reconstructed signatures differ from BLF baseline");
  check(compact_hash(read_all(root / "SBL" / "P416_SBL_reconstructed.vbf")) ==
                "5985a7ba6e080a9d42916991c736c203b3d34f7920b0ab7683e1a93c3ef7d3b9" &&
            compact_hash(read_all(root / "ESS" / "P416_ESS_reconstructed.vbf")) ==
                "b9e3c2e0ea23286944d4397e563e6975bbd3cacc1d13f1db2f51baf578500c7d" &&
            compact_hash(read_all(root / "APP" / "P416_APP_reconstructed.vbf")) ==
                "525319cf93c9f36fbc70136db3ebcbd43805d22de627e7375a831bc7c7198566",
        "Geely P416 reconstructed VBF SHA-256 mismatch");

  const auto workflow = uds::create_flash_workflow(L"geely_p416");
  check(workflow && workflow->id() == L"geely_p416" &&
            workflow->report_title(profile).find("P416") != std::string::npos,
        "Geely P416 workflow registry mapping mismatch");
}

void validate_trace(const std::vector<Request>& requests,
                    uds::GeelyP416EntryMode mode,
                    const uds::GeelyP416Images& images) {
  const std::vector<Request> app_prefix{
      {0x7FF, {0x3E, 0x00}}, {0x716, {0x10, 0x01}},
      {0x716, {0x10, 0x02}}, {0x7FF, {0x3E, 0x00}},
      {0x716, {0x27, 0x01}}, {0x716, {0x27, 0x02, 0xFA, 0xE1, 0x9E}},
  };
  const std::vector<Request> pls_prefix{
      {0x7DF, {0x3E, 0x00}}, {0x701, {0x10, 0x01}},
      {0x701, {0x10, 0x03}}, {0x701, {0x10, 0x02}},
      {0x7FF, {0x3E, 0x00}}, {0x716, {0x27, 0x01}},
      {0x716, {0x27, 0x02, 0xFA, 0xE1, 0x9E}},
  };
  const auto& prefix = mode == uds::GeelyP416EntryMode::app_to_app
                           ? app_prefix
                           : pls_prefix;
  check(requests.size() > prefix.size(), "fake ECU captured too few requests");
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    check(requests[index].id == prefix[index].id &&
              requests[index].payload == prefix[index].payload,
          "Geely P416 APP/PLS entry Golden Trace mismatch");
  }

  std::vector<std::vector<std::uint8_t>> downloads;
  std::vector<std::vector<std::uint8_t>> erases;
  std::vector<std::vector<std::uint8_t>> signatures;
  for (const auto& request : requests) {
    if (!request.payload.empty() && request.payload[0] == 0x34U) {
      downloads.push_back(request.payload);
    }
    if (request.payload.size() == 12U && request.payload[0] == 0x31U &&
        request.payload[2] == 0xFFU) {
      erases.push_back(request.payload);
    }
    if (request.payload.size() == 260U && request.payload[0] == 0x31U &&
        request.payload[2] == 0x02U && request.payload[3] == 0x12U) {
      signatures.push_back(request.payload);
    }
  }
  std::vector<std::vector<std::uint8_t>> expected_downloads;
  for (const auto* file : {&images.sbl, &images.ess, &images.app}) {
    for (const auto& block : file->blocks) {
      expected_downloads.push_back(uds::geely_p416_request_download(
          file->data_format_identifier, block.address,
          static_cast<std::uint32_t>(block.data.size())));
    }
  }
  check(downloads == expected_downloads,
        "Geely P416 ten RequestDownload records mismatch");
  const std::vector<std::vector<std::uint8_t>> expected_erases{
      uds::geely_p416_erase_memory(0x0013C000, 0x2C),
      uds::geely_p416_erase_memory(0x0013C100, 0x40),
      uds::geely_p416_erase_memory(0x000C0000, 0x2C),
      uds::geely_p416_erase_memory(0x000C1000, 0x7B000),
  };
  check(erases == expected_erases,
        "Geely P416 ESS/APP erase Golden Trace mismatch");
  check(signatures ==
            std::vector<std::vector<std::uint8_t>>{
                uds::geely_p416_verify_signature(images.sbl.signature),
                uds::geely_p416_verify_signature(images.ess.signature),
                uds::geely_p416_verify_signature(images.app.signature)},
        "Geely P416 signature request order mismatch");
  check(requests[requests.size() - 3U].payload ==
                std::vector<std::uint8_t>({0x31, 0x01, 0x02, 0x05}) &&
            requests[requests.size() - 2U].payload ==
                std::vector<std::uint8_t>({0x11, 0x01}) &&
            requests.back().payload ==
                std::vector<std::uint8_t>({0x10, 0x03}),
        "Geely P416 dependency/reset/post-reset tail mismatch");
}

void run_fake_flow(uds::GeelyP416EntryMode mode) {
  const auto root = std::filesystem::path(UDS_SOURCE_DIR) / "resources" /
                    "geely_p416";
  const auto images = load_images(root);
  ScriptedGeelyBus bus;
  uds::IsoTpConfig app_config;
  app_config.tx_id = 0x716;
  app_config.rx_id = 0x616;
  app_config.padding = 0x55;
  app_config.st_min = 0;
  uds::IsoTpSession app_transport(bus, app_config);
  auto app_functional_config = app_config;
  app_functional_config.tx_id = 0x7FF;
  uds::IsoTpSession app_functional_transport(bus, app_functional_config);
  auto pls_config = app_config;
  pls_config.tx_id = 0x701;
  pls_config.rx_id = 0x761;
  uds::IsoTpSession pls_transport(bus, pls_config);
  auto pls_functional_config = pls_config;
  pls_functional_config.tx_id = 0x7DF;
  uds::IsoTpSession pls_functional_transport(bus, pls_functional_config);
  uds::UdsClient app_physical(app_transport);
  uds::UdsClient app_functional(app_functional_transport);
  uds::UdsClient pls_physical(pls_transport);
  uds::UdsClient pls_functional(pls_functional_transport);
  uds::GeelyP416Flow flow(app_physical, app_functional, pls_physical,
                          pls_functional, app_transport, {},
                          {0ms, 0ms, 1ms});
  flow.run(images, mode);
  check(flow.core_programming_completed(),
        "Geely P416 Fake ECU flow did not complete");
  check(bus.wake_count > 0U && !bus.invalid_wake,
        "Geely P416 0x53F/200 ms wake contract was not active");
  validate_trace(bus.snapshot_requests(), mode, images);
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 1) {
      for (int index = 1; index < argc; ++index) {
        const auto parsed = uds::load_vbf(std::filesystem::path(argv[index]));
        check(!parsed.blocks.empty(),
              std::string("VBF has no data blocks: ") + argv[index]);
        std::cout << "VBF parse PASS: " << argv[index]
                  << "; type=" << parsed.sw_part_type
                  << "; blocks=" << parsed.blocks.size()
                  << "; signature=" << parsed.signature.size() << " bytes\n";
      }
      return 0;
    }
    test_profile_parser_key_and_resources();
    run_fake_flow(uds::GeelyP416EntryMode::app_to_app);
    run_fake_flow(uds::GeelyP416EntryMode::pls_to_app);
    std::cout << "geely_p416_tests PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "geely_p416_tests FAIL: " << error.what() << '\n';
    return 1;
  }
}
