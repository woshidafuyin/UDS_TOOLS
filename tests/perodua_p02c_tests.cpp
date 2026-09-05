#include "core/aes_cmac.hpp"
#include "core/hex.hpp"
#include "core/profile.hpp"
#include "core/version_check_plan.hpp"
#include "flash/perodua_p02c_flow.hpp"
#include "flash/perodua_p02c_workflow.hpp"

#include <Windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Endpoint = uds::PeroduaEndpoint;
using namespace std::chrono_literals;
void check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
template<class Fn> void fails(Fn fn, const std::string& message) {
  bool failed = false;
  try { fn(); } catch (const std::exception&) { failed = true; }
  check(failed, message);
}
Bytes hex(const char* text) { return uds::from_hex(text); }

void crypto_vectors() {
  const auto key_bytes = hex("2b7e151628aed2a6abf7158809cf4f3c");
  uds::Aes128Block key{};
  std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
  const auto message = hex(
      "6bc1bee22e409f96e93d7e117393172a"
      "ae2d8a571e03ac9c9eb76fac45af8e51"
      "30c81c46a35ce411e5fbc1191a0a52ef"
      "f69f2445df4f9b17ad2b417be66c3710");
  // RFC4493 section 4: independent published vectors, not OEM secrets.
  const std::array vectors{
      std::pair{0U, "bb1d6929e95937287fa37d129b756746"},
      std::pair{16U, "070a16b46b4d4144f79bdd9dd04a287c"},
      std::pair{40U, "dfa66747de9ae63030ca32611497c827"},
      std::pair{64U, "51f0bebf7e3b9d92fc49741779363cfe"}};
  for (const auto& [count, expected] : vectors) {
    const auto result = uds::aes128_cmac(key, std::span(message).first(count));
    check(Bytes(result.begin(), result.end()) == hex(expected), "RFC4493 CMAC vector failed");
  }
  check(uds::perodua_crc32(hex("313233343536373839")) == 0xCBF43926U,
        "CRC32 IEEE check value failed");
  check(uds::perodua_crc32(hex("313233343536373839"), false) == 0xFC891918U, "CRC32 non-reflected check value failed");
}

Bytes fingerprint() {
  std::tm date{};
  date.tm_year = 126;
  date.tm_mon = 8;
  date.tm_mday = 5;
  return uds::perodua_fingerprint(date, L"SHOP000001TESTER00000000001");
}

uds::PeroduaImages images() {
  return {
      {"Driver", 0x2000, 0x100, {{{0x2000, hex("313233343536373839")}}, 9}},
      {{"APP", 0x10000, 0x100,
          {{{0x10000, hex("01020304050607")}, {0x10040, hex("AABBCCDD")}}, 11}},
       {"CAL", 0x20000, 0x100, {{{0x20000, hex("102030")}}, 3}}}};
}

struct Request { Endpoint endpoint; Bytes data; };

struct Peer {
  std::vector<Request> requests;
  std::vector<Bytes> functionals;
  std::vector<std::chrono::milliseconds> waits;
  Bytes saved_fingerprint;
  Bytes precondition_tail;
  Bytes bad_request;
  Bytes bad_reply;
  unsigned timeout_count{};
  unsigned request_count_36{};
  bool wrong_bsc{};
  bool zero_seed{};
  bool corrupt_fingerprint{};
  std::size_t negotiated_length{6};
  std::size_t key_calls{};
  std::stop_source* cancel_on_download{};

  uds::UdsResponse request(Endpoint endpoint, std::span<const std::uint8_t> payload,
                           std::chrono::milliseconds p2, std::chrono::milliseconds p2star) {
    check(p2 == 150ms && p2star == 5000ms, "wrong CES006 diagnostic timeouts");
    Bytes data(payload.begin(), payload.end());
    requests.push_back({endpoint, data});
    Bytes reply;
    if (data == bad_request) reply = bad_reply;
    else if (data == hex("22F191")) reply = hex("62F191435044");
    else if (data == hex("31010203")) {
      check(endpoint == Endpoint::gateway, "0203 must address the gateway");
      reply = hex("71010203");
      reply.insert(reply.end(), precondition_tail.begin(), precondition_tail.end());
    } else if (data == hex("1002")) reply = hex("5002003201F4");
    else if (data == hex("2707")) {
      reply = {0x67, 0x07};
      reply.insert(reply.end(), 16, zero_seed ? 0 : 0x11);
    } else if (data.size() == 18 && data[0] == 0x27 && data[1] == 0x08) {
      reply = hex("6708");
    } else if (data.size() == 33 && data[0] == 0x2E) {
      check(data[1] == 0xF1 && data[2] == 0x07, "wrong fingerprint DID");
      saved_fingerprint.assign(data.begin() + 3, data.end());
      reply = hex("6EF107");
    } else if (data.size() == 11 && data[0] == 0x34) {
      check(data[1] == 0 && data[2] == 0x44, "wrong download format");
      if (cancel_on_download) cancel_on_download->request_stop();
      reply = {0x74, 0x20, static_cast<std::uint8_t>(negotiated_length >> 8U),
                static_cast<std::uint8_t>(negotiated_length)};
    } else if (data.size() >= 3 && data[0] == 0x36) {
      ++request_count_36;
      if (timeout_count) { --timeout_count; throw uds::UdsResponseTimeout(); }
      reply = {0x76, static_cast<std::uint8_t>(data[1] + (wrong_bsc ? 1 : 0))};
    } else if (data == hex("37")) reply = hex("77");
    else if (data[0] == 0x31) reply = {0x71, 0x01, data[2], data[3], 0x00};
    else if (data == hex("1101")) reply = hex("5101");
    else if (data == hex("22F186")) reply = hex("62F18601");
    else if (data == hex("22F107")) {
      reply = hex("62F107");
      reply.insert(reply.end(), saved_fingerprint.begin(), saved_fingerprint.end());
      if (corrupt_fingerprint) reply.back() ^= 1;
    } else throw std::runtime_error("unexpected UDS request in Perodua test: " + uds::to_hex(data));
    if (data != hex("31010203")) check(endpoint == Endpoint::ecu, "programming must address CPD physically");
    const bool negative = !reply.empty() && reply[0] == 0x7F;
    return {!negative, data, reply, static_cast<std::uint8_t>(negative ? reply[2] : 0),
             negative ? "NRC rejected" : "OK", 1ms};
  }
  uds::PeroduaIo io() {
    return {
      [&](Endpoint e, auto r, auto p2, auto p2star) { return request(e, r, p2, p2star); },
      [&](auto data) { functionals.emplace_back(data.begin(), data.end()); },
      [&](auto duration) { waits.push_back(duration); },
      [&](auto seed) {
        check(seed.size() == 16, "keygen did not receive 16 bytes");
        ++key_calls;
        return Bytes(16, 0x22);
      }, {}, {}};
  }
  bool has(const Bytes& data) const {
    return std::any_of(requests.begin(), requests.end(), [&](auto& r) { return r.data == data; });
  }
};

void protocol_success() {
  Peer peer;
  uds::PeroduaP02cFlow flow(peer.io());
  flow.run(images(), fingerprint());
  check(flow.programming_completed(), "programming completion flag not set");
  const std::vector<Bytes> expected_functionals{
      hex("1083"), hex("8582"), hex("288103"), hex("1083"),
      hex("288003"), hex("8581"), hex("1081")};
  check(peer.functionals == expected_functionals, "pre/post functional sequence differs from CES012");
  check(peer.waits == std::vector<std::chrono::milliseconds>{50ms,50ms,50ms,2000ms,50ms,50ms,50ms,50ms},
        "CES012 50ms/2s delays missing");
  check(peer.requests[0].data == hex("22F191") && peer.requests[1].data == hex("31010203"),
        "identity/precondition order wrong");
  check(peer.has(hex("31010202CBF43926")), "Driver CRC32 request must be MSB first");
  check(peer.has(hex("3400440000200000000009")), "Driver download address/length wrong");
  check(peer.has(hex("3101FF00440001000000000007")), "APP logical erase window wrong");
  check(peer.has(hex("360131323334")) && peer.has(hex("360235363738")) && peer.has(hex("360339")),
        "TransferData must honor negotiated length minus SID/BSC and short last block");
  const auto erase_count = std::count_if(peer.requests.begin(), peer.requests.end(), [](auto& r) {
    return r.data.size() >= 4 && r.data[0] == 0x31 && r.data[2] == 0xFF && r.data[3] == 0;
  });
  check(erase_count == 3, "each APP/CAL segment must execute its own EraseMemory");
  check(peer.has(hex("3601AABBCCDD")), "BSC must restart for each RequestDownload");
  check(peer.saved_fingerprint.size() == 30 &&
        std::equal(peer.saved_fingerprint.begin(), peer.saved_fingerprint.begin() + 3, hex("260905").begin()),
        "fingerprint must use YY MM DD BCD");

  Peer alternative;
  alternative.precondition_tail = {0,0,0,0};
  alternative.zero_seed = true;
  uds::PeroduaP02cFlow(alternative.io()).run(images(), fingerprint());
  check(alternative.key_calls == 0 && !alternative.has(Bytes{0x27,0x08}),
        "already-unlocked ECU should skip SendKey");
}

void failures_and_retries() {
  for (const auto& tail : {Bytes{1}, Bytes{0}, Bytes{0,0,1,0}, Bytes{0,0,0,0,0}}) {
    Peer peer;
    peer.precondition_tail = tail;
    fails([&] { uds::PeroduaP02cFlow(peer.io()).run(images(), fingerprint()); },
          "unfulfilled/malformed preconditions were accepted");
    check(!peer.has(hex("1002")) && peer.functionals.size() == 1,
          "failed preconditions must stop before disabling DTC/traffic/programming");
  }
  for (const auto& request : {hex("31010202CBF43926"), hex("3101FF00440001000000000007"), hex("3101FF01")}) {
    Peer peer;
    peer.bad_request = request;
    peer.bad_reply = {0x71,0x01,request[2],request[3],0x01};
    fails([&] { uds::PeroduaP02cFlow(peer.io()).run(images(), fingerprint()); },
          "positive SID with failed routineResult was accepted");
    check(!peer.has(hex("1101")), "routine failure must not reset ECU into an incomplete image");
  }
  for (const auto& reply : {hex("7F2735"), hex("7F2736"), hex("7F2737"), hex("67070102")}) {
    Peer peer;
    peer.bad_request = hex("2707"); peer.bad_reply = reply;
    fails([&] { uds::PeroduaP02cFlow(peer.io()).run(images(), fingerprint()); }, "bad seed/security NRC accepted");
    check(peer.saved_fingerprint.empty(), "security failure allowed fingerprint/programming");
  }
  Peer retry;
  retry.timeout_count = 2;
  uds::PeroduaP02cFlow(retry.io()).run(images(), fingerprint());
  std::vector<Bytes> first_blocks;
  for (const auto& r : retry.requests) if (r.data[0] == 0x36) first_blocks.push_back(r.data);
  check(first_blocks.size() >= 3 && first_blocks[0] == first_blocks[1] && first_blocks[1] == first_blocks[2],
        "retry must preserve exact BSC and payload");
  Peer exhausted; exhausted.timeout_count = 3;
  fails([&] { uds::PeroduaP02cFlow(exhausted.io()).run(images(), fingerprint()); }, "timeout retry limit not enforced");
  check(exhausted.request_count_36 == 3 && !exhausted.has(hex("37")), "more than two retries or advance after timeout");
  Peer bsc; bsc.wrong_bsc = true;
  fails([&] { uds::PeroduaP02cFlow(bsc.io()).run(images(), fingerprint()); }, "wrong BSC accepted");
  check(bsc.request_count_36 == 1, "response mismatch was retried");
  check(!bsc.has(hex("22F107")), "no extra fingerprint readback");
  std::stop_source stop;
  Peer cancelled; cancelled.cancel_on_download = &stop;
  fails([&] { uds::PeroduaP02cFlow(cancelled.io()).run(images(), fingerprint(), stop.get_token()); }, "cancellation ignored");
  check(cancelled.request_count_36 == 0, "cancellation allowed transfer to start");
}

void boundaries() {
  for (const auto& response : {hex("74200000"), hex("74200002"), hex("74201000"),
                               hex("7400"), hex("74210006"), hex("74500102030405")})
    fails([&] { uds::perodua_max_block_length(response); }, "invalid block negotiation accepted");
  check(uds::perodua_max_block_length(hex("74200FFF")) == 4095, "4095-byte block rejected");
  auto fixture = images();
  fixture.driver.image.segments[0].data.assign(1030, 0xAA);
  fixture.driver.length = 0x1000;
  Peer wrap;
  uds::PeroduaP02cFlow(wrap.io()).run(fixture, fingerprint());
  std::vector<std::uint8_t> bsc;
  for (const auto& r : wrap.requests) {
    if (r.data[0] == 0x37) break;
    if (r.data[0] == 0x36) bsc.push_back(r.data[1]);
  }
  check(bsc.size() == 258 && bsc[254] == 255 && bsc[255] == 0 && bsc[256] == 1,
        "BSC FF -> 00 -> 01 wrap failed");
  auto invalid = images();
  invalid.driver.image.segments[0].address = 0xFFFFFFFF;
  Peer peer;
  fails([&] { uds::PeroduaP02cFlow(peer.io()).run(invalid, fingerprint()); }, "UDS address overflow accepted");
  check(peer.requests.empty(), "invalid UDS address touched bus");
  std::tm date{}; date.tm_year=126; date.tm_mon=1; date.tm_mday=29;
  fails([&] { uds::perodua_fingerprint(date,L"TEST"); }, "invalid leap date accepted");
  date.tm_year=124;
  check(uds::perodua_fingerprint(date,L"TEST")[2] == 0x29, "valid leap date rejected");
  fails([&] { uds::perodua_fingerprint(date,L""); }, "empty tester identity accepted");
  fails([&] { uds::perodua_mode(L"ft"); }, "undefined FT entry accepted");
  check(uds::perodua_mode(L"cal") == uds::PeroduaMode::cal &&
        uds::perodua_mode(L"app_cal") == uds::PeroduaMode::app_cal, "mode mapping wrong");
}

struct FixtureDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      (L"uds_perodua_tests_" + std::to_wstring(GetCurrentProcessId()));
  FixtureDirectory() { std::filesystem::create_directories(path); }
  ~FixtureDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void write_protected_key(const std::filesystem::path& path) {
  Bytes key(16, 0x33);
  DATA_BLOB raw{16, key.data()}, encrypted{};
  check(CryptProtectData(&raw, L"Perodua unit test (synthetic key)", nullptr, nullptr,
                        nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted) != FALSE,
        "cannot protect dummy key");
  std::ofstream out(path, std::ios::binary);
  out.write("CHKEY1",6);
  out.write(reinterpret_cast<char*>(encrypted.pbData), encrypted.cbData);
  LocalFree(encrypted.pbData);
}

class NoHardwareProvider final : public uds::ICanBusProvider {
public:
  mutable unsigned opened{};
  std::unique_ptr<uds::ICanBus> create(uds::CanChannelConfig) const override {
    ++opened; throw std::runtime_error("test must not access hardware");
  }
};

void files_and_profile() {
  FixtureDirectory dir;
  const auto key = dir.path / "dummy.key";
  write_protected_key(key);
  check(uds::load_protected_aes128_key(key) == uds::Aes128Block{
      0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33},
        "protected key roundtrip mismatch");
  { std::ofstream out(dir.path / "bad.key"); out << "do not accept raw plaintext OEM keys"; }
  fails([&] { uds::load_protected_aes128_key(dir.path / "bad.key"); }, "plaintext key accepted");
  { std::ofstream out(dir.path / "firmware.bin", std::ios::binary); out << "123456789"; }
  const auto image = uds::load_perodua_image(dir.path / "firmware.bin",0x2000);
  check(image.segments.size() == 1 && image.segments[0].address == 0x2000 && image.payload_size == 9,
        "BIN address binding incorrect");
  { std::ofstream out(dir.path / "firmware.s19"); out << "S30E00002000313233343536373839F5\n"; }
  // Deliberately invalid S-record checksum must be rejected by the shared parser.
  fails([&] { uds::load_perodua_image(dir.path / "firmware.s19",0); }, "invalid S-record accepted");
  const auto source = std::filesystem::path(UDS_SOURCE_DIR) / "profiles/perodua_p02c.ini";
  auto profile = uds::load_profile_ini(source);
  check(profile.flow == L"perodua_p02c" && !profile.placeholder &&
        profile.tx_id == 0x714 && profile.rx_id == 0x794 && profile.gateway_tx_id == 0x701 &&
        profile.gateway_rx_id == 0x781 && !profile.can_fd && profile.padding == 0xFF &&
        profile.supports_cal_download && !profile.supports_ft_entry &&
        !profile.copy_selected_files_to_resources && profile.security_algorithm == L"aes128_cmac",
        "Perodua profile contract mismatch");
  check(uds::is_flash_workflow_registered(profile.flow), "Perodua workflow not registered");
  check(uds::load_version_check_plan(source,{}).items.size() == 5, "Perodua version read plan missing");
  profile.security_key_file = key;
  profile.programming_tester_identity = L"SHOP000001TESTER00000000001";
  uds::save_profile_ini(profile, dir.path / "roundtrip.ini");
  const auto loaded = uds::load_profile_ini(dir.path / "roundtrip.ini");
  check(loaded.security_key_file == key && loaded.gateway_tx_id == 0x701 &&
        loaded.programming_tester_identity == profile.programming_tester_identity &&
        !loaded.copy_selected_files_to_resources, "new profile fields were lost when saved");
  auto provider = std::make_shared<NoHardwareProvider>();
  uds::FlashJob job;
  job.profile = profile; job.can_bus_provider = provider;
  job.driver_file = dir.path / "firmware.bin";
  job.app_file = job.driver_file; job.cal_file = job.driver_file;
  for (const auto* mode : {L"app",L"cal",L"app_cal"}) {
    job.entry_mode = mode;
    fails([&] { uds::PeroduaP02cWorkflow().run(job, {}, {}); }, "missing project CRC definition was accepted");
  }
  check(provider->opened == 0, "unconfirmed CRC opened the CAN channel");
}

// Exercise the production workflow through real UdsClient + IsoTpSession.
// Only the CAN adapter is replaced; all frames use 8-byte Classic CAN.
class SimulatedCanBus final : public uds::ICanBus {
public:
  explicit SimulatedCanBus(std::shared_ptr<Peer> peer) : peer_(std::move(peer)) {}
  void open() override { open_ = true; }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }
  void send(const uds::CanFrame& frame) override {
    std::lock_guard lock(mutex_);
    check(frame.data.size() == 8 && !frame.fd && !frame.brs && !frame.extended,
          "Perodua must use 8-byte Classic CAN");
    if (frame.id == 0x7DF) {
      const auto length = frame.data[0];
      check(length == 2 || length == 3, "functional request must be a SingleFrame");
      Bytes data(frame.data.begin() + 1, frame.data.begin() + length + 1);
      if (data != hex("3E80")) peer_->functionals.push_back(data);
      return;
    }
    check(frame.id == 0x714 || frame.id == 0x701, "wrong physical endpoint");
    const auto type = frame.data[0] >> 4U;
    if (type == 0) {
      const auto length = frame.data[0] & 0x0FU;
      for (std::size_t i = length + 1; i < 8; ++i)
        check(frame.data[i] == 0xFF, "wrong padding byte");
      handle(frame.id, Bytes(frame.data.begin() + 1, frame.data.begin() + 1 + length));
    } else if (type == 1) {
      length_ = ((frame.data[0] & 0x0FU) << 8U) | frame.data[1];
      incoming_.assign(frame.data.begin() + 2, frame.data.end());
      queue(frame.id + 0x80, {0x30,0,0});
    } else if (type == 2) {
      incoming_.insert(incoming_.end(), frame.data.begin() + 1, frame.data.end());
      if (incoming_.size() >= length_) {
        incoming_.resize(length_);
        auto complete = std::move(incoming_);
        handle(frame.id,std::move(complete));
      }
    } else if (type == 3) {
      check(frame.data[1] == (boot_ ? 0 : 8) && frame.data[2] == (boot_ ? 0 : 20),
            "wrong application/boot receive BS or STmin");
      std::uint8_t sequence = 1;
      for (std::size_t offset = 6; offset < pending_.size(); offset += 7) {
        Bytes data{static_cast<std::uint8_t>(0x20 | sequence)};
        const auto count = std::min<std::size_t>(7,pending_.size() - offset);
        data.insert(data.end(), pending_.begin() + offset, pending_.begin() + offset + count);
        queue(frame.id + 0x80, std::move(data));
        sequence = static_cast<std::uint8_t>((sequence + 1) & 0x0F);
      }
      pending_.clear();
    } else throw std::runtime_error("unknown ISO-TP frame");
  }
  std::optional<uds::CanFrame> receive(std::chrono::milliseconds) override {
    std::lock_guard lock(mutex_);
    if (rx_.empty()) return {};
    auto frame = rx_.front(); rx_.pop_front(); return frame;
  }
private:
  void queue(std::uint32_t id, Bytes data) {
    data.resize(8,0xFF);
    rx_.push_back({id,std::move(data),false,false,false});
  }
  void handle(std::uint32_t tx, Bytes request) {
    if (request == hex("1002")) boot_ = true;
    auto reply = peer_->request(tx == 0x701 ? Endpoint::gateway : Endpoint::ecu,
                               request,150ms,5000ms).response;
    if (request[0] == 0x31 && request != hex("31010203")) {
      // Pending must be consumed without resending the destructive request.
      queue(tx + 0x80, {3,0x7F,0x31,0x78});
      queue(tx + 0x80, {3,0x7F,0x31,0x78});
    }
    if (request == hex("1101")) boot_ = false;
    if (reply.size() <= 7) {
      Bytes frame{static_cast<std::uint8_t>(reply.size())};
      frame.insert(frame.end(),reply.begin(),reply.end());
      queue(tx + 0x80,std::move(frame));
    } else {
      pending_ = reply;
      Bytes frame{static_cast<std::uint8_t>(0x10 | (reply.size() >> 8U)),
                   static_cast<std::uint8_t>(reply.size())};
      frame.insert(frame.end(),reply.begin(),reply.begin() + 6);
      queue(tx + 0x80,std::move(frame));
    }
  }
  std::shared_ptr<Peer> peer_;
  std::mutex mutex_;
  std::deque<uds::CanFrame> rx_;
  Bytes incoming_, pending_;
  std::size_t length_{};
  bool open_{true}, boot_{};
};

class SimulatedProvider final : public uds::ICanBusProvider {
public:
  std::shared_ptr<Peer> peer = std::make_shared<Peer>();
  std::unique_ptr<uds::ICanBus> create(uds::CanChannelConfig config) const override {
    check(config.nominal_bitrate == 500000 && !config.can_fd, "wrong CAN adapter config");
    return std::make_unique<SimulatedCanBus>(peer);
  }
};

void workflow_integration() {
  FixtureDirectory dir;
  write_protected_key(dir.path / "test.key");
  { std::ofstream out(dir.path / "image.bin",std::ios::binary); out << "123456789"; }
  auto p = uds::load_profile_ini(std::filesystem::path(UDS_SOURCE_DIR) / "profiles/perodua_p02c.ini");
  p.programming_crc_variant=L"reflected";
  p.driver_start=0x2000; p.driver_length=0x100;
  p.app_start=0x10000; p.app_length=0x100;
  p.cal_start=0x20000; p.cal_length=0x100;
  p.programming_tester_identity=L"SHOP000001TESTER00000000001";
  for (const auto* mode : {L"app",L"cal",L"app_cal"}) {
    auto provider=std::make_shared<SimulatedProvider>();
    uds::FlashJob job;
    job.profile=p; job.entry_mode=mode; job.can_bus_provider=provider;
    job.driver_file=dir.path / "image.bin";
    if (job.entry_mode != L"cal") job.app_file=job.driver_file;
    if (job.entry_mode != L"app") job.cal_file=job.driver_file;
    job.security_key_file=dir.path / "test.key";
    bool completed=false;
    uds::FlashWorkflowCallbacks callbacks;
    callbacks.report=[&](auto step,auto verdict,auto) {
      if (step == "Perodua download" && verdict == "PASS") completed=true;
    };
    uds::PeroduaP02cWorkflow().run(job,callbacks,{});
    check(completed,"real workflow failed to report completion through simulated ISO-TP");
    const auto& peer=*provider->peer;
    check(peer.has(hex("3101FF00440001000000000009")) == (job.entry_mode != L"cal"),
          "APP erase selection does not match mode");
    check(peer.has(hex("3101FF00440002000000000009")) == (job.entry_mode != L"app"),
          "CAL erase selection does not match mode");
  }
}
} // namespace

int main() {
  try {
    crypto_vectors(); protocol_success(); failures_and_retries(); boundaries(); files_and_profile(); workflow_integration();
    std::cout << "perodua_p02c_tests: PASS (crypto, protocol order, segmentation, failure gates, retries, files, profile)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "perodua_p02c_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
