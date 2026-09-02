#include "core/can_bus.hpp"
#include "core/asc_trace.hpp"
#include "core/bus_monitor_trace.hpp"
#include "core/can_id_filter.hpp"
#include "core/flash_data.hpp"
#include "core/hex.hpp"
#include "core/html_report.hpp"
#include "core/isotp.hpp"
#include "core/profile.hpp"
#include "core/version_check_plan.hpp"
#include "core/sha256.hpp"
#include "core/uds_client.hpp"
#include "core/uds_nrc.hpp"
#include "flash/baic_radar_flow.hpp"
#include "flash/baic_radar_workflows.hpp"
#include "flash/chery_ars1_33_flow.hpp"
#include "flash/chery_ars1_31_app_flow.hpp"
#include "flash/chery_ars1_31_app_flow.hpp"
#include "flash/chery_kp31_flow.hpp"
#include "flash/chuneng_331_flow.hpp"
#include "flash/chuneng_331_workflow.hpp"
#include "flash/flash_workflow.hpp"
#include "flash/longma_ars1_31_flow.hpp"
#include "flash/lp_arc_flow.hpp"
#include "flash/lp_arf_flow.hpp"
#include "flash/lp_arf_workflow.hpp"
#include "flash/shidaixinan_hjzj_fmr_flow.hpp"
#include "flash/xizhong_rsmr_flow.hpp"
#include "flash/xizhong_rsmr_workflow.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

class MockBus final : public uds::ICanBus {
public:
  void open() override { open_ = true; }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }
  void send(const uds::CanFrame& frame) override {
    sent.push_back(frame);
    if (!inject_after_send.empty()) {
      for (auto& response : inject_after_send) rx.push_back(std::move(response));
      inject_after_send.clear();
    }
  }
  std::optional<uds::CanFrame> receive(std::chrono::milliseconds) override {
    if (rx.empty()) return std::nullopt;
    auto value = rx.front();
    rx.pop_front();
    return value;
  }

  bool open_{};
  std::vector<uds::CanFrame> sent;
  std::deque<uds::CanFrame> rx;
  std::vector<uds::CanFrame> inject_after_send;
};

namespace {

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

uds::CanFrame frame(std::initializer_list<std::uint8_t> data) {
  return {0x77A, data, false, false, false};
}

void test_hex() {
  check(uds::to_hex(uds::from_hex("10 01")) == "10 01", "hex round-trip failed");
}

void test_sha256() {
  constexpr std::array<std::uint8_t, 3> input{'a', 'b', 'c'};
  check(uds::to_hex(uds::sha256(input)) ==
            "BA 78 16 BF 8F 01 CF EA 41 41 40 DE 5D AE 22 23 "
            "B0 03 61 A3 96 17 7A 9C B4 10 FF 61 F2 00 15 AD",
        "SHA-256 known vector mismatch");
}

void test_asc_trace_can_bus() {
  const auto exported_classic = uds::format_asc_record(
      0.125, 2, uds::CanTraceDirection::transmit,
      uds::CanFrame{0x744, {0x10, 0x01}, false, false, false});
  const auto exported_fd = uds::format_asc_record(
      0.250, 2, uds::CanTraceDirection::receive,
      uds::CanFrame{0x18DAF1B7, {0x30, 0x00, 0x0A}, true, true, true});
  check(exported_classic.find("2 744 Tx d 2 10 01") != std::string::npos &&
            exported_fd.find("CANFD 2 Rx 18daf1b7x 1 1 3 3 30 00 0A") !=
                std::string::npos,
        "ASC export formatter omitted CAN ID or frame attributes");

  const auto trace_path =
      std::filesystem::temp_directory_path() / "uds_asc_trace_test.asc";
  const auto blf_path =
      std::filesystem::temp_directory_path() / "uds_asc_trace_test.blf";
  std::error_code ignored;
  std::filesystem::remove(trace_path, ignored);
  std::filesystem::remove(blf_path, ignored);
  std::filesystem::remove(blf_path.wstring() + L".partial", ignored);

  auto inner = std::make_unique<MockBus>();
  auto* inner_view = inner.get();
  inner->rx.push_back(
      uds::CanFrame{0x18DAF1B7, {0x30, 0x00, 0x0A}, true, true, true});
  auto writer = std::make_shared<uds::AscTraceWriter>(trace_path, 2);
  auto blf_writer = std::make_shared<uds::BusMonitorTraceSession>(
      blf_path.parent_path(), blf_path);
  check(writer->is_open(), "ASC trace file could not be opened");
  check(blf_writer->start(2), "paired BLF trace file could not be opened");
  {
    uds::TracingCanBus bus(
        std::move(inner),
        std::vector<std::shared_ptr<uds::ICanTraceWriter>>{writer,
                                                          blf_writer});
    bus.open();
    bus.send(uds::CanFrame{0x744, {0x10, 0x01}, false, false, false});
    const std::array fallback_batch{
        uds::CanFrame{0x745, {0x21, 0x02}, false, false, false},
        uds::CanFrame{0x746, {0x22, 0x03}, false, false, false},
    };
    bus.send_batch(fallback_batch);
    const auto received = bus.receive(std::chrono::milliseconds(1));
    check(received && received->id == 0x18DAF1B7,
          "tracing CAN bus changed the received frame");
    check(inner_view->sent.size() == 3 &&
              inner_view->sent.front().data ==
                  std::vector<std::uint8_t>({0x10, 0x01}),
          "tracing CAN bus changed the transmitted frame");
  }
  writer.reset();
  check(blf_writer->frame_count() == 4,
        "paired BLF trace did not receive the same four CAN frames as ASC");
  std::string blf_snapshot;
  check(blf_writer->export_snapshot(
            [&blf_snapshot](std::string_view chunk) {
              blf_snapshot.append(chunk);
              return true;
            }),
        "paired BLF trace could not be inspected");
  const auto blf_byte = [&blf_snapshot](std::size_t offset) {
    return static_cast<std::uint8_t>(blf_snapshot.at(offset));
  };
  const auto blf_u32 = [&blf_byte](std::size_t offset) {
    return static_cast<std::uint32_t>(blf_byte(offset)) |
           (static_cast<std::uint32_t>(blf_byte(offset + 1)) << 8U) |
           (static_cast<std::uint32_t>(blf_byte(offset + 2)) << 16U) |
           (static_cast<std::uint32_t>(blf_byte(offset + 3)) << 24U);
  };
  constexpr std::size_t kPairedFirstObject = 144 + 16 + 16;
  constexpr std::size_t kPairedFourthObject = kPairedFirstObject + 3 * 48;
  constexpr std::size_t kPairedFourthPayload = kPairedFourthObject + 32;
  check(blf_snapshot.starts_with("LOGG") &&
            blf_u32(kPairedFirstObject + 12) == 1 &&
            blf_byte(kPairedFirstObject + 32 + 2) == 1 &&
            blf_u32(kPairedFirstObject + 32 + 4) == 0x744 &&
            blf_byte(kPairedFirstObject + 32 + 8) == 0x10 &&
            blf_u32(kPairedFourthObject + 12) == 100 &&
            blf_byte(kPairedFourthPayload + 2) == 0 &&
            blf_u32(kPairedFourthPayload + 4) ==
                (0x80000000U | 0x18DAF1B7U) &&
            blf_byte(kPairedFourthPayload + 20) == 0x30,
        "paired BLF trace changed CAN direction, ID, frame type, or payload");
  blf_writer->stop();
  check(std::filesystem::is_regular_file(blf_path),
        "paired BLF trace did not finalize with the ASC basename");
  blf_writer.reset();

  std::ifstream input(trace_path, std::ios::binary);
  const std::string asc((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
  check(asc.find("Begin Triggerblock") != std::string::npos &&
            asc.find("2 744 Tx d 2 10 01") != std::string::npos &&
            asc.find("2 745 Tx d 2 21 02") != std::string::npos &&
            asc.find("2 746 Tx d 2 22 03") != std::string::npos &&
            asc.find("CANFD 2 Rx 18daf1b7x 1 1 3 3 30 00 0A") !=
                std::string::npos &&
            asc.find("End TriggerBlock") != std::string::npos,
        "ASC trace did not preserve raw classic/FD CAN frames");

  const auto named = uds::make_asc_trace_path(
      std::filesystem::temp_directory_path(), L"changan_c857", L"main",
      L"app_cal");
  const auto file_name = named.filename().wstring();
  check(file_name.starts_with(L"trace_") &&
            file_name.find(L"_changan_c857_main_app_cal") !=
                std::wstring::npos &&
            named.extension() == L".asc" &&
            named.parent_path().filename() == L"flash" &&
            named.parent_path().parent_path().filename() == L"traces",
        "ASC trace filename does not follow the operation naming contract");
  check(uds::make_asc_trace_path(std::filesystem::temp_directory_path(),
                                 L"geely_p416", L"default", L"probe")
                .parent_path()
                .filename() == L"probe" &&
            uds::make_asc_trace_path(std::filesystem::temp_directory_path(),
                                     L"geely_p416", L"default", L"version")
                    .parent_path()
                    .filename() == L"version" &&
            uds::make_asc_trace_path(std::filesystem::temp_directory_path(),
                                     L"geely_p416", L"default",
                                     L"diagnostic")
                    .parent_path()
                    .filename() == L"diagnostic",
        "ASC trace operation directories are not classified");
  std::filesystem::remove(trace_path, ignored);
  std::filesystem::remove(blf_path, ignored);
}

void test_html_report_navigation_and_transfer_aggregation() {
  char* requested_output_value{};
  std::size_t requested_output_size{};
  _dupenv_s(&requested_output_value, &requested_output_size,
            "UDS_REPORT_VALIDATION_DIR");
  const std::string requested_output =
      requested_output_value ? requested_output_value : "";
  std::free(requested_output_value);
  const auto preserve = !requested_output.empty();
  const auto directory = preserve
                             ? std::filesystem::path(requested_output)
                             : std::filesystem::temp_directory_path() /
                                   "uds_html_report_navigation_test";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory);
  const auto asc1 = directory / "cycle_1.asc";
  const auto blf1 = directory / "cycle_1.blf";
  const auto asc2 = directory / "cycle_2.asc";
  const auto blf2 = directory / "cycle_2.blf";
  for (const auto& path : {asc1, blf1, asc2, blf2}) {
    std::ofstream file(path, std::ios::binary);
    file << "trace-evidence";
  }

  uds::HtmlReport report;
  const auto started = std::chrono::system_clock::now();
  auto at = [&](int milliseconds) {
    return started + std::chrono::milliseconds(milliseconds);
  };
  report.add({at(0), "Flash target", "INFO", "Profile=synthetic; Repetitions=2"});
  report.add({at(1), "Pre-flash qualification", "WARN",
              "Status=WARN; optional post-flash version plan unavailable"});
  report.add({at(2), "Flash cycle 1/2", "INFO", "Complete workflow started"});
  report.add({at(3), "[第1/2次] 10 02 ProgrammingSession", "PASS", "50 02"});
  report.add({at(4), "[第1/2次] 27 01 SecurityAccess seed", "PASS", "67 01"});
  report.add({at(5), "[第1/2次] 34 APP RequestDownload", "PASS",
              "address=0x1000; length=0x0008"});
  report.add({at(20), "[第1/2次] 36 APP TransferData", "PASS",
              "blocks=2"});
  report.add({at(21), "[第1/2次] 37 APP RequestTransferExit", "PASS",
              "77"});
  report.add({at(22), "[第1/2次] APP Verification", "PASS", "verified"});
  report.add({at(23), "[第1/2次] DependencyCheck", "PASS", "71 01"});
  report.add({at(24), "[第1/2次] ECUReset", "PASS", "51 01"});
  report.add({at(25), "Flash cycle 1/2", "PASS", "Complete workflow passed"});
  report.add({at(30), "Flash cycle 2/2", "INFO", "Complete workflow started"});
  report.add({at(31), "[第2/2次] 10 02 ProgrammingSession", "PASS", "50 02"});
  report.add({at(32), "[第2/2次] 36 APP TransferData", "PASS", "blocks=1"});
  report.add({at(33), "Flash cycle 2/2", "PASS", "Complete workflow passed"});
  report.add({at(34), "ASC + BLF Trace cycle 1/2", "INFO",
              "Cycle 1/2 raw ASC PASS: " + asc1.string() +
                  "; raw BLF PASS: " + blf1.string()});
  report.add({at(35), "ASC + BLF Trace cycle 2/2", "INFO",
              "Cycle 2/2 raw ASC PASS: " + asc2.string() +
                  "; raw BLF PASS: " + blf2.string()});
  report.add_event({at(36), 2, uds::FlashStage::app_transfer,
                    static_cast<std::uint8_t>(0x36),
                    uds::FlashImageRole::app,
                    "27 SecurityAccess misleading text", "WARN",
                    "STRUCTURED_STAGE_OVERRIDES_TEXT"});

  report.add_transcript({at(10), "Workflow", "INFO",
                         "[第1/2次] 36 APP block 1/2"});
  report.add_transcript({at(11), "Workflow", "INFO",
                         "[第1/2次] TX [0x701] 36 FF 01 02 03 04 [6 bytes]"});
  report.add_transcript({at(12), "Workflow", "INFO",
                         "[第1/2次] RX [0x761] 7F 36 78 | NRC 0x78"});
  report.add_transcript({at(13), "Workflow", "PASS",
                         "[第1/2次] 36 APP block 1/2 PASS: 76 FF"});
  report.add_transcript({at(14), "Workflow", "INFO",
                         "[第1/2次] 36 APP block 2/2"});
  report.add_transcript({at(15), "Workflow", "INFO",
                         "[第1/2次] TX [0x701] 36 00 05 06 07 08 [6 bytes]"});
  report.add_transcript({at(16), "Workflow", "PASS",
                         "[第1/2次] 36 APP block 2/2 PASS: 76 00"});
  report.add_transcript({at(17), "Workflow", "INFO",
                         "RAW_SENTINEL_KEEP_ORDER"});
  report.add_transcript({at(32), "Workflow", "PASS",
                         "[第2/2次] 36 APP block 1/1 PASS: 76 01"});

  const auto path = report.write(directory, "Synthetic Navigation Report");
  std::ifstream input(path, std::ios::binary);
  const std::string html((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  check(html.find("id='report-navigation'") != std::string::npos &&
            html.find("id='summary'") != std::string::npos &&
            html.find("id='configuration'") != std::string::npos &&
            html.find("id='pre-flash-check'") != std::string::npos &&
            html.find("id='app-transfer'") != std::string::npos &&
            html.find("id='cycle-1'") != std::string::npos &&
            html.find("id='cycle-1-app-transfer'") != std::string::npos &&
            html.find("id='cycle-2-app-transfer'") != std::string::npos &&
            html.find("id='trace-evidence'") != std::string::npos &&
            html.find("id='raw-log'") != std::string::npos,
        "HTML report omitted stable or per-cycle navigation anchors");
  check(html.find("成功，但存在警告（PASS with warnings）") !=
                std::string::npos &&
            html.find("NOT_RUN'><span class='symbol'>&mdash;</span>未执行</span><span>4. Driver下载") !=
                std::string::npos &&
            html.find("FF → 00 回绕") != std::string::npos &&
            html.find("展开全部数据块日志") != std::string::npos &&
            html.find("RAW_SENTINEL_KEEP_ORDER") != std::string::npos,
        "HTML report omitted warning result, transfer summary, or raw evidence");
  check(html.find("<details class='transfer-log' open") == std::string::npos &&
            html.find("<details class='raw-log-details' open") ==
                std::string::npos &&
            html.find("<script") == std::string::npos &&
            html.find("cdn") == std::string::npos,
        "HTML report details are not offline-safe or default-collapsed");
  const auto structured_anchor = html.find("id='cycle-2-app-transfer'");
  const auto structured_heading_end = html.find("</h5>", structured_anchor);
  const auto misleading_anchor = html.find("id='cycle-2-security-access'");
  const auto misleading_heading_end = html.find("</h4>", misleading_anchor);
  check(structured_anchor != std::string::npos &&
            structured_heading_end != std::string::npos &&
            html.substr(structured_anchor,
                        structured_heading_end - structured_anchor)
                    .find("status WARN") != std::string::npos &&
            misleading_anchor != std::string::npos &&
            misleading_heading_end != std::string::npos &&
            html.substr(misleading_anchor,
                        misleading_heading_end - misleading_anchor)
                    .find("status NOT_RUN") != std::string::npos,
        "structured report stage did not override misleading log text");

  std::set<std::string> ids;
  std::smatch match;
  const std::regex id_pattern(R"(\sid='([^']+)')");
  for (std::sregex_iterator it(html.begin(), html.end(), id_pattern), end;
       it != end; ++it) {
    check(ids.insert((*it)[1].str()).second,
          "HTML report generated a duplicate id");
  }
  const std::regex href_pattern(R"(href='#([^']+)')");
  for (std::sregex_iterator it(html.begin(), html.end(), href_pattern), end;
       it != end; ++it) {
    check(ids.contains((*it)[1].str()),
          "HTML report navigation link has no target anchor");
  }
  if (!preserve) std::filesystem::remove_all(directory, ignored);
}

void test_bus_monitor_trace_session() {
  const auto directory = std::filesystem::temp_directory_path() /
                         "uds_bus_monitor_trace_session_test";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory);

  const auto abandoned = directory / "bus_monitor_abandoned_CH2.blf.partial";
  std::uintmax_t expected_recovered_size{};
  {
    uds::BusMonitorTraceSession interrupted_source(directory);
    check(interrupted_source.start(2),
          "BLF recovery fixture could not be started");
    interrupted_source.append(
        uds::CanFrame{0x772, {0x10, 0x01}, false, false, false});
    std::string valid_blf;
    check(interrupted_source.export_snapshot(
              [&valid_blf](std::string_view chunk) {
                valid_blf.append(chunk);
                return true;
              }),
          "BLF recovery fixture could not be exported");
    expected_recovered_size = valid_blf.size();
    std::ofstream output(abandoned, std::ios::binary | std::ios::trunc);
    output.write(valid_blf.data(),
                 static_cast<std::streamsize>(valid_blf.size()));
    output << "torn-container";
    interrupted_source.stop();
  }

  uds::BusMonitorTraceSession session(directory);
  const auto recovery = session.recover_incomplete();
  check(recovery.recovered == 1 && recovery.failed == 0 &&
            !std::filesystem::exists(abandoned) &&
            std::filesystem::exists(
                directory / "bus_monitor_abandoned_CH2.blf") &&
            std::filesystem::file_size(
                directory / "bus_monitor_abandoned_CH2.blf") ==
                expected_recovered_size,
        "abandoned bus-monitor BLF trace was not recovered");

  check(session.start(3) && session.is_active() &&
            session.path().filename().string().ends_with(".blf.partial"),
        "bus-monitor trace session did not create a partial BLF file");
  session.append(
      uds::CanFrame{0x744, {0x10, 0x01}, false, false, false, true});
  session.append(uds::CanFrame{0x18DAF1B7,
                               {0x30, 0x00, 0x0A},
                               true,
                               true,
                               true,
                               false});
  session.flush();
  std::string snapshot;
  check(session.export_snapshot([&snapshot](std::string_view chunk) {
          snapshot.append(chunk);
          return true;
        }),
        "active bus-monitor BLF snapshot could not be exported");
  const auto byte = [&snapshot](std::size_t offset) {
    return static_cast<std::uint8_t>(snapshot.at(offset));
  };
  const auto u32 = [&byte](std::size_t offset) {
    return static_cast<std::uint32_t>(byte(offset)) |
           (static_cast<std::uint32_t>(byte(offset + 1)) << 8U) |
           (static_cast<std::uint32_t>(byte(offset + 2)) << 16U) |
           (static_cast<std::uint32_t>(byte(offset + 3)) << 24U);
  };
  constexpr std::size_t kFirstObject = 144 + 16 + 16;
  constexpr std::size_t kSecondObject = kFirstObject + 48;
  constexpr std::size_t kSecondPayload = kSecondObject + 32;
  check(session.frame_count() == 2 && snapshot.starts_with("LOGG") &&
            snapshot.size() >= kSecondPayload + 84 &&
            u32(kFirstObject + 12) == 1 &&
            byte(kFirstObject + 32 + 2) == 1 &&
            u32(kFirstObject + 32 + 4) == 0x744 &&
            byte(kFirstObject + 32 + 8) == 0x10 &&
            u32(kSecondObject + 12) == 100 &&
            byte(kSecondPayload + 2) == 0 &&
            u32(kSecondPayload + 4) == (0x80000000U | 0x18DAF1B7U) &&
            byte(kSecondPayload + 13) == 3 &&
            byte(kSecondPayload + 14) == 3 &&
            byte(kSecondPayload + 20) == 0x30 &&
            byte(kSecondPayload + 21) == 0x00 &&
            byte(kSecondPayload + 22) == 0x0A,
        "active bus-monitor BLF snapshot was incomplete");

  session.stop();
  check(!session.is_active() &&
            session.path().filename().string().ends_with(".blf") &&
            std::filesystem::exists(session.path()),
        "bus-monitor trace session did not finalize its BLF file");
  std::ifstream input(session.path(), std::ios::binary);
  std::array<char, 4> signature{};
  input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
  check(signature == std::array<char, 4>{'L', 'O', 'G', 'G'},
        "finalized bus-monitor BLF trace has an invalid signature");

  std::filesystem::remove_all(directory, ignored);
}

void test_can_id_filter() {
  const auto parsed = uds::parse_can_id_filter(
      "772、7DF 700-70F，18DAxxxx !705,!18DAF1B6");
  const auto* filter = std::get_if<uds::CanIdFilter>(&parsed);
  check(filter && filter->matches(0x772) && filter->matches(0x7DF) &&
            filter->matches(0x704) && !filter->matches(0x705) &&
            filter->matches(0x18DA1234) &&
            !filter->matches(0x18DAF1B6) && !filter->matches(0x123),
        "CAN ID filter exact/range/mask/exclusion grammar mismatch");

  const auto only_exclusion = uds::parse_can_id_filter("!520");
  const auto* exclusion_filter =
      std::get_if<uds::CanIdFilter>(&only_exclusion);
  check(exclusion_filter && !exclusion_filter->matches(0x520) &&
            exclusion_filter->matches(0x521),
        "CAN ID exclusion-only filter must include all other IDs");

  const auto empty = uds::parse_can_id_filter("  ， 、  ");
  const auto* empty_filter = std::get_if<uds::CanIdFilter>(&empty);
  check(empty_filter && empty_filter->matches(0x123) &&
            empty_filter->matches(0x18DAF1B6),
        "empty CAN ID filter must match all valid IDs");

  for (const auto expression : {"7FF-700", "18DGxxxx", "20000000", "!"}) {
    check(std::holds_alternative<uds::CanIdFilterError>(
              uds::parse_can_id_filter(expression)),
          std::string("invalid CAN ID filter was accepted: ") + expression);
  }
}

void test_uds_single_frame() {
  MockBus bus;
  bus.rx.push_back(frame({0x02, 0x50, 0x01, 0, 0, 0, 0, 0}));
  uds::IsoTpSession tp(bus, {0x772, 0x77A, 0x55});
  uds::UdsClient client(tp);
  const std::array<std::uint8_t, 2> request{0x10, 0x01};
  const auto response = client.request(request);

  check(response.success, "positive UDS response was not accepted");
  check(response.response == std::vector<std::uint8_t>({0x50, 0x01}),
        "positive UDS payload mismatch");
  check(bus.sent.size() == 1, "single-frame request sent unexpected frame count");
  check(bus.sent.front().id == 0x772, "single-frame request used wrong CAN ID");
  check(bus.sent.front().data ==
            std::vector<std::uint8_t>({0x02, 0x10, 0x01, 0x55, 0x55, 0x55, 0x55, 0x55}),
        "single-frame request padding mismatch");
}

void test_uds_response_pending_and_nrc() {
  MockBus pending_bus;
  pending_bus.rx.push_back(frame({0x03, 0x7F, 0x31, 0x78, 0, 0, 0, 0}));
  pending_bus.rx.push_back(frame({0x04, 0x71, 0x01, 0xFF, 0x00, 0, 0, 0}));
  uds::IsoTpSession pending_tp(pending_bus, {0x772, 0x77A, 0x55});
  uds::UdsClient pending_client(pending_tp);
  const std::array<std::uint8_t, 4> routine{0x31, 0x01, 0xFF, 0x00};
  const auto pending_result = pending_client.request(routine);
  check(pending_result.success, "UDS ResponsePending did not continue to positive response");
  check(pending_result.response == std::vector<std::uint8_t>({0x71, 0x01, 0xFF, 0x00}),
        "UDS response after pending mismatch");

  MockBus nrc_bus;
  nrc_bus.rx.push_back(frame({0x03, 0x7F, 0x27, 0x35, 0, 0, 0, 0}));
  uds::IsoTpSession nrc_tp(nrc_bus, {0x772, 0x77A, 0x55});
  std::vector<std::string> nrc_logs;
  uds::UdsClient nrc_client(
      nrc_tp, [&nrc_logs](const std::string& line) { nrc_logs.push_back(line); });
  const std::array<std::uint8_t, 2> key{0x27, 0x12};
  const auto nrc_result = nrc_client.request(key);
  check(!nrc_result.success, "negative UDS response was accepted as success");
  check(nrc_result.nrc == 0x35, "negative UDS response lost NRC value");
  check(nrc_result.detail.find("NRC 0x35 InvalidKey") != std::string::npos &&
            !nrc_logs.empty() &&
            nrc_logs.back().find("NRC 0x35 InvalidKey") != std::string::npos,
        "negative UDS response did not retain the concrete NRC meaning");
}

void test_chuneng_ft_pending_switches_to_selected_app_response() {
  using namespace std::chrono_literals;
  for (const auto app_response_id : {0x72DU, 0x72FU}) {
    MockBus bus;
    bus.rx.push_back(uds::CanFrame{
        0x761, {0x03, 0x7F, 0x10, 0x78, 0, 0, 0, 0},
        false, false, false});
    bus.rx.push_back(uds::CanFrame{
        app_response_id,
        {0x06, 0x50, 0x02, 0x00, 0x32, 0x01, 0x5E, 0x00},
        false, true, false});

    uds::IsoTpConfig config{0x701, 0x761, 0x55};
    config.alternate_rx_id = app_response_id;
    uds::IsoTpSession transport(bus, config);
    std::vector<std::string> logs;
    uds::UdsClient client(
        transport,
        [&logs](const std::string& line) { logs.push_back(line); });
    const auto result = client.request(
        std::array<std::uint8_t, 2>{0x10, 0x02}, 2ms, 2ms);

    check(result.success &&
              result.response == std::vector<std::uint8_t>(
                                     {0x50, 0x02, 0x00, 0x32, 0x01, 0x5E}) &&
              transport.last_rx_id() == app_response_id,
          "ChuNeng FT transition did not accept 50 02 on the selected APP response ID");
    const auto app_id_was_logged = std::any_of(
        logs.begin(), logs.end(), [app_response_id](const std::string& line) {
          const auto expected = app_response_id == 0x72DU
                                    ? std::string{"RX [0x72D] 50 02"}
                                    : std::string{"RX [0x72F] 50 02"};
          return line.find(expected) != std::string::npos;
        });
    check(app_id_was_logged,
          "ChuNeng FT transition log lost the final APP response ID");
  }

  MockBus wrong_side_bus;
  wrong_side_bus.rx.push_back(uds::CanFrame{
      0x761, {0x03, 0x7F, 0x10, 0x78, 0, 0, 0, 0},
      false, false, false});
  wrong_side_bus.rx.push_back(uds::CanFrame{
      0x72D, {0x06, 0x50, 0x02, 0x00, 0x32, 0x01, 0x5E, 0x00},
      false, true, false});
  uds::IsoTpConfig left_config{0x701, 0x761, 0x55};
  left_config.alternate_rx_id = 0x72F;
  uds::IsoTpSession left_transport(wrong_side_bus, left_config);
  uds::UdsClient left_client(left_transport);
  bool wrong_side_rejected = false;
  try {
    static_cast<void>(left_client.request(
        std::array<std::uint8_t, 2>{0x10, 0x02}, 2ms, 2ms));
  } catch (const std::runtime_error& error) {
    wrong_side_rejected =
        std::string_view(error.what()).find("ISO-TP receive timeout") !=
        std::string_view::npos;
  }
  check(wrong_side_rejected,
        "ChuNeng left-rear FT transition accepted the right-rear response ID");
}

void test_uds_observe_consumes_non_gating_responses() {
  using namespace std::chrono_literals;

  MockBus negative_bus;
  negative_bus.rx.push_back(
      frame({0x03, 0x7F, 0x31, 0x13, 0, 0, 0, 0}));
  uds::IsoTpSession negative_tp(negative_bus, {0x772, 0x77A, 0x55});
  std::vector<std::string> logs;
  uds::UdsClient negative_client(
      negative_tp, [&logs](const std::string& line) { logs.push_back(line); });
  const std::array<std::uint8_t, 4> certificate{
      0x31, 0x01, 0x60, 0x00};
  const auto negative =
      negative_client.request_observe(certificate, 10ms, 20ms);
  check(negative.kind == uds::UdsObservationKind::negative &&
            negative.nrc == 0x13 && negative_bus.rx.empty() &&
            !logs.empty() && logs.back().find("NRC 0x13") != std::string::npos,
        "non-gating UDS observation did not consume/log the final NRC");

  // Reproduce the LP-ARF failure shape: a delayed 31 response must be consumed
  // before the following multi-frame 2E request waits for FlowControl.
  negative_bus.rx.push_back(frame({0x30, 0x00, 0x00, 0, 0, 0, 0, 0}));
  negative_bus.rx.push_back(frame({0x03, 0x6E, 0xF1, 0x98, 0, 0, 0, 0}));
  std::vector<std::uint8_t> fingerprint{0x2E, 0xF1, 0x98};
  fingerprint.resize(19, 0);
  const auto fingerprint_response = negative_client.request(fingerprint);
  check(fingerprint_response.success &&
            fingerprint_response.response ==
                std::vector<std::uint8_t>({0x6E, 0xF1, 0x98}),
        "consumed certificate NRC contaminated the next multi-frame request");

  MockBus positive_bus;
  positive_bus.rx.push_back(
      frame({0x05, 0x71, 0x01, 0x60, 0x01, 0x04, 0, 0}));
  uds::IsoTpSession positive_tp(positive_bus, {0x772, 0x77A, 0x55});
  uds::UdsClient positive_client(positive_tp);
  const std::array<std::uint8_t, 4> verify{0x31, 0x01, 0x60, 0x01};
  const auto positive = positive_client.request_observe(verify, 10ms, 20ms);
  check(positive.kind == uds::UdsObservationKind::positive &&
            positive.response ==
                std::vector<std::uint8_t>({0x71, 0x01, 0x60, 0x01, 0x04}),
        "non-gating UDS observation lost a positive response");

  MockBus timeout_bus;
  uds::IsoTpSession timeout_tp(timeout_bus, {0x772, 0x77A, 0x55});
  uds::UdsClient timeout_client(timeout_tp);
  const auto timeout = timeout_client.request_observe(verify, 2ms, 4ms);
  check(timeout.kind == uds::UdsObservationKind::timeout &&
            timeout.response.empty() && timeout_bus.sent.size() == 1,
        "non-gating UDS observation did not return a clean timeout");

  MockBus pending_bus;
  pending_bus.rx.push_back(
      frame({0x03, 0x7F, 0x31, 0x78, 0, 0, 0, 0}));
  pending_bus.rx.push_back(
      frame({0x03, 0x7F, 0x31, 0x13, 0, 0, 0, 0}));
  uds::IsoTpSession pending_tp(pending_bus, {0x772, 0x77A, 0x55});
  uds::UdsClient pending_client(pending_tp);
  const auto pending =
      pending_client.request_observe(certificate, 10ms, 20ms);
  check(pending.kind == uds::UdsObservationKind::negative &&
            pending.nrc == 0x13 && pending_bus.rx.empty(),
        "non-gating UDS observation did not consume the final response after NRC78");
}

void test_uds_nrc_diagnostics() {
  const std::array required_nrcs{
      0x10U, 0x11U, 0x12U, 0x13U, 0x21U, 0x22U, 0x24U,
      0x31U, 0x33U, 0x35U, 0x36U, 0x37U, 0x70U, 0x71U,
      0x72U, 0x73U, 0x78U, 0x7EU, 0x7FU};
  for (const auto nrc : required_nrcs) {
    const auto detail = uds::format_uds_nrc(static_cast<std::uint8_t>(nrc));
    check(detail.find("UnknownNRC") == std::string::npos &&
              detail.find("NRC 0x") == 0,
          "required NRC is missing its maintained explanation");
  }
  check(uds::format_uds_nrc(0x31) ==
            "NRC 0x31 RequestOutOfRange（请求超出范围：当前例程/参数不支持）",
        "NRC31 operator explanation changed unexpectedly");

  const std::array<std::uint8_t, 8> failure_frame{
      0x03, 0x7F, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00};
  const auto failure =
      uds::parse_isotp_single_frame_negative_response(failure_frame);
  check(failure && failure->request_sid == 0x31 && failure->nrc == 0x31 &&
            failure->kind == uds::UdsNegativeResponseKind::failure,
        "classic CAN NRC31 single frame was not classified as final failure");

  const std::array<std::uint8_t, 8> pending_frame{
      0x03, 0x7F, 0x36, 0x78, 0x00, 0x00, 0x00, 0x00};
  const auto pending =
      uds::parse_isotp_single_frame_negative_response(pending_frame);
  check(pending && pending->request_sid == 0x36 && pending->nrc == 0x78 &&
            pending->kind == uds::UdsNegativeResponseKind::pending,
        "NRC78 was incorrectly classified as a final failure");

  const std::array<std::uint8_t, 8> can_fd_escape_frame{
      0x00, 0x03, 0x7F, 0x27, 0x35, 0x00, 0x00, 0x00};
  const auto can_fd_failure =
      uds::parse_isotp_single_frame_negative_response(can_fd_escape_frame);
  check(can_fd_failure && can_fd_failure->request_sid == 0x27 &&
            can_fd_failure->nrc == 0x35,
        "CAN FD escape-length negative response was not recognized");

  const std::array<std::uint8_t, 8> positive_frame{
      0x02, 0x50, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
  check(!uds::parse_isotp_single_frame_negative_response(positive_frame),
        "positive response was incorrectly classified as an NRC");

  const std::array<std::uint8_t, 8> routine_failure_frame{
      0x05, 0x71, 0x01, 0x02, 0x02, 0x05, 0x55, 0x55};
  const auto routine_failure =
      uds::parse_isotp_single_frame_routine_result(routine_failure_frame);
  check(routine_failure && routine_failure->routine_id == 0x0202 &&
            routine_failure->status == 0x05 && routine_failure->failure &&
            uds::format_uds_routine_result(*routine_failure).find(
                "数据/软件签名校验") != std::string::npos,
        "positive RoutineControl status 05 was not explained as failure");
  const std::array<std::uint8_t, 8> routine_pass_frame{
      0x05, 0x71, 0x01, 0x02, 0x02, 0x04, 0x55, 0x55};
  const auto routine_pass =
      uds::parse_isotp_single_frame_routine_result(routine_pass_frame);
  check(routine_pass && !routine_pass->failure,
        "positive RoutineControl status 04 was not retained as pass");
  const std::array<std::uint8_t, 8> precondition_warning_frame{
      0x05, 0x71, 0x01, 0x02, 0x03, 0x05, 0x55, 0x55};
  const auto precondition_warning =
      uds::parse_isotp_single_frame_routine_result(
          precondition_warning_frame);
  check(precondition_warning && !precondition_warning->failure &&
            uds::format_uds_routine_result(*precondition_warning).find(
                "WARN") != std::string::npos,
        "ARC331 routine 0203 status 05 was misclassified as final failure");
}

void test_isotp_multiframe_receive() {
  MockBus bus;
  bus.rx.push_back(frame({0x10, 0x0A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
  bus.rx.push_back(frame({0x21, 0x07, 0x08, 0x09, 0x0A, 0x55, 0x55, 0x55}));
  uds::IsoTpSession tp(bus, {0x772, 0x77A, 0x55});
  const auto payload = tp.receive(std::chrono::milliseconds(10));

  check(payload == std::vector<std::uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}),
        "ISO-TP multi-frame receive payload mismatch");
  check(bus.sent.size() == 1, "ISO-TP receive did not send exactly one flow-control frame");
  check(bus.sent.front().data ==
            std::vector<std::uint8_t>({0x30, 0x00, 0x0A, 0, 0, 0, 0, 0}),
        "ISO-TP receive flow-control frame mismatch");
}

void test_isotp_multiframe_send() {
  MockBus bus;
  bus.rx.push_back(frame({0x30, 0x00, 0x00, 0, 0, 0, 0, 0}));
  uds::IsoTpSession tp(bus, {0x772, 0x77A, 0x55});
  const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  tp.send(payload);

  check(bus.sent.size() == 2, "ISO-TP multi-frame request sent wrong frame count");
  check(bus.sent[0].data ==
            std::vector<std::uint8_t>({0x10, 0x0A, 1, 2, 3, 4, 5, 6}),
        "ISO-TP first frame mismatch");
  check(bus.sent[1].data ==
            std::vector<std::uint8_t>({0x21, 7, 8, 9, 10, 0x55, 0x55, 0x55}),
        "ISO-TP consecutive frame mismatch");
}

void test_isotp_reorders_adjacent_consecutive_frames() {
  MockBus bus;
  bus.rx.push_back(frame(
      {0x10, 0x14, 0x62, 0xF1, 0x87, 0x33, 0x36, 0x30}));
  bus.rx.push_back(frame(
      {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
  bus.rx.push_back(frame(
      {0x21, 0x32, 0x2D, 0x34, 0x30, 0x30, 0x31, 0x37}));
  uds::IsoTpSession transport(bus, {0x772, 0x77A, 0x55});
  const auto payload = transport.receive(std::chrono::milliseconds(10));

  check(payload ==
            std::vector<std::uint8_t>({0x62, 0xF1, 0x87, 0x33, 0x36, 0x30,
                                       0x32, 0x2D, 0x34, 0x30, 0x30, 0x31,
                                       0x37, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00}),
        "ISO-TP did not restore adjacent out-of-order consecutive frames");
}

void test_isotp_drains_stale_receive_queue_before_request() {
  MockBus bus;
  bus.rx.push_back(
      uds::CanFrame{0x18FF3AB7, {0xA3, 0x01, 0x10, 0x10}, true, false, false});
  bus.rx.push_back(
      uds::CanFrame{0x18DAF1B7, {0x22, 0, 0, 0, 0, 0, 0, 0},
                    true, false, false});
  bus.inject_after_send.push_back(
      uds::CanFrame{0x18DAF1B7,
                    {0x06, 0x62, 0xF1, 0x87, 0x33, 0x36, 0x30, 0},
                    true, false, false});

  uds::IsoTpConfig config{
      0x18DAB7F1, 0x18DAF1B7, 0xCC, 0, 0,
      std::chrono::milliseconds(1000), std::chrono::milliseconds(1000),
      true, true, true, true};
  config.drain_receive_before_send = true;
  uds::IsoTpSession transport(bus, config);
  uds::UdsClient client(transport);
  const std::array<std::uint8_t, 3> request{0x22, 0xF1, 0x87};
  const auto response = client.request(request);

  check(response.success &&
            response.response ==
                std::vector<std::uint8_t>({0x62, 0xF1, 0x87, 0x33, 0x36,
                                           0x30}),
        "ISO-TP stale receive drain did not preserve the new response");
  check(bus.rx.empty() && bus.sent.size() == 1,
        "ISO-TP stale receive drain left old frames in front of the request");
}

void test_uds_wait_can_be_cancelled() {
  using namespace std::chrono_literals;
  MockBus bus;
  bus.rx.push_back(frame({0x03, 0x7F, 0x31, 0x78, 0, 0, 0, 0}));
  uds::IsoTpSession tp(bus, {0x772, 0x77A, 0x55});
  std::stop_source stop_source;
  uds::UdsClient client(tp, {}, stop_source.get_token());
  std::jthread stopper([&stop_source] {
    std::this_thread::sleep_for(25ms);
    stop_source.request_stop();
  });
  const std::array<std::uint8_t, 4> routine{0x31, 0x01, 0xFF, 0x00};
  const auto started = std::chrono::steady_clock::now();
  bool cancelled = false;
  try {
    (void)client.request(routine, 2s, 15s);
  } catch (const std::exception& error) {
    cancelled =
        std::string_view(error.what()).find("operation cancelled by user") !=
        std::string_view::npos;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  check(cancelled, "UDS ResponsePending wait ignored cancellation");
  check(elapsed < 500ms, "UDS cancellation waited for P2* timeout");
}

void test_isotp_mixed_can_fd_adaptation() {
  MockBus request_bus;
  const uds::CanFrame classic_fc{
      0x18DAF1B7, {0x30, 0x00, 0x00, 0, 0, 0, 0, 0},
      true, false, false};
  request_bus.rx.push_back(classic_fc);
  request_bus.rx.push_back(classic_fc);
  uds::IsoTpConfig request_config{
      0x18DAB7F1, 0x18DAF1B7, 0xCC, 0, 0,
      std::chrono::milliseconds(1000), std::chrono::milliseconds(1000),
      true, true, true, true};
  request_config.adapt_consecutive_frames_to_flow_control = true;
  uds::IsoTpSession request_tp(request_bus, request_config);
  const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  request_tp.send(payload);
  request_tp.send(payload);

  check(request_bus.sent.size() == 4,
        "adaptive ISO-TP request sent wrong frame count");
  check(request_bus.sent[0].fd && request_bus.sent[0].brs &&
            !request_bus.sent[1].fd && !request_bus.sent[1].brs &&
            request_bus.sent[2].fd && request_bus.sent[2].brs &&
            !request_bus.sent[3].fd && !request_bus.sent[3].brs,
        "FD FirstFrame did not adapt its CF to Classic FC or reset on next request");

  MockBus response_bus;
  response_bus.rx.push_back(
      {0x18DAF1B7, {0x10, 0x0A, 1, 2, 3, 4, 5, 6},
       true, false, false});
  response_bus.rx.push_back(
      {0x18DAF1B7, {0x21, 7, 8, 9, 10, 0, 0, 0},
       true, false, false});
  auto response_config = request_config;
  response_config.adapt_flow_control_to_first_frame = true;
  response_config.flow_control_delay = std::chrono::milliseconds(0);
  uds::IsoTpSession response_tp(response_bus, response_config);
  check(response_tp.receive(std::chrono::milliseconds(10)) == payload,
        "adaptive ISO-TP response payload mismatch");
  check(response_bus.sent.size() == 1 && !response_bus.sent[0].fd &&
            !response_bus.sent[0].brs &&
            response_bus.sent[0].data ==
                std::vector<std::uint8_t>({0x30, 0x00, 0x00, 0, 0, 0, 0, 0}),
         "Classic ECU FirstFrame did not receive Classic 30 00 00 FlowControl");

  MockBus block_bus;
  block_bus.rx.push_back(
      {0x18DAF1B7, {0x30, 0x01, 0x00, 0, 0, 0, 0, 0},
       true, false, false});
  block_bus.rx.push_back(
      {0x18DAF1B7, {0x30, 0x02, 0x05, 0, 0, 0, 0, 0},
       true, true, true});
  block_bus.rx.push_back(
      {0x18DAF1B7, {0x30, 0x00, 0x00, 0, 0, 0, 0, 0},
       true, false, false});
  uds::IsoTpSession block_tp(block_bus, request_config);
  std::vector<std::uint8_t> block_payload(34, 0xA5);
  const auto started = std::chrono::steady_clock::now();
  block_tp.send(block_payload);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  check(block_bus.rx.empty() && block_bus.sent.size() == 5,
        "adaptive ISO-TP did not apply updated secondary FC block sizes");
  check(block_bus.sent[0].fd && !block_bus.sent[1].fd &&
            block_bus.sent[2].fd && block_bus.sent[3].fd &&
            !block_bus.sent[4].fd,
        "adaptive ISO-TP did not apply each secondary FC frame format");
  check(elapsed >= std::chrono::milliseconds(8),
        "adaptive ISO-TP did not apply secondary FC STmin");

  const auto rejects_secondary_fc = [&](std::vector<std::uint8_t> invalid) {
    MockBus invalid_bus;
    invalid_bus.rx.push_back(
        {0x18DAF1B7, {0x30, 0x01, 0x00, 0, 0, 0, 0, 0},
         true, false, false});
    invalid_bus.rx.push_back(
        {0x18DAF1B7, std::move(invalid), true, false, false});
    uds::IsoTpSession invalid_tp(invalid_bus, request_config);
    try {
      invalid_tp.send(std::vector<std::uint8_t>(14, 0x5A));
      return false;
    } catch (const std::runtime_error&) {
      return true;
    }
  };
  check(rejects_secondary_fc({0x30, 0x00}),
        "adaptive ISO-TP accepted a short secondary FC");
  check(rejects_secondary_fc({0x20, 0x00, 0x00}),
        "adaptive ISO-TP accepted a non-FC secondary PCI");
}

void test_isotp_can_fd_64_byte_frames() {
  uds::IsoTpConfig config{
      0x7A4, 0x7AC, 0x00, 0, 0,
      std::chrono::milliseconds(1000),
      std::chrono::milliseconds(1000),
      false, false, true, false};
  config.tx_data_length = 64;
  config.batch_consecutive_frames = false;

  MockBus single_bus;
  uds::IsoTpSession single_tp(single_bus, config);
  std::vector<std::uint8_t> single_payload(13);
  for (std::size_t index = 0; index < single_payload.size(); ++index) {
    single_payload[index] = static_cast<std::uint8_t>(index + 1U);
  }
  single_tp.send(single_payload);
  check(single_bus.sent.size() == 1U &&
            single_bus.sent[0].fd && !single_bus.sent[0].brs &&
            single_bus.sent[0].data.size() == 16U &&
            single_bus.sent[0].data[0] == 0x00 &&
            single_bus.sent[0].data[1] == 0x0D &&
            std::equal(single_payload.begin(), single_payload.end(),
                       single_bus.sent[0].data.begin() + 2),
        "CAN FD escape-length SingleFrame does not match captured 16-byte format");

  MockBus multi_bus;
  multi_bus.rx.push_back(
      uds::CanFrame{0x7AC,
                    {0x30, 0x00, 0x00, 0, 0, 0, 0, 0},
                    false, true, true});
  uds::IsoTpSession multi_tp(multi_bus, config);
  std::vector<std::uint8_t> multi_payload(0x402);
  for (std::size_t index = 0; index < multi_payload.size(); ++index) {
    multi_payload[index] = static_cast<std::uint8_t>(index);
  }
  multi_tp.send(multi_payload);
  check(multi_bus.sent.size() == 17U &&
            multi_bus.sent[0].data.size() == 64U &&
            multi_bus.sent[0].data[0] == 0x14 &&
            multi_bus.sent[0].data[1] == 0x02 &&
            multi_bus.sent[1].data.size() == 64U &&
            multi_bus.sent[1].data[0] == 0x21 &&
            multi_bus.sent.back().data.size() == 20U &&
            multi_bus.sent.back().data[0] == 0x20,
        "CAN FD 64-byte First/Consecutive Frame segmentation mismatch");

  MockBus receive_bus;
  std::vector<std::uint8_t> escaped(16, 0x00);
  escaped[0] = 0x00;
  escaped[1] = 0x0D;
  std::copy(single_payload.begin(), single_payload.end(),
            escaped.begin() + 2);
  receive_bus.rx.push_back(
      uds::CanFrame{0x7AC, escaped, false, true, true});
  uds::IsoTpSession receive_tp(receive_bus, config);
  check(receive_tp.receive(std::chrono::milliseconds(10)) ==
            single_payload,
        "CAN FD escape-length SingleFrame receive mismatch");
}

void test_flash_data() {
  const auto temp = std::filesystem::temp_directory_path() / "uds_cpp_core_test";
  std::filesystem::remove_all(temp);
  std::filesystem::create_directories(temp);
  try {
    {
      std::ofstream srec(temp / "sample.s19");
      srec << "S107000001020304EE\n";
    }
    const auto image = uds::load_srecord_window(temp / "sample.s19", 0, 8);
    check(image == std::vector<std::uint8_t>({1, 2, 3, 4, 0xFF, 0xFF, 0xFF, 0xFF}),
          "S-record window or FF fill mismatch");
    const auto automatic =
        uds::load_single_srecord_segment(temp / "sample.s19");
    check(automatic.address == 0 &&
              automatic.data ==
                  std::vector<std::uint8_t>({1, 2, 3, 4}),
          "S-record automatic single-segment layout mismatch");
    {
      std::ofstream asc(temp / "sample.asc");
      asc << "AA BB CC DD";
    }
    const auto verify = uds::load_asc_hex(temp / "sample.asc", 4, 4);
    check(verify == std::vector<std::uint8_t>({0xAA, 0xBB, 0xCC, 0xDD}),
          "ASC verification payload mismatch");
    {
      std::ofstream srec(temp / "filtered.s19");
      srec << "S107000001020304EE\n";
      srec << "S1050010AABB85\n";
    }
    const auto filtered = uds::load_srecord_window_filtered(temp / "filtered.s19", 0x10, 4);
    check(filtered == std::vector<std::uint8_t>({0xAA, 0xBB, 0xFF, 0xFF}),
          "filtered S-record window mismatch");
    const auto segmented =
        uds::load_srecord_image(temp / "filtered.s19");
    check(segmented.segments.size() == 2U &&
              segmented.payload_size == 6U &&
              segmented.segments[0].address == 0 &&
              segmented.segments[1].address == 0x10,
          "S-record automatic multi-segment analysis mismatch");
    {
      std::ofstream rsa(temp / "sample.rsa");
      rsa << "0xAA, 0xBB, 0xCC, 0xDD";
    }
    const auto rsa = uds::load_hex_bytes(temp / "sample.rsa", 4, 4);
    check(rsa == std::vector<std::uint8_t>({0xAA, 0xBB, 0xCC, 0xDD}),
          "RSA hexadecimal byte parsing mismatch");
    const auto crc16 = [](std::span<const std::uint8_t> data) {
      std::uint16_t crc = 0xFFFFU;
      for (const auto byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
          crc = (crc & 0x8000U) != 0U
                    ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                    : static_cast<std::uint16_t>(crc << 1U);
        }
      }
      return crc;
    };
    const auto crc32 = [](std::span<const std::uint8_t> data) {
      std::uint32_t crc = 0xFFFFFFFFU;
      for (const auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
          crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
      }
      return crc ^ 0xFFFFFFFFU;
    };
    const auto append_be32 = [](std::vector<std::uint8_t>& destination,
                                std::uint32_t value) {
      for (const auto shift : {24U, 16U, 8U, 0U}) {
        destination.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    };
    const auto append_block = [&](std::vector<std::uint8_t>& destination,
                                  std::uint32_t address,
                                  std::span<const std::uint8_t> payload) {
      append_be32(destination, address);
      append_be32(destination, static_cast<std::uint32_t>(payload.size()));
      destination.insert(destination.end(), payload.begin(), payload.end());
      const auto crc = crc16(payload);
      destination.push_back(static_cast<std::uint8_t>(crc >> 8U));
      destination.push_back(static_cast<std::uint8_t>(crc));
    };
    const auto byte_hex = [](std::span<const std::uint8_t> bytes) {
      return uds::to_hex(bytes, false);
    };
    const std::vector<std::uint8_t> cbf_main{0x01, 0x02, 0x03, 0x04};
    std::vector<std::uint8_t> cbf_abt{0x00, 0x00, 0x00, 0x01};
    append_be32(cbf_abt, 0x000C0000U);
    append_be32(cbf_abt, static_cast<std::uint32_t>(cbf_main.size()));
    const auto main_hash = uds::sha256(cbf_main);
    cbf_abt.insert(cbf_abt.end(), main_hash.begin(), main_hash.end());
    check(cbf_abt.size() == 0x2CU, "CBF fixture ABT size is invalid");
    std::vector<std::uint8_t> cbf_body;
    append_block(cbf_body, 0x000C0000U, cbf_main);
    const std::array<std::uint8_t, 8> abt_header{0x00, 0x0C, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x2C};
    std::vector<std::uint8_t> abt_hash_input(abt_header.begin(), abt_header.end());
    abt_hash_input.insert(abt_hash_input.end(), cbf_abt.begin(), cbf_abt.end());
    const auto abt_hash = uds::sha256(abt_hash_input);
    append_block(cbf_body, 0x000C0000U, cbf_abt);
    const std::vector<std::uint8_t> signature(256U, 0x5AU);
    const auto cbf_path = temp / "sample.cbf";
    {
      std::ofstream cbf(cbf_path, std::ios::binary);
      cbf << "cbf_version=1.0;\ncbf_header = {\n"
          << "  sw_id=\"TEST\";\n  sw_version=\"01\";\n  sw_type=\"DATA\";\n"
          << "  data_format_id=0x00;\n  ecu_address=0x072E;\n"
          << "  erase_range={(0x000C0000, 0x00000004)};\n"
          << "  abt_start=0x000C0000;\n  abt_length=0x0000002C;\n"
          << "  abt_hash=0x" << byte_hex(abt_hash) << ";\n"
          << "  dev_signature=0x" << byte_hex(signature) << ";\n"
          << "  cbf_checksum=0x" << std::hex << std::uppercase << crc32(cbf_body) << ";\n}\n";
      cbf.write(reinterpret_cast<const char*>(cbf_body.data()),
                static_cast<std::streamsize>(cbf_body.size()));
    }
    const auto cbf = uds::load_chuneng_cbf(cbf_path);
    check(cbf.software_type == "DATA" && cbf.main.address == 0x000C0000U &&
              cbf.main.data == cbf_main && cbf.abt.data == cbf_abt &&
              cbf.device_signature == signature,
          "CBF main data, ABT, or signature extraction mismatch");
    auto damaged = cbf_body;
    damaged.back() ^= 0x01U;
    {
      std::ofstream damaged_cbf(cbf_path, std::ios::binary | std::ios::trunc);
      damaged_cbf << "cbf_version=1.0;\ncbf_header = {\n"
          << "  sw_id=\"TEST\";\n  sw_version=\"01\";\n  sw_type=\"DATA\";\n"
          << "  data_format_id=0x00;\n  ecu_address=0x072E;\n"
          << "  abt_start=0x000C0000;\n  abt_length=0x0000002C;\n"
          << "  abt_hash=0x" << byte_hex(abt_hash) << ";\n"
          << "  dev_signature=0x" << byte_hex(signature) << ";\n"
          << "  cbf_checksum=0x" << std::hex << std::uppercase << crc32(cbf_body) << ";\n}\n";
      damaged_cbf.write(reinterpret_cast<const char*>(damaged.data()),
                        static_cast<std::streamsize>(damaged.size()));
    }
    bool rejected_cbf = false;
    try {
      static_cast<void>(uds::load_chuneng_cbf(cbf_path));
    } catch (const std::runtime_error&) {
      rejected_cbf = true;
    }
    check(rejected_cbf, "CBF parser accepted a block with an invalid CRC16");
  } catch (...) {
    std::filesystem::remove_all(temp);
    throw;
  }
  std::filesystem::remove_all(temp);
}

void test_profile_round_trip() {
  const auto temp = std::filesystem::temp_directory_path() / "uds_cpp_profile_test";
  std::filesystem::remove_all(temp);
  std::filesystem::create_directories(temp);
  try {
    uds::FlashProfile expected;
    expected.id = L"chuneng_331_test";
    expected.flow = L"chuneng_331";
    expected.name = L"楚能 331 测试";
    expected.description = L"项目配置 Unicode 往返测试";
    expected.placeholder = false;
    expected.can_fd = false;
    expected.power_control = false;
    expected.supports_app_tmp_package = true;
    expected.lock_diagnostic_ids = true;
    expected.app_entry_label = L"APP";
    expected.ft_entry_label = L"FT";
    expected.tx_id = 0x772;
    expected.rx_id = 0x77A;
    expected.functional_id = 0x7DF;
    expected.programming_tx_id = 0x716;
    expected.programming_rx_id = 0x617;
    expected.channel = 2;
    expected.nominal_bitrate = 500000;
    expected.data_bitrate = 2000000;
    expected.padding = 0x55;
    expected.isotp_st_min = 0;
    expected.security_level = 0x11;
    expected.security_variant = L"";
    expected.driver0_start = 0x499000;
    expected.driver0_length = 0x10;
    expected.driver_start = 0x49C038;
    expected.driver_length = 0x1EB8;
    expected.app_start = 0xC1000;
    expected.app_length = 0x7B000;
    expected.cal_start = 0xB0000;
    expected.cal_length = 0x200;
    expected.expected_driver_crc16 = 0xCC3C;
    expected.driver_file = L"resources/chuneng_2944/Driver/FlashDriver.srec";
    expected.app_file = L"resources/chuneng_2944/APP/app.s19";
    expected.cal_file = L"resources/chuneng_2944/CAL/cal.s19";
    expected.driver_verify_file = L"resources/chuneng_2944/Verification/driver.asc";
    expected.app_verify_file = L"resources/chuneng_2944/Verification/app.asc";
    expected.app_verify_label = L"Certificate";
    expected.cal_verify_file = L"resources/chuneng_2944/Verification/cal.asc";
    expected.security_dll = L"resources/chuneng_2944/dll/keygen.dll";
    expected.targets.push_back(
        {L"secondary", L"从雷达", 0x760, 0x768, false, 0xB5E2,
         {}, L"resources/changan_c857/APP/ICRR/app.s19", {}, {}, {}, {},
         L"resources/changan_c857/dll/SeedKey_Slave.dll"});

    const auto file = temp / "profile.ini";
    uds::save_profile_ini(expected, file);
    const auto actual = uds::load_profile_ini(file);
    check(actual.id == expected.id && actual.flow == expected.flow &&
              actual.name == expected.name && actual.description == expected.description &&
              actual.placeholder == expected.placeholder &&
               actual.can_fd == expected.can_fd &&
               actual.power_control == expected.power_control &&
               actual.supports_app_tmp_package ==
                   expected.supports_app_tmp_package &&
               actual.lock_diagnostic_ids == expected.lock_diagnostic_ids &&
               actual.app_entry_label == expected.app_entry_label &&
               actual.ft_entry_label == expected.ft_entry_label &&
              actual.tx_id == expected.tx_id &&
              actual.rx_id == expected.rx_id &&
              actual.functional_id == expected.functional_id &&
              actual.programming_tx_id == expected.programming_tx_id &&
              actual.programming_rx_id == expected.programming_rx_id &&
              actual.channel == expected.channel &&
              actual.nominal_bitrate == expected.nominal_bitrate &&
              actual.data_bitrate == expected.data_bitrate && actual.padding == expected.padding &&
              actual.isotp_st_min == expected.isotp_st_min &&
              actual.security_level == expected.security_level &&
              actual.security_variant == expected.security_variant &&
              actual.driver0_start == expected.driver0_start &&
              actual.driver0_length == expected.driver0_length &&
              actual.driver_start == expected.driver_start &&
               actual.driver_length == expected.driver_length &&
               actual.app_start == expected.app_start && actual.app_length == expected.app_length &&
               actual.cal_start == expected.cal_start && actual.cal_length == expected.cal_length &&
               actual.expected_driver_crc16 ==
                   expected.expected_driver_crc16 &&
               actual.driver_file == expected.driver_file && actual.app_file == expected.app_file &&
               actual.cal_file == expected.cal_file &&
               actual.driver_verify_file == expected.driver_verify_file &&
               actual.app_verify_file == expected.app_verify_file &&
               actual.app_verify_label == expected.app_verify_label &&
               actual.cal_verify_file == expected.cal_verify_file &&
               actual.security_dll == expected.security_dll &&
               actual.targets.size() == 1 &&
               actual.targets[0].id == expected.targets[0].id &&
               actual.targets[0].display_name ==
                   expected.targets[0].display_name &&
               actual.targets[0].tx_id == expected.targets[0].tx_id &&
               actual.targets[0].rx_id == expected.targets[0].rx_id &&
               actual.targets[0].expected_app_crc16 ==
                   expected.targets[0].expected_app_crc16 &&
               actual.targets[0].app_file ==
                   expected.targets[0].app_file &&
               actual.targets[0].security_dll ==
                   expected.targets[0].security_dll,
          "flash profile round-trip mismatch");
  } catch (...) {
    std::filesystem::remove_all(temp);
    throw;
  }
  std::filesystem::remove_all(temp);
}

void test_profile_discovery() {
  const auto temp = std::filesystem::temp_directory_path() / "uds_cpp_profile_catalog_test";
  std::filesystem::remove_all(temp);
  std::filesystem::create_directories(temp);
  try {
    uds::FlashProfile zeta;
    zeta.id = L"zeta";
    zeta.flow = L"chuneng_331";
    zeta.name = L"Zeta project";
    uds::save_profile_ini(zeta, temp / "zeta.ini");

    uds::FlashProfile alpha;
    alpha.id = L"alpha";
    alpha.flow = L"future_flow";
    alpha.name = L"Alpha project";
    uds::save_profile_ini(alpha, temp / "alpha.INI");

    uds::FlashProfile pending;
    pending.id = L"pending";
    pending.name = L"Pending project";
    pending.description = L"data pending";
    pending.placeholder = true;
    uds::save_profile_ini(pending, temp / "pending.ini");

    std::ofstream(temp / "ignored.txt") << "not a profile";
    const auto catalog = uds::discover_flash_profiles(temp);
    check(catalog.errors.empty(), "valid profile catalog returned load errors");
    check(catalog.profiles.size() == 3, "profile catalog did not load every INI file");
    check(catalog.profiles[0].profile.id == L"alpha" &&
              catalog.profiles[1].profile.id == L"pending" &&
              catalog.profiles[1].profile.placeholder &&
              catalog.profiles[1].profile.flow.empty() &&
              catalog.profiles[2].profile.id == L"zeta",
          "profile catalog sorting mismatch");
  } catch (...) {
    std::filesystem::remove_all(temp);
    throw;
  }
  std::filesystem::remove_all(temp);
}

void test_workflow_registry() {
  check(!uds::is_flash_workflow_registered(L"chuneng_331"),
        "legacy ChuNeng 331 workflow must not remain selectable");
  check(uds::is_flash_workflow_registered(L"chery_ars1_33"),
        "Chery ARS1.33 workflow is not registered");
  check(uds::is_flash_workflow_registered(L"chery_kp31"),
        "Chery KP31 workflow is not registered");
  check(uds::is_flash_workflow_registered(L"chery_e0y"),
        "Chery E0Y workflow is not registered");
  check(uds::is_flash_workflow_registered(L"chery_t22"),
        "Chery T22 workflow is not registered");
  check(uds::is_flash_workflow_registered(L"chery_t1ej"),
        "Chery T1EJ workflow is not registered");
  check(uds::is_flash_workflow_registered(L"changan_c857"),
        "Changan C857 workflow is not registered");
  check(uds::is_flash_workflow_registered(L"lingyao_b216"),
        "Lingyao B216 workflow is not registered");
  check(uds::is_flash_workflow_registered(L"longma_ars1_31"),
        "Longma ARS1.31 workflow is not registered");
  check(uds::is_flash_workflow_registered(L"xizhong_rsmr"),
        "Xizhong RSMR workflow is not registered");
  check(uds::is_flash_workflow_registered(L"xizhong_lsmr"),
        "Xizhong LSMR workflow is not registered");
  const auto xizhong_rsmr = uds::create_flash_workflow(L"xizhong_rsmr");
  const auto xizhong_lsmr = uds::create_flash_workflow(L"xizhong_lsmr");
  check(xizhong_rsmr && xizhong_lsmr &&
            xizhong_rsmr->id() == L"xizhong_rsmr" &&
            xizhong_lsmr->id() == L"xizhong_lsmr" &&
            xizhong_rsmr->report_title({}) ==
                "Xizhong RSMR Download Report" &&
            xizhong_lsmr->report_title({}) ==
                "Xizhong LSMR Download Report",
        "Xizhong RSMR/LSMR factories did not create target-specific workflows");
  check(uds::is_flash_workflow_registered(
            L"shidaixinan_hjzj_fmr"),
        "Shidaixinan HJZJ_FMR workflow is not registered");
  check(uds::is_flash_workflow_registered(L"lp_arc"),
        "LP-ARC workflow is not registered");
  check(uds::is_flash_workflow_registered(L"chuneng_arc331"),
        "ChuNeng ARC331 radar workflow is not registered");
  check(uds::is_flash_workflow_registered(L"lp_arf"),
        "LP-ARF workflow is not registered");
  check(!uds::is_flash_workflow_registered(L"lp_arf231_a12") &&
            !uds::is_flash_workflow_registered(L"lp_arf231_b11"),
        "retired A12/B11 duplicate ARF workflows are still registered");
  check(uds::is_flash_workflow_registered(L"geely_p416"),
        "Geely P416 workflow is not registered");
  check(!uds::is_flash_workflow_registered(L"geely_p146"),
        "Removed Geely P146 workflow is still registered");
  check(uds::is_flash_workflow_registered(L"baic_n61ab"),
        "BAIC N61AB workflow is not registered");
  check(uds::is_flash_workflow_registered(L"baic_bqb41"),
        "BAIC BQB41 workflow is not registered");
  check(!uds::is_flash_workflow_registered(L"future_flow"),
        "unknown workflow was reported as registered");
  const auto registered = uds::registered_flash_workflows();
  check(registered.size() == 17 &&
            std::find(registered.begin(), registered.end(), L"chuneng_331") == registered.end() &&
            std::find(registered.begin(), registered.end(), L"chuneng_arc331") != registered.end() &&
             std::find(registered.begin(), registered.end(), L"chery_ars1_33") != registered.end() &&
             std::find(registered.begin(), registered.end(), L"chery_kp31") != registered.end() &&
            std::find(registered.begin(), registered.end(), L"chery_e0y") != registered.end() &&
            std::find(registered.begin(), registered.end(), L"chery_t22") != registered.end() &&
            std::find(registered.begin(), registered.end(), L"chery_t1ej") != registered.end() &&
            std::find(registered.begin(), registered.end(), L"changan_c857") != registered.end() &&
            std::find(registered.begin(), registered.end(), L"lingyao_b216") != registered.end() &&
            std::find(registered.begin(), registered.end(), L"longma_ars1_31") != registered.end() &&
             std::find(registered.begin(), registered.end(), L"xizhong_rsmr") != registered.end() &&
             std::find(registered.begin(), registered.end(), L"xizhong_lsmr") != registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"shidaixinan_hjzj_fmr") != registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"lp_arc") != registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"lp_arf") != registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"geely_p416") != registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"geely_p146") == registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"baic_n61ab") != registered.end() &&
            std::find(registered.begin(), registered.end(),
                      L"baic_bqb41") != registered.end(),
        "workflow registry enumeration mismatch");
  bool rejected = false;
  try {
    static_cast<void>(uds::create_flash_workflow(L"future_flow"));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected, "workflow factory accepted an unknown flow");
}

void test_chuneng_cbf_software_type_compatibility() {
  check(uds::kChuneng331CbfDriverAddress == 0x10280000U &&
            uds::is_supported_chuneng_driver_cbf_type("EXE") &&
            uds::is_supported_chuneng_driver_cbf_type("SBL") &&
            !uds::is_supported_chuneng_driver_cbf_type("APP") &&
            uds::is_supported_chuneng_app_cbf_type("DATA") &&
            uds::is_supported_chuneng_app_cbf_type("APP") &&
            !uds::is_supported_chuneng_app_cbf_type("SBL"),
        "ChuNeng CBF software type compatibility mismatch");

  check(uds::resolve_chuneng_331_input_mode(L"driver.cbf", L"app.CBF") ==
                uds::Chuneng331InputMode::cbf_pair &&
            uds::resolve_chuneng_331_input_mode(L"driver.CBF", L"app.s19") ==
                uds::Chuneng331InputMode::driver_cbf_app_srecord &&
            uds::resolve_chuneng_331_input_mode(L"driver.srec", L"app.S19") ==
                uds::Chuneng331InputMode::srecord_pair,
        "ChuNeng CBF/S-record input mode detection mismatch");
  bool rejected_inverse_mixed = false;
  try {
    static_cast<void>(uds::resolve_chuneng_331_input_mode(
        L"driver.s19", L"app.cbf"));
  } catch (const std::invalid_argument&) {
    rejected_inverse_mixed = true;
  }
  check(rejected_inverse_mixed,
        "ChuNeng accepted unsupported Driver S-record + APP CBF input");
  bool rejected_unknown = false;
  try {
    static_cast<void>(uds::resolve_chuneng_331_input_mode(
        L"driver.bin", L"app.bin"));
  } catch (const std::invalid_argument&) {
    rejected_unknown = true;
  }
  check(rejected_unknown,
        "ChuNeng accepted an unsupported non-CBF/non-S-record input set");

  check(uds::chuneng_331_abt_sidecar_path(
            L"package/Driver_Ver.asc") ==
            std::filesystem::path(L"package/Driver_ABT.asc"),
        "ChuNeng S-record ABT sidecar naming contract changed");
  const std::vector<std::uint8_t> image{0x11, 0x22, 0x33, 0x44};
  std::vector<std::uint8_t> abt{0x00, 0x00, 0x00, 0x01,
                                0x10, 0x28, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x04};
  const auto digest = uds::sha256(image);
  abt.insert(abt.end(), digest.begin(), digest.end());
  const auto metadata = uds::validate_chuneng_331_abt(abt, image);
  check(metadata.source_address == 0x10280000U &&
            metadata.image_length == image.size(),
        "ChuNeng S-record ABT metadata/SHA binding was not validated");
  auto mismatched = image;
  mismatched[0] ^= 0xFFU;
  bool rejected_mismatched_abt = false;
  try {
    static_cast<void>(uds::validate_chuneng_331_abt(abt, mismatched));
  } catch (const std::runtime_error&) {
    rejected_mismatched_abt = true;
  }
  check(rejected_mismatched_abt,
        "ChuNeng accepted an ABT from a different S-record package");
}

void test_chuneng_331_updated_protocol_contract() {
  std::tm local{};
  local.tm_year = 126;
  local.tm_mon = 6;
  local.tm_mday = 29;
  const auto fingerprint = uds::chuneng_331_fingerprint_f184(local);
  check(fingerprint == std::vector<std::uint8_t>(
                           {0x26, 0x07, 0x29, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
        "ChuNeng F184 fingerprint must be exactly nine bytes");

  check(uds::chuneng_331_transfer_block_length(
            std::array<std::uint8_t, 4>{0x74, 0x20, 0x08, 0x02}) == 0x802 &&
            uds::kChuneng331BlockLength == 0x802 &&
            uds::kChuneng331TransferDataLength == 0x800,
        "ChuNeng block_length must be 0x802 with 0x800 bytes of TransferData payload");

  bool rejected = false;
  try {
    static_cast<void>(uds::chuneng_331_transfer_block_length(
        std::array<std::uint8_t, 4>{0x74, 0x20, 0x04, 0x02}));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected, "ChuNeng accepted an ECU block length smaller than 0x802");

  check(uds::kChuneng331TesterPresentPeriod ==
            std::chrono::milliseconds(2000) &&
            uds::kChuneng331SessionControlDelay ==
                std::chrono::milliseconds(50) &&
            uds::kChuneng331FunctionalControlDelay ==
                std::chrono::milliseconds(100) &&
            uds::kChuneng331FtProgrammingTransitionDelay ==
                std::chrono::milliseconds(2000),
        "ChuNeng standard timing contract mismatch");
  const auto left_ft = uds::chuneng_331_ft_transition_endpoints(
      0x701, 0x761, 0x72F);
  const auto right_ft = uds::chuneng_331_ft_transition_endpoints(
      0x701, 0x761, 0x72D);
  check(left_ft.request_id == 0x701 &&
            left_ft.pending_response_id == 0x761 &&
            left_ft.final_response_id == 0x72F &&
            right_ft.request_id == 0x701 &&
            right_ft.pending_response_id == 0x761 &&
            right_ft.final_response_id == 0x72D,
        "ChuNeng FT transition endpoint mapping mismatch");
  check(uds::kChuneng331FunctionalDefaultSessionRequest ==
            std::array<std::uint8_t, 2>{0x10, 0x01} &&
            uds::kChuneng331ExtendedSessionRequest ==
                std::array<std::uint8_t, 2>{0x10, 0x03} &&
            uds::kChuneng331FtFunctionalSessionPreamble ==
                std::array<std::array<std::uint8_t, 2>, 2>{
                    std::array<std::uint8_t, 2>{0x10, 0x01},
                    std::array<std::uint8_t, 2>{0x10, 0x03}} &&
            uds::kChuneng331ProgrammingPrecondition ==
                std::array<std::uint8_t, 4>{0x31, 0x01, 0x02, 0x03} &&
            uds::kChuneng331FunctionalExtendedSession ==
                std::array<std::uint8_t, 2>{0x10, 0x83} &&
            uds::kChuneng331DisableDtc ==
                std::array<std::uint8_t, 2>{0x85, 0x82} &&
            uds::kChuneng331DisableCommunication ==
                std::array<std::uint8_t, 3>{0x28, 0x83, 0x03} &&
            uds::kChuneng331ProgrammingSession ==
                std::array<std::uint8_t, 2>{0x10, 0x02},
        "ChuNeng standard preprogramming sequence mismatch");
  check(uds::chuneng_331_precondition_nrc_allows_continue(0x31) &&
            !uds::chuneng_331_precondition_nrc_allows_continue(0x22) &&
            !uds::chuneng_331_precondition_nrc_allows_continue(0x78),
        "ChuNeng ARC331 precondition NRC continuation policy mismatch");
  check(uds::kChuneng331EnableDtc ==
            std::array<std::uint8_t, 2>{0x85, 0x81} &&
            uds::kChuneng331EnableCommunication ==
                std::array<std::uint8_t, 3>{0x28, 0x80, 0x03} &&
            uds::kChuneng331FunctionalDefaultSession ==
                std::array<std::uint8_t, 2>{0x10, 0x81} &&
            uds::kChuneng331ClearDtc ==
                std::array<std::uint8_t, 4>{0x14, 0xFF, 0xFF, 0xFF},
        "ChuNeng standard post-reset sequence mismatch");
  check(uds::chuneng_331_routine_success_prefix(0x0203) ==
            std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x03, 0x04} &&
            uds::chuneng_331_routine_success_prefix(0x0202) ==
                std::array<std::uint8_t, 5>{0x71, 0x01, 0x02, 0x02, 0x04} &&
            uds::chuneng_331_routine_success_prefix(0xFF01) ==
                std::array<std::uint8_t, 5>{0x71, 0x01, 0xFF, 0x01, 0x04},
        "ChuNeng routine success status must be 0x04");
}

void test_xizhong_rsmr_profile_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile = uds::load_profile_ini(source / "profiles" / "xizhong_rsmr.ini");
  check(profile.id == L"xizhong_rsmr" && profile.flow == L"xizhong_rsmr" &&
             profile.supports_ft_entry && profile.default_entry_mode == L"app" &&
             profile.lock_diagnostic_ids &&
            profile.extended_id && profile.uds_fd && profile.uds_brs &&
            profile.tx_id == 0x18DAB7F1 && profile.rx_id == 0x18DAF1B7 &&
            profile.functional_id == 0x18DBFFF1 && profile.ft_tx_id == 0x701 &&
            profile.ft_rx_id == 0x761 && profile.app_verify_label == L"Hash" &&
            profile.padding == uds::kXizhongPhysicalPadding &&
            profile.isotp_st_min == 0,
        "Xizhong RSMR profile mismatch");
  const auto versions = uds::load_version_check_plan(
      source / "profiles" / "xizhong_rsmr.ini", L"");
  check(versions.session == 0x01 &&
            versions.precondition == L"xizhong_nm" &&
            versions.items.size() == 3 &&
            versions.items[0].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x87}) &&
            versions.items[1].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x80}) &&
            versions.items[2].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x89}) &&
            versions.items[0].expected.empty() &&
            versions.items[1].expected.empty() &&
            versions.items[2].expected.empty(),
        "Xizhong RSMR read-only version plan mismatch");
  const auto check_version_requests = [&](const char* id,
                                          std::initializer_list<unsigned> dids) {
    const auto plan = uds::load_version_check_plan(
        source / "profiles" / (std::string(id) + ".ini"), L"");
    check(plan.items.size() == dids.size(),
          std::string("Approved version-read plan item count mismatch: ") + id);
    std::size_t index{};
    for (const auto did : dids) {
      check(plan.items[index].request ==
                std::vector<std::uint8_t>(
                    {0x22, static_cast<std::uint8_t>(did >> 8U),
                     static_cast<std::uint8_t>(did)}),
            "Approved version-read DID mismatch");
      ++index;
    }
  };
  check_version_requests("chery_ars1_33",
                         {0xF187, 0xF180, 0xF189, 0xF032, 0xF195, 0xF013,
                          0xF031});
  check_version_requests("chery_e0y",
                         {0xF187, 0xF180, 0xF189, 0xF032, 0xF195, 0xF013,
                          0xF031});
  check_version_requests("chery_kp31",
                         {0xF187, 0xF180, 0xF189, 0xF032, 0xF195, 0xF013,
                          0xF031});
  check_version_requests("chery_t1ej",
                         {0xF187, 0xF180, 0xF188, 0xF032, 0xF195, 0xF013,
                          0xF031});
  check_version_requests("chery_t22",
                         {0xF187, 0xF180, 0xF188, 0xF032, 0xF195, 0xF013,
                          0xF031});
  check_version_requests("chuneng_331_left_rear",
                         {0xF187, 0xF180, 0xF195, 0xF189, 0xF193});
  check_version_requests("longma_ars1_31",
                         {0xF187, 0xF170, 0xF189, 0xF180, 0xF188, 0xF193,
                          0xF195});
  check_version_requests("lp_arc",
                         {0xF187, 0xF180, 0xF189, 0xF195, 0xF182, 0xF188,
                          0xF193, 0xF150, 0xF191, 0xFF00});
  check_version_requests("lp_arf",
                         {0xF187, 0xF180, 0xF189, 0xF195, 0xF182, 0xF188,
                          0xF193, 0xF150});
  check_version_requests("shidaixinan_hjzj_fmr",
                         {0xF193, 0xF195, 0xF189, 0xF191, 0xF180, 0xF183});
  check_version_requests(
      "geely_p416",
      {0xF180, 0xF120, 0xF121, 0xF125, 0xF12A, 0xF12B, 0xF12E,
       0xF1A0, 0xF1A1, 0xF1A5, 0xF1AA, 0xF1AB, 0xF1AE});
  check_version_requests(
      "geely_p611",
      {0xF180, 0xF120, 0xF121, 0xF125, 0xF12A, 0xF12B, 0xF12E,
       0xF1A0, 0xF1A1, 0xF1A5, 0xF1AA, 0xF1AB, 0xF1AE});
  for (const auto* id : {"shidaixinan_muxing2_fmr",
                          "shidaixinan_qingling_fmr",
                          "shidaixinan_tianwangxing_fmr"}) {
    check_version_requests(id, {0xF187, 0xF193, 0xF195, 0xF191, 0xF189,
                                0xF180, 0xF183, 0xF175, 0xF170});
  }
  const auto root = source / "resources" / "xizhong_rsmr";
  const auto driver = uds::load_srecord_window(
      root / "ARC2.33C1_HQ001A_FlashDrv.s19", 0x80000, 0x400);
  check(driver.size() == 0x400,
        "Xizhong Driver resource mismatch");
  const auto app = uds::load_srecord_window(
      root / "RSMR_AA_APP1_V09.13.00.s19", 0xC0000, 0x300000);
  check(app.size() == 0x300000,
        "Xizhong APP resource mismatch");
  const auto app_hash = uds::load_srecord_window(
      root / "RSMR_AA_APP1_V09.13.00_Hash.s19", 0x0, 0x20);
  check(app_hash.size() == 0x20,
        "Xizhong Hash resource mismatch");
  check(uds::sha256(driver) == uds::kXizhongDriverHash,
        "Xizhong Driver data SHA-256 mismatch");
  const auto calculated_app_hash = uds::sha256(app);
  check(std::equal(calculated_app_hash.begin(), calculated_app_hash.end(),
                   app_hash.begin(), app_hash.end()),
        "Xizhong APP data SHA-256 does not match Hash S19");
  check(std::filesystem::file_size(
            root / "CDD" / "EP32_V1.7.110.100.cdd") == 1186637,
        "Xizhong passing CDD provenance resource mismatch");

  auto configurable_profile = profile;
  configurable_profile.tx_id = 0x18DA01F1;
  configurable_profile.rx_id = 0x18DAF101;
  bool configurable_endpoint_accepted = true;
  try {
    uds::validate_xizhong_configurable_endpoint(
        configurable_profile, uds::XizhongRadarTarget::rsmr);
  } catch (const std::runtime_error&) {
    configurable_endpoint_accepted = false;
  }
  check(configurable_endpoint_accepted,
        "Xizhong RSMR rejected a valid configurable 29-bit APP endpoint");
}

void test_xizhong_lsmr_profile_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile = uds::load_profile_ini(source / "profiles" / "xizhong_lsmr.ini");
  check(profile.id == L"xizhong_lsmr" && profile.flow == L"xizhong_lsmr" &&
            !profile.placeholder && profile.vendor_name == L"犀重" &&
            profile.project_name == L"HQ001" && profile.device_name == L"LSMR" &&
            profile.tx_id == 0x18DAB6F1 && profile.rx_id == 0x18DAF1B6 &&
            profile.functional_id == 0x18DBFFF1 && profile.ft_tx_id == 0x714 &&
            profile.ft_rx_id == 0x71C && !profile.supports_ft_entry &&
            profile.security_level == 0x11 &&
            profile.security_dll.filename() == L"XZ_GenerateKeyEx_LSMR.dll" &&
            profile.driver_file.empty() && profile.app_file.empty() &&
            profile.app_verify_file.empty() && profile.targets.size() == 1 &&
            profile.targets[0].id == L"lsmr" &&
            profile.targets[0].tx_id == 0x18DAB6F1 &&
            profile.targets[0].rx_id == 0x18DAF1B6 &&
            profile.targets[0].ft_tx_id == 0x714 &&
            profile.targets[0].ft_rx_id == 0x71C &&
            profile.targets[0].pending_validation,
        "Xizhong LSMR profile mismatch");
  const auto root = source / "resources" / "xizhong_lsmr";
  check(std::filesystem::is_regular_file(root / "XZ_GenerateKeyEx_LSMR.dll") &&
            uds::load_srecord_window(root / "ARC2.33C1_HQ001A_FlashDrv.s19",
                                     0x80000, 0x400).size() == 0x400,
        "Xizhong LSMR resources missing");
  const auto lsmr_nm = uds::xizhong_nm_wakeup_frame(0x18FFA0B6);
  const auto tester_present = uds::xizhong_tester_present_frames(0x18DBFFF1);
  check(lsmr_nm.id == 0x18FFA0B6 && lsmr_nm.extended && !lsmr_nm.fd &&
            !lsmr_nm.brs && tester_present[0].frame.id == 0x18DBFFF1 &&
            tester_present[0].frame.extended && !tester_present[0].frame.fd &&
            tester_present[1].frame.fd && tester_present[1].frame.brs,
        "Xizhong LSMR CAN frame contract mismatch");

  auto configurable_profile = profile;
  configurable_profile.tx_id = 0x18DA02F1;
  configurable_profile.rx_id = 0x18DAF102;
  bool configurable_endpoint_accepted = true;
  try {
    uds::validate_xizhong_configurable_endpoint(
        configurable_profile, uds::XizhongRadarTarget::lsmr);
  } catch (const std::runtime_error&) {
    configurable_endpoint_accepted = false;
  }
  check(configurable_endpoint_accepted,
        "Xizhong LSMR rejected a valid configurable 29-bit APP endpoint");
  configurable_profile.ft_rx_id = 0;
  bool invalid_ft_rejected = false;
  try {
    uds::validate_xizhong_configurable_endpoint(
        configurable_profile, uds::XizhongRadarTarget::lsmr);
  } catch (const std::runtime_error&) {
    invalid_ft_rejected = true;
  }
  check(invalid_ft_rejected,
        "Xizhong LSMR accepted an invalid target-specific FT endpoint");

  const auto workflow = uds::create_flash_workflow(L"xizhong_lsmr");
  uds::FlashJob job;
  job.profile = profile;
  job.entry_mode = L"ft";
  bool ft_rejected = false;
  try {
    workflow->run(job, {}, {});
  } catch (const std::runtime_error& error) {
    ft_rejected = std::string(error.what()).find("RaderID=1") !=
                  std::string::npos;
  }
  check(ft_rejected,
        "Xizhong LSMR unexpectedly accepted the empty CANoe FT branch");

  job.entry_mode = L"auto";
  bool invalid_mode_rejected = false;
  try {
    workflow->run(job, {}, {});
  } catch (const std::runtime_error& error) {
    invalid_mode_rejected = std::string(error.what()).find("只允许APP") !=
                            std::string::npos;
  }
  check(invalid_mode_rejected,
        "Xizhong LSMR unexpectedly accepted an undeclared entry mode");

  job.entry_mode = L"app";
  bool missing_images_rejected = false;
  try {
    workflow->run(job, {}, {});
  } catch (const std::runtime_error& error) {
    missing_images_rejected = std::string(error.what()).find("同一LSMR发布包") !=
                              std::string::npos;
  }
  check(missing_images_rejected,
        "Xizhong LSMR did not fail closed when Driver/APP/Hash were absent");
}

void test_xizhong_rsmr_protocol_baseline() {
  check(uds::kXizhongDisableDtc ==
            std::array<std::uint8_t, 2>{0x85, 0x02},
        "Xizhong disable-DTC request mismatch");
  check(uds::kXizhongDisableCommunication ==
            std::array<std::uint8_t, 3>{0x28, 0x03, 0x01},
        "Xizhong disable-communication request mismatch");
  check(uds::kXizhongEnableCommunication ==
            std::array<std::uint8_t, 3>{0x28, 0x00, 0x01} &&
            uds::kXizhongEnableDtc ==
                std::array<std::uint8_t, 2>{0x85, 0x01} &&
            uds::kXizhongDefaultSession ==
                std::array<std::uint8_t, 2>{0x10, 0x01},
        "Xizhong post-reset recovery request mismatch");
  check(uds::kXizhongPhysicalPadding == 0xCC &&
            uds::kXizhongFunctionalPadding == 0x00 &&
             uds::kXizhongP2 == std::chrono::milliseconds(100) &&
             uds::kXizhongTransferDataP2 ==
                 std::chrono::milliseconds(2000) &&
             uds::kXizhongP2Star == std::chrono::milliseconds(5000) &&
             uds::kXizhongFlowControlDelay == std::chrono::milliseconds(10) &&
             uds::kXizhongTesterPresentPeriod == std::chrono::milliseconds(4000) &&
             uds::kXizhongNmMaxConsecutiveFailures == 5 &&
             uds::kXizhongNmWakeupSettle == std::chrono::milliseconds(1000) &&
             uds::kXizhongSessionSettle == std::chrono::milliseconds(200) &&
             uds::kXizhongRequestDownloadSettle == std::chrono::milliseconds(50) &&
             uds::kXizhongRoutineSettle == std::chrono::milliseconds(50) &&
             uds::kXizhongAppHashSettle == std::chrono::milliseconds(1000) &&
             uds::kXizhongResetSettle == std::chrono::milliseconds(1000) &&
             uds::kXizhongFtEndpointSettle == std::chrono::milliseconds(2000),
         "Xizhong transport/timing baseline mismatch");

  const auto tester_present = uds::xizhong_rsmr_tester_present_frames();
  check(tester_present.size() == 2,
        "Xizhong passed-BLF TesterPresent stream count mismatch");
  const auto is_expected_tp = [](const auto& item) {
    return item.frame.id == 0x18DBFFF1 && item.frame.extended &&
           item.frame.data == std::vector<std::uint8_t>(
               {0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}) &&
           item.period == std::chrono::milliseconds(4000);
  };
  check(is_expected_tp(tester_present[0]) &&
            is_expected_tp(tester_present[1]) &&
            !tester_present[0].frame.fd && !tester_present[0].frame.brs &&
            tester_present[1].frame.fd && tester_present[1].frame.brs,
        "Xizhong Classic + FD/BRS TesterPresent pair mismatch");
  const auto nm_wakeup = uds::xizhong_rsmr_nm_wakeup_frame();
  check(nm_wakeup.id == 0x18FFA025 && nm_wakeup.extended && !nm_wakeup.fd &&
            !nm_wakeup.brs && nm_wakeup.data.size() == 8 &&
            std::all_of(nm_wakeup.data.begin(), nm_wakeup.data.end(),
                        [](std::uint8_t value) { return value == 0; }) &&
            uds::kXizhongNmPeriod == std::chrono::milliseconds(200),
        "Xizhong CANoe NM_ICG wakeup baseline mismatch");
  check(!uds::xizhong_rsmr_report_line(
             "36 APP progress: 128/1536 blocks") &&
             !uds::xizhong_rsmr_report_line("34 APP") &&
             uds::xizhong_rsmr_report_line("34 APP PASS: 74 20 08 02") &&
             uds::xizhong_rsmr_report_line(
                 "22 F189 WARN: ECU returned 7F 22 31") &&
             uds::xizhong_rsmr_report_line(
                 "TransferData (0x36) APP PASS: blocks=1536"),
         "Xizhong report did not collapse per-block TransferData progress");
  uds::UdsResponse optional_f189;
  optional_f189.response = {0x7F, 0x22, 0x31};
  optional_f189.nrc = 0x31;
  check(uds::xizhong_rsmr_optional_f189_nrc(optional_f189),
        "Xizhong F189 NRC31 was not classified as the CAPL-compatible warning");
  optional_f189.nrc = 0x22;
  check(!uds::xizhong_rsmr_optional_f189_nrc(optional_f189),
        "Xizhong F189 incorrectly accepted an unrelated NRC");

  MockBus physical_bus;
  uds::IsoTpSession physical_tp(
      physical_bus,
      {0x18DAB7F1, 0x18DAF1B7, uds::kXizhongPhysicalPadding, 0, 0,
       std::chrono::milliseconds(1000), std::chrono::milliseconds(1000),
       true, true, true, true});
  physical_tp.send(std::array<std::uint8_t, 2>{0x10, 0x03});
  check(physical_bus.sent.size() == 1 && physical_bus.sent[0].fd &&
            physical_bus.sent[0].brs &&
            physical_bus.sent[0].data ==
                std::vector<std::uint8_t>({0x02, 0x10, 0x03, 0xCC,
                                           0xCC, 0xCC, 0xCC, 0xCC}),
        "Xizhong physical SF format/padding mismatch");

  MockBus functional_bus;
  uds::IsoTpSession functional_tp(
      functional_bus,
      {0x18DBFFF1, 0x18DAF1B7, uds::kXizhongFunctionalPadding, 0, 0,
       std::chrono::milliseconds(1000), std::chrono::milliseconds(1000),
       true, true, true, true});
  functional_tp.send(uds::kXizhongDisableDtc);
  check(functional_bus.sent.size() == 1 && functional_bus.sent[0].fd &&
            functional_bus.sent[0].brs &&
            functional_bus.sent[0].data ==
                std::vector<std::uint8_t>({0x02, 0x85, 0x02, 0x00,
                                           0x00, 0x00, 0x00, 0x00}),
        "Xizhong functional SF format/padding mismatch");

  std::tm local{};
  local.tm_year = 126;
  local.tm_mon = 6;
  local.tm_mday = 20;
  const auto f184 = uds::xizhong_rsmr_f184_data(local);
  check(f184.size() == 20 &&
            std::all_of(f184.begin(), f184.begin() + 16,
                        [](std::uint8_t value) { return value == 0; }) &&
            std::vector<std::uint8_t>(f184.begin() + 16, f184.end()) ==
                std::vector<std::uint8_t>({0x20, 0x26, 0x07, 0x20}),
        "Xizhong F184 Sign/BCD date layout mismatch");
}

void test_chery_ars133_protocol_and_resources() {
  const auto precondition_frames = uds::chery_ars133_precondition_frames();
  check(precondition_frames.size() == 3,
        "Chery ARS1.33 precondition frame count mismatch");
  check(precondition_frames[0].frame.id == 0x600 &&
            precondition_frames[0].frame.data ==
                std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00}) &&
            !precondition_frames[0].frame.extended &&
            !precondition_frames[0].frame.fd &&
            !precondition_frames[0].frame.brs &&
            precondition_frames[0].period == std::chrono::milliseconds(100),
        "Chery ARS1.33 0x600 wake-up precondition mismatch");
  check(precondition_frames[1].frame.id == 0x25B &&
            precondition_frames[1].frame.data ==
                std::vector<std::uint8_t>({0x00, 0x00, 0x02, 0x00,
                                           0x00, 0x00, 0x00, 0x00}) &&
            !precondition_frames[1].frame.extended &&
            !precondition_frames[1].frame.fd &&
            !precondition_frames[1].frame.brs &&
            precondition_frames[1].period == std::chrono::milliseconds(20),
        "Chery ARS1.33 0x25B PowerMode=ON precondition mismatch");
  check(precondition_frames[2].frame.id == 0x4B4 &&
            precondition_frames[2].frame.data ==
                std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x10}) &&
            !precondition_frames[2].frame.extended &&
            !precondition_frames[2].frame.fd &&
            !precondition_frames[2].frame.brs &&
            precondition_frames[2].period == std::chrono::milliseconds(100),
        "Chery ARS1.33 0x4B4 PRND=P precondition mismatch");

  const auto tester_present =
      uds::chery_ars133_app_tester_present_frame(0x7DF);
  check(tester_present.id == 0x7DF &&
            tester_present.data ==
                std::vector<std::uint8_t>(
                    {0x02, 0x3E, 0x80, 0x55, 0x55, 0x55, 0x55, 0x55}) &&
            !tester_present.extended && !tester_present.fd &&
            !tester_present.brs &&
            uds::kCheryArs133SuppressedSessionSettle ==
                std::chrono::milliseconds(100) &&
            uds::kCheryArs133WakeupSettle ==
                std::chrono::milliseconds(1000),
        "Chery ARS1.33 APP-only TesterPresent frame mismatch");

  const auto app_cal_plan =
      uds::resolve_chery_ars133_download_plan(L"app_cal");
  const auto app_plan =
      uds::resolve_chery_ars133_download_plan(L"app");
  const auto cal_plan =
      uds::resolve_chery_ars133_download_plan(L"cal");
  check(app_cal_plan.mode == uds::CheryArs133FlashMode::AppCal &&
            app_cal_plan.download_app && app_cal_plan.download_cal &&
            !app_cal_plan.periodic_tester_present &&
            app_plan.mode == uds::CheryArs133FlashMode::AppOnly &&
            app_plan.download_app && !app_plan.download_cal &&
            app_plan.periodic_tester_present &&
            cal_plan.mode == uds::CheryArs133FlashMode::CalOnly &&
            !cal_plan.download_app && cal_plan.download_cal &&
            !cal_plan.periodic_tester_present,
        "Chery ARS1.33 three-mode download plan mismatch");
  bool invalid_mode_rejected = false;
  try {
    static_cast<void>(
        uds::resolve_chery_ars133_download_plan(L"unsupported"));
  } catch (const std::invalid_argument&) {
    invalid_mode_rejected = true;
  }
  check(invalid_mode_rejected,
        "Chery ARS1.33 unsupported mode was not rejected");

  const auto request_download = uds::chery_ars133_request_download(0x00499000, 0x10);
  check(request_download == std::vector<std::uint8_t>(
          {0x34, 0x00, 0x44, 0x00, 0x49, 0x90, 0x00, 0x00, 0x00, 0x00, 0x10}),
        "Chery ARS1.33 RequestDownload encoding mismatch");
  const auto erase = uds::chery_ars133_erase_memory(0x000C1000, 0x7B000);
  check(erase == std::vector<std::uint8_t>(
          {0x31, 0x01, 0xFF, 0x00, 0x44, 0x00, 0x0C, 0x10, 0x00,
           0x00, 0x07, 0xB0, 0x00}),
        "Chery ARS1.33 erase request encoding mismatch");
  check(uds::chery_ars133_max_block_length(
            std::array<std::uint8_t, 6>{0x74, 0x40, 0x00, 0x00, 0x04, 0x00}) == 0x400,
        "Chery ARS1.33 max block length decoding mismatch");
  check(uds::chery_ars133_max_block_length(
            std::array<std::uint8_t, 6>{0x74, 0x40, 0x00, 0x00, 0x00, 0x00}) == 0x400,
        "Chery ARS1.33 zero block length fallback mismatch");

  const auto root = std::filesystem::path(UDS_SOURCE_DIR) / "resources" / "chery_ars1_33";
  const auto profile = uds::load_profile_ini(
      std::filesystem::path(UDS_SOURCE_DIR) / "profiles" / "chery_ars1_33.ini");
  check(!profile.placeholder && profile.flow == L"chery_ars1_33" &&
            profile.vendor_name == L"奇瑞" &&
            profile.project_name == L"ARS1.33" &&
            profile.device_name == L"从雷达" && !profile.can_fd &&
            !profile.power_control && profile.tx_id == 0x6C4 && profile.rx_id == 0x6C5 &&
            profile.functional_id == 0x7DF && profile.nominal_bitrate == 500000 &&
             profile.padding == 0x55 && profile.isotp_st_min == 0 &&
             profile.security_level == 0x11 && profile.security_variant.empty() &&
              profile.supports_cal_download &&
              profile.default_entry_mode == L"app_cal" &&
              profile.targets.size() == 2 &&
              profile.targets[0].id == L"secondary" &&
              profile.targets[0].display_name == L"从雷达" &&
              profile.targets[0].tx_id == 0x6C4 &&
              profile.targets[0].rx_id == 0x6C5 &&
              !profile.targets[0].pending_validation &&
              profile.targets[1].id == L"main" &&
              profile.targets[1].display_name == L"主雷达" &&
              profile.targets[1].tx_id == 0x71F &&
              profile.targets[1].rx_id == 0x79F &&
              profile.targets[1].pending_validation &&
              profile.driver0_start == 0x499000 && profile.driver0_length == 0x10 &&
             profile.driver_start == 0x49C038 && profile.driver_length == 0x1EB8 &&
             profile.app_start == 0xC1000 && profile.app_length == 0x7B000 &&
             profile.cal_start == 0xB0000 && profile.cal_length == 0x200 &&
             profile.cal_file == std::filesystem::path(
                 L"resources\\chery_ars1_33\\CAL\\ARS133_Cail_20260605_"
                 L"S0000054590_02.00.01.S19") &&
             profile.cal_verify_file == std::filesystem::path(
                 L"resources\\chery_ars1_33\\Verification\\6a4e11fa3ff68_"
                 L"CIR_S0000054590_020001_CAL1_MCU_UDS_20260622.rsa"),
         "Chery ARS1.33 packaged profile mismatch");
  const auto fld = root / "FLD" / "ARS1.33_702000275AA_S0000054588_FLD_020001.s19";
  const auto app = root / "APP" /
      "ARS1.33C3A_AF2T3R_B1.0.00_APP_V02.00.02C_CHF0376N_without_boot.s19";
  const auto cal = root / "CAL" / "ARS133_Cail_20260605_S0000054590_02.00.01.S19";
  const auto driver_rsa = root / "Verification" /
      "6a4e11d2ebc8c_CIR_S0000054588_020002_FLD1_MCU_UDS_20260622.rsa";
  const auto app_rsa = root / "Verification" /
      "6a4e11f5384ca_CIR_S0000054589_020002_ASW1_MCU_UDS_20260706.rsa";
  const auto cal_rsa = root / "Verification" /
      "6a4e11fa3ff68_CIR_S0000054590_020001_CAL1_MCU_UDS_20260622.rsa";
  check(uds::load_srecord_window_filtered(fld, 0x00499000, 0x10).size() == 0x10,
        "packaged Chery FLD block0 is invalid");
  check(uds::load_srecord_window_filtered(fld, 0x0049C038, 0x1EB8).size() == 0x1EB8,
        "packaged Chery FLD window is invalid");
  check(uds::load_srecord_window(app, 0x000C1000, 0x7B000).size() == 0x7B000,
         "packaged Chery APP window is invalid");
  check(uds::load_srecord_window(cal, 0x000B0000, 0x200).size() == 0x200,
        "packaged Chery CAL window is invalid");
  check(uds::load_hex_bytes(driver_rsa, 512, 512).size() == 512 &&
            uds::load_hex_bytes(app_rsa, 512, 512).size() == 512 &&
            uds::load_hex_bytes(cal_rsa, 512, 512).size() == 512,
         "packaged Chery RSA payload is invalid");
}

void test_chery_kp31_protocol_and_resources() {
  const auto request_download =
      uds::chery_kp31_request_download(0x08000000, 0x400);
  check(request_download == std::vector<std::uint8_t>(
          {0x34, 0x00, 0x44, 0x08, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x04, 0x00}),
        "Chery KP31 Driver RequestDownload encoding mismatch");
  const auto erase =
      uds::chery_kp31_erase_memory(0xC0080000, 0xF5000);
  check(erase == std::vector<std::uint8_t>(
          {0x31, 0x01, 0xFF, 0x00, 0x44, 0xC0, 0x08, 0x00,
           0x00, 0x00, 0x0F, 0x50, 0x00}),
        "Chery KP31 FF00 erase encoding mismatch");
  const auto cal_erase =
      uds::chery_kp31_erase_memory(0xC0180000, 0xC8);
  check(cal_erase == std::vector<std::uint8_t>(
          {0x31, 0x01, 0xFF, 0x00, 0x44, 0xC0, 0x18, 0x00,
           0x00, 0x00, 0x00, 0x00, 0xC8}),
        "Chery KP31 CAL erase encoding mismatch");
  check(uds::chery_kp31_max_block_length(
            std::array<std::uint8_t, 6>{0x74, 0x40, 0x00, 0x00, 0x04, 0x00}) ==
            0x400 &&
            uds::chery_kp31_max_block_length(
                std::array<std::uint8_t, 6>{0x74, 0x40, 0x00, 0x00, 0x00,
                                            0x00}) == 0x400,
        "Chery KP31 max block length decoding mismatch");
  const auto app_plan = uds::resolve_chery_kp31_download_plan(L"app");
  const auto cal_plan = uds::resolve_chery_kp31_download_plan(L"cal");
  const auto app_cal_plan =
      uds::resolve_chery_kp31_download_plan(L"app_cal");
  check(app_plan.mode == uds::CheryKp31FlashMode::AppOnly &&
            app_plan.download_app && !app_plan.download_cal &&
            cal_plan.mode == uds::CheryKp31FlashMode::CalOnly &&
            !cal_plan.download_app && cal_plan.download_cal &&
            app_cal_plan.mode == uds::CheryKp31FlashMode::AppCal &&
            app_cal_plan.download_app && app_cal_plan.download_cal,
        "Chery KP31 APP/CAL/APP+CAL mode resolution mismatch");
  bool invalid_mode_rejected = false;
  try {
    static_cast<void>(uds::resolve_chery_kp31_download_plan(L"ft"));
  } catch (const std::invalid_argument&) {
    invalid_mode_rejected = true;
  }
  check(invalid_mode_rejected,
        "Chery KP31 unsupported FT mode was not rejected");

  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile =
      uds::load_profile_ini(source / "profiles" / "chery_kp31.ini");
  check(profile.id == L"chery_kp31" && profile.flow == L"chery_kp31" &&
            profile.vendor_name == L"奇瑞" &&
            profile.project_name == L"KP31" &&
            profile.device_name == L"雷达" && !profile.placeholder &&
            !profile.can_fd && !profile.power_control &&
            !profile.supports_ft_entry && profile.supports_cal_download &&
            profile.lock_diagnostic_ids &&
            profile.default_entry_mode == L"app" &&
            profile.tx_id == 0x70D && profile.rx_id == 0x78D &&
            profile.functional_id == 0x7DF && profile.channel == 1 &&
            profile.nominal_bitrate == 500000 && profile.padding == 0x55 &&
            profile.isotp_st_min == 0 && profile.security_level == 0x11 &&
            profile.security_variant.empty() &&
            profile.driver_start == 0x08000000 &&
            profile.driver_length == 0x400 &&
            profile.app_start == 0xC0080000 &&
            profile.app_length == 0xF5000 &&
            profile.cal_start == 0xC0180000 &&
            profile.cal_length == 0xC8 && profile.driver_file.empty() &&
            profile.app_file.empty() && profile.driver_verify_file.empty() &&
            profile.cal_file.empty() && profile.app_verify_file.empty() &&
            profile.cal_verify_file.empty() && profile.targets.size() == 1 &&
            profile.targets[0].id == L"radar" &&
            profile.targets[0].display_name == L"雷达" &&
            profile.targets[0].tx_id == 0x70D &&
            profile.targets[0].rx_id == 0x78D &&
            profile.targets[0].pending_validation,
        "Chery KP31 packaged profile mismatch");

  const auto root = source / "resources" / "chery_kp31";
  check(std::filesystem::file_size(
            root / "dll" / "CHERY_E0Y_UPDATE23231115.dll") == 864256 &&
            std::filesystem::file_size(
                root / "CDD" / "E0Y.110.110_20260309.100.cdd") == 618085 &&
            std::filesystem::file_size(root / "Reference" / "Flash.can") ==
                69842 &&
            std::filesystem::file_size(root / "Driver" / "FlashDrv.s19") ==
                2616,
        "Chery KP31 packaged reference resources mismatch");
}

void test_chery_ars131_project_contracts() {
  const auto& t1ej = uds::chery_ars1_31_app_spec(uds::CheryArs131Project::t1ej);
  const auto& t22 = uds::chery_ars1_31_app_spec(uds::CheryArs131Project::t22);
  const auto& e0y = uds::chery_ars1_31_app_spec(uds::CheryArs131Project::e0y);
  check(t1ej.tx_id == 0x7AF && t1ej.rx_id == 0x7BF &&
            t1ej.seed_subfunction == 0x07 && t1ej.seed_length == 4 &&
            t1ej.fingerprint_did == 0xF15A &&
            t1ej.d004_mode == uds::CheryArs131D004Mode::routine_only &&
            t1ej.install_d005 && t1ej.restore_default_session,
        "T1EJ frozen normal-flow contract mismatch");
  check(t22.tx_id == 0x7AF && t22.rx_id == 0x7BF &&
            t22.seed_subfunction == 0x07 && t22.seed_length == 4 &&
            t22.fingerprint_did == 0xF15A &&
            t22.d004_mode == uds::CheryArs131D004Mode::app_signature &&
            t22.post_d004_delay == std::chrono::milliseconds(2000) &&
            t22.initial_physical_extended_session && t22.install_d005 &&
            !t22.restore_default_session,
        "T22 frozen normal-flow contract mismatch");
  check(e0y.tx_id == 0x70D && e0y.rx_id == 0x78D &&
            e0y.seed_subfunction == 0x11 && e0y.seed_length == 16 &&
            e0y.fingerprint_did == 0xF184 &&
            e0y.precondition_routine == 0x0203 &&
            e0y.verification_routine == 0xDD02 &&
            e0y.d004_mode == uds::CheryArs131D004Mode::none &&
            !e0y.install_d005,
        "E0Y frozen normal-flow contract mismatch");
  check(uds::chery_ars1_31_request_download(0x08000000, 0x400) ==
            std::vector<std::uint8_t>({0x34, 0x00, 0x44, 0x08, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x04, 0x00}) &&
            uds::chery_ars1_31_erase_memory(0xC0080000, 0xF5000) ==
            std::vector<std::uint8_t>({0x31, 0x01, 0xFF, 0x00, 0x44,
                                      0xC0, 0x08, 0x00, 0x00, 0x00,
                                      0x0F, 0x50, 0x00}),
        "Chery ARS1.31 common address encoding mismatch");
  const auto public_key_request = uds::chery_e0y_update_public_key_request();
  check(public_key_request.size() == 517 &&
            public_key_request[0] == 0x2E &&
            public_key_request[1] == 0x6F &&
            public_key_request[2] == 0x00 &&
            uds::to_hex(uds::sha256(std::span(public_key_request).subspan(3))) ==
                "51 7F B5 82 90 44 5B AC CA 04 2A 72 59 45 44 EB "
                "64 E5 A4 1C D3 4A 9E 4A 4E 72 43 EA 66 E6 07 62",
        "E0Y Update_PublicKey request differs from CANoe publickeydata[514]");
  const auto t1ej_app = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::t1ej, L"app");
  const auto t1ej_cal = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::t1ej, L"cal");
  const auto t1ej_app_cal = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::t1ej, L"app_cal");
  const auto e0y_cal = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::e0y, L"cal");
  const auto e0y_app_cal = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::e0y, L"app_cal");
  const auto t22_cal = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::t22, L"cal");
  const auto t22_app_cal = uds::resolve_chery_ars1_31_download_plan(
      uds::CheryArs131Project::t22, L"app_cal");
  check(t1ej_app.download_app && !t1ej_app.download_cal &&
            !t1ej_cal.download_app && t1ej_cal.download_cal &&
            t1ej_app_cal.download_app && t1ej_app_cal.download_cal &&
            !e0y_cal.download_app && e0y_cal.download_cal &&
            e0y_app_cal.download_app && e0y_app_cal.download_cal &&
            !t22_cal.download_app && t22_cal.download_cal &&
            t22_app_cal.download_app && t22_app_cal.download_cal,
        "T1EJ/T22/E0Y APP/TC_7/TC_2 mode resolution mismatch");
  bool invalid_mode_rejected = false;
  try {
    static_cast<void>(uds::resolve_chery_ars1_31_download_plan(
        uds::CheryArs131Project::t22, L"ft"));
  } catch (const std::invalid_argument&) {
    invalid_mode_rejected = true;
  }
  check(invalid_mode_rejected, "T22 unexpectedly accepted unsupported FT mode");

  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  for (const auto* profile_name : {"chery_t1ej.ini", "chery_t22.ini",
                                   "chery_e0y.ini"}) {
    const auto profile = uds::load_profile_ini(source / "profiles" / profile_name);
    const auto workflow = uds::create_flash_workflow(profile.flow);
    check(workflow && workflow->id() == profile.flow &&
              !profile.lock_diagnostic_ids &&
              !workflow->report_title(profile).empty() &&
              !profile.driver_file.empty() && !profile.app_file.empty() &&
              !profile.driver_verify_file.empty() &&
              !profile.app_verify_file.empty() && !profile.security_dll.empty(),
          std::string("independent Chery workflow/profile mismatch: ") +
              profile_name);
    check(uds::load_srecord_window(source / profile.driver_file,
                                   profile.driver_start,
                                   profile.driver_length).size() ==
                  profile.driver_length &&
              uds::load_srecord_window(source / profile.app_file,
                                       profile.app_start,
                                       profile.app_length).size() ==
                  profile.app_length &&
              uds::load_hex_bytes(source / profile.driver_verify_file,
                                  512, 512).size() == 512 &&
              uds::load_hex_bytes(source / profile.app_verify_file,
                                  512, 512).size() == 512 &&
              std::filesystem::is_regular_file(source / profile.security_dll),
          std::string("Chery packaged resources mismatch: ") + profile_name);
    check(profile.supports_cal_download &&
                profile.cal_start == 0xC0180000 &&
                profile.cal_length == 0xC8 &&
                !profile.cal_file.empty() &&
                !profile.cal_verify_file.empty() &&
                uds::load_srecord_window(source / profile.cal_file,
                                         profile.cal_start,
                                         profile.cal_length).size() ==
                    profile.cal_length &&
                uds::load_hex_bytes(source / profile.cal_verify_file,
                                    512, 512).size() == 512,
          "T1EJ/T22/E0Y CAL/TC_7 and APP+CAL/TC_2 resources mismatch");
    if (profile.id == L"chery_t22") {
      check(profile.cal_file.filename() ==
                    L"T22_inter_ICE_S0000038443_20260316.s19" &&
                profile.cal_verify_file.filename() ==
                    L"CIR_S0000038443_000202_CAL1_MCU_UDS_20260317.rsa" &&
                uds::load_srecord_window(
                    source / "resources/chery_t22/CAL/"
                             "T22_inter_PHEV_S0000016021_20260316.s19",
                    profile.cal_start, profile.cal_length).size() ==
                    profile.cal_length &&
                uds::load_hex_bytes(
                    source / "resources/chery_t22/Verification/"
                             "CIR_S0000016021_000202_CAL1_MCU_UDS_20260317.rsa",
                    512, 512).size() == 512,
            "T22 Panel default ICE or alternate PHEV CAL/RSA pairing mismatch");
    }
  }
}

void test_lp_arf_tmp_packages() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  for (const auto& package_path : {
           source / "resources/lp_arf231_a12/Verification/"
                    "LP-MRS050-BA_V3.01.07_R_20260608.tmp",
           source / "resources/lp_arf231_b11/Verification/"
                    "LP-MRS050-BA_V2.10.16_R_20240802.tmp"}) {
    const auto package =
        uds::load_leapmotor_tmp(package_path);
    const auto artifacts =
        uds::load_lp_arf_artifacts(package_path);
    const auto hash = uds::sha256(package.app.data);
    check(package.app.address == uds::kLpArfAppAddress &&
              artifacts.certificate_embedded &&
              artifacts.images.app.address == package.app.address &&
              artifacts.images.app.data == package.app.data &&
              artifacts.images.certificate == package.certificate &&
              package.app.data.size() == uds::kLpArfAppLength &&
              package.certificate.size() == uds::kLpArfCertificateLength &&
              package.metadata_json.find("\"SignInfoLen\":\t1322") !=
                  std::string::npos &&
              std::equal(hash.begin(), hash.end(),
                         package.certificate.begin()),
          std::string("ARF structured TMP package mismatch: ") +
              package_path.string());
  }
  const auto valid_tmp =
      source / "resources/lp_arf231_a12/Verification/"
               "LP-MRS050-BA_V3.01.07_R_20260608.tmp";
  std::ifstream package_input(valid_tmp, std::ios::binary);
  std::vector<std::uint8_t> corrupted_package(
      (std::istreambuf_iterator<char>(package_input)), {});
  const auto metadata_length =
      (static_cast<std::uint32_t>(corrupted_package[4]) << 24U) |
      (static_cast<std::uint32_t>(corrupted_package[5]) << 16U) |
      (static_cast<std::uint32_t>(corrupted_package[6]) << 8U) |
      static_cast<std::uint32_t>(corrupted_package[7]);
  const auto app_offset = 8U + metadata_length + 13U;
  corrupted_package[app_offset] ^= 0x01U;
  const auto corrupted_tmp =
      std::filesystem::temp_directory_path() /
      "uds_cpp_corrupted_leapmotor.tmp";
  {
    std::ofstream output(corrupted_tmp, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(corrupted_package.data()),
                 static_cast<std::streamsize>(corrupted_package.size()));
  }
  bool corrupted_parsed = false;
  try {
    const auto parsed = uds::load_leapmotor_tmp(corrupted_tmp);
    corrupted_parsed =
        parsed.app.data.size() == uds::kLpArfAppLength &&
        parsed.app.data.front() == corrupted_package[app_offset];
  } catch (const std::runtime_error&) {
    corrupted_parsed = false;
  }
  std::error_code remove_error;
  std::filesystem::remove(corrupted_tmp, remove_error);
  check(corrupted_parsed,
        "Leapmotor TMP parser rejected a structurally valid package because of local integrity policy");

  const auto external_s19 =
      source / "resources/lp_arf/APP/"
               "ARF6.31V1.0_PF4T4R_B1.00.01_APP_V1.00.04_CHF0383N_without_boot.s19";
  const auto unbound_artifacts =
      uds::load_lp_arf_artifacts(external_s19, valid_tmp);
  check(!unbound_artifacts.certificate_embedded &&
            unbound_artifacts.images.app.data.size() ==
                uds::kLpArfAppLength &&
            unbound_artifacts.images.certificate.size() ==
                uds::kLpArfCertificateLength,
        "LP-ARF S19+TMP parsing still enforces a local APP/certificate binding policy");
  const auto app_only_artifacts =
      uds::load_lp_arf_artifacts(external_s19);
  check(!app_only_artifacts.certificate_embedded &&
            app_only_artifacts.images.app.address == uds::kLpArfAppAddress &&
            app_only_artifacts.images.app.data.size() ==
                uds::kLpArfAppLength &&
            app_only_artifacts.images.certificate.empty(),
        "LP-ARF did not accept APP-only input for CANoe-style certificate Skip");
  const auto n61 = uds::load_profile_ini(
      source / "profiles" / "baic_n61ab.ini");
  const auto bqb41 = uds::load_profile_ini(
      source / "profiles" / "baic_bqb41.ini");
  check(!n61.placeholder && n61.flow == L"baic_n61ab" &&
            n61.tx_id == 0x723 && n61.rx_id == 0x72B &&
            !n61.can_fd && n61.driver_start == 0x08000000 &&
            n61.app_start == 0xC0080000 &&
            !bqb41.placeholder && bqb41.flow == L"baic_bqb41" &&
            bqb41.can_fd && bqb41.uds_fd && bqb41.uds_brs &&
            bqb41.targets.size() == 4 &&
            bqb41.driver_start == 0x00000000 &&
            bqb41.app_start == 0x00040000 &&
            !bqb41.security_dll.empty(),
        "BAIC N61AB/BQB41 profile contracts are not frozen correctly");
  const auto n61_versions = uds::load_version_check_plan(
      source / "profiles" / "baic_n61ab.ini", {});
  const auto bqb41_versions = uds::load_version_check_plan(
      source / "profiles" / "baic_bqb41.ini", L"bqb41_0");
  check(n61_versions.session == 0x01 && n61_versions.items.size() == 9 &&
            n61_versions.items[0].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x87}) &&
            n61_versions.items[1].decoder == L"hex" &&
            n61_versions.items[4].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x95}) &&
            !n61_versions.items[8].required &&
            n61_versions.items[8].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0xA1}) &&
            bqb41_versions.session == 0x01 &&
            bqb41_versions.items.size() == 10 &&
            bqb41_versions.items[0].required &&
            bqb41_versions.items[1].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x83}) &&
            !bqb41_versions.items[9].required &&
            bqb41_versions.items[9].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x99}),
        "BAIC N61AB/BQB41 version-read DID plans are not frozen correctly");
  const auto n61_driver = uds::load_srecord_window(
      source / n61.driver_file, n61.driver_start, n61.driver_length);
  const auto n61_app = uds::load_srecord_window(
      source / n61.app_file, n61.app_start, n61.app_length);
  check(n61_driver.size() == 0x400 && n61_app.size() == 0xF5000 &&
            std::filesystem::file_size(source / n61.security_dll) == 834048 &&
            std::filesystem::file_size(source / bqb41.security_dll) == 909312,
        "BAIC runtime S19/SeedKey resources are incomplete");
}

void test_baic_radar_protocol() {
  check(uds::baic_radar_request_download(0x08000000, 0x400) ==
            std::vector<std::uint8_t>(
                {0x34, 0x00, 0x44, 0x08, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x04, 0x00}),
        "BAIC N61AB Driver RequestDownload encoding mismatch");
  check(uds::baic_radar_erase_memory(0x00040000, 0x80000) ==
            std::vector<std::uint8_t>(
                {0x31, 0x01, 0xFF, 0x00, 0x44,
                 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}),
        "BAIC BQB41 APP erase encoding mismatch");
  check(uds::baic_radar_max_block_length(
            std::array<std::uint8_t, 4>{0x74, 0x20, 0x03, 0xEF}) ==
            0x3EF,
        "BAIC BQB41 max block length decoding mismatch");
  constexpr std::array<std::uint8_t, 9> test_vector{
      '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  check(uds::baic_radar_crc32(test_vector) == 0x340BC6D9U,
        "BAIC CRC32 does not match the archived CAPL no-final-XOR algorithm");

  uds::FlashProfile configurable_profile;
  configurable_profile.tx_id = 0x700;
  configurable_profile.rx_id = 0x708;
  bool n61_custom_accepted = true;
  bool bqb41_custom_accepted = true;
  try {
    uds::validate_baic_configurable_endpoint(
        configurable_profile, uds::BaicRadarProject::n61ab);
  } catch (const std::runtime_error&) {
    n61_custom_accepted = false;
  }
  try {
    uds::validate_baic_configurable_endpoint(
        configurable_profile, uds::BaicRadarProject::bqb41);
  } catch (const std::runtime_error&) {
    bqb41_custom_accepted = false;
  }
  check(n61_custom_accepted && bqb41_custom_accepted,
        "BAIC workflow rejected a valid configurable APP endpoint");
  configurable_profile.tx_id = 0x1800;
  bool invalid_standard_id_rejected = false;
  try {
    uds::validate_baic_configurable_endpoint(
        configurable_profile, uds::BaicRadarProject::bqb41);
  } catch (const std::runtime_error&) {
    invalid_standard_id_rejected = true;
  }
  check(invalid_standard_id_rejected,
        "BAIC workflow accepted an out-of-range standard CAN endpoint");
}

void test_longma_ars131_protocol_and_resources() {
  check(uds::longma_ars131_endpoint_supported(0x744, 0x74C) &&
            uds::longma_ars131_endpoint_supported(0x760, 0x768) &&
            !uds::longma_ars131_endpoint_supported(0x744, 0x768) &&
            !uds::longma_ars131_secondary_endpoint(0x744, 0x74C) &&
            uds::longma_ars131_secondary_endpoint(0x760, 0x768),
        "Longma ARS1.31 main/secondary endpoint classification mismatch");
  const auto request_download =
      uds::longma_ars131_request_download(0x08000000, 0x400);
  check(request_download == std::vector<std::uint8_t>(
          {0x34, 0x00, 0x44, 0x08, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x04, 0x00}),
        "Longma ARS1.31 Driver RequestDownload encoding mismatch");

  const auto erase = uds::longma_ars131_erase_memory(0xC0080000, 0xF5000);
  check(erase == std::vector<std::uint8_t>(
          {0x31, 0x01, 0xFF, 0x00, 0xC0, 0x08, 0x00, 0x00,
           0x00, 0x0F, 0x50, 0x00}),
        "Longma ARS1.31 FF00 erase request mismatch");
  check(uds::longma_ars131_max_block_length(
            std::array<std::uint8_t, 4>{0x74, 0x20, 0x08, 0x02}) == 0x802,
        "Longma ARS1.31 max block length decoding mismatch");
  const auto app_plan = uds::resolve_longma_ars131_download_plan(L"app");
  const auto ft_plan = uds::resolve_longma_ars131_download_plan(L"ft");
  const auto cal_plan = uds::resolve_longma_ars131_download_plan(L"cal");
  const auto app_cal_plan =
      uds::resolve_longma_ars131_download_plan(L"app_cal");
  check(!app_plan.ft_entry && app_plan.download_app &&
            !app_plan.download_cal && ft_plan.ft_entry &&
            ft_plan.download_app && !ft_plan.download_cal &&
            !cal_plan.ft_entry && !cal_plan.download_app &&
            cal_plan.download_cal && !app_cal_plan.ft_entry &&
            app_cal_plan.download_app && app_cal_plan.download_cal,
        "Longma ARS1.31 operation-mode plan mismatch");

  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile =
      uds::load_profile_ini(source / "profiles" / "longma_ars1_31.ini");
  check(profile.id == L"longma_ars1_31" &&
             profile.flow == L"longma_ars1_31" &&
             profile.name == L"长马 1.31" && !profile.placeholder &&
             !profile.can_fd && !profile.power_control &&
             profile.supports_ft_entry && !profile.supports_cal_download &&
             profile.default_entry_mode == L"app" &&
             profile.tx_id == 0x744 && profile.rx_id == 0x74C &&
             profile.functional_id == 0x7DF &&
             profile.ft_tx_id == 0x714 && profile.ft_rx_id == 0x71C &&
             !profile.ft_extended_id && !profile.ft_uds_fd &&
             !profile.ft_uds_brs && profile.ft_padding == 0x00 &&
             profile.channel == 2 &&
            profile.nominal_bitrate == 500000 && profile.padding == 0x00 &&
            profile.isotp_st_min == 0 && profile.security_level == 0x01 &&
            profile.security_variant.empty() &&
            profile.driver_start == 0x08000000 &&
            profile.driver_length == 0x400 &&
            profile.app_start == 0xC0080000 &&
            profile.app_length == 0xF5000 &&
            profile.expected_driver_crc16 ==
                uds::kLongmaArs131ReferenceDriverCrc &&
            profile.targets.size() == 2 &&
             profile.targets[0].tx_id == 0x744 &&
             profile.targets[0].rx_id == 0x74C &&
             profile.targets[0].ft_tx_id == 0x714 &&
             profile.targets[0].ft_rx_id == 0x71C &&
             profile.targets[0].expected_app_crc16 ==
                 uds::kLongmaArs131ReferenceAppCrc &&
             profile.targets[1].tx_id == 0x760 &&
             profile.targets[1].rx_id == 0x768 &&
             profile.targets[1].ft_tx_id == 0x714 &&
             profile.targets[1].ft_rx_id == 0x71C &&
             profile.targets[1].pending_validation,
        "Longma ARS1.31 packaged profile mismatch");

  const auto root = source / "resources" / "longma_ars1_31";
  const auto driver = uds::load_srecord_window(
      root / "Driver" / "ARS1.31C3A_J90K_FlashDriver.s19",
      0x08000000, 0x400);
  const auto app = uds::load_srecord_window(
      root / "APP" /
          "ARS1.31C3A_J90KAF_V1.9_APP_V2.1.00_CHF0326N_without_boot.s19",
      0xC0080000, 0xF5000);
  check(uds::longma_ars131_crc16_ccitt_false(driver) ==
            uds::kLongmaArs131ReferenceDriverCrc,
        "Longma ARS1.31 packaged Driver CRC differs from passing CANoe trace");
  check(uds::longma_ars131_crc16_ccitt_false(app) ==
            uds::kLongmaArs131ReferenceAppCrc,
        "Longma ARS1.31 packaged APP CRC differs from passing CANoe trace");
  check(std::filesystem::file_size(root / "dll" / "S202_SeednKey_cdd .dll") ==
            777728 &&
            std::filesystem::file_size(root / "CDD" / "EP32_V1.7.100.cdd") ==
            1168531,
        "Longma ARS1.31 packaged DLL/CDD resource mismatch");
}

void test_c857_project_profiles_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto validate = [&](const std::wstring& id,
                            const std::wstring& device_name,
                            const std::filesystem::path& resource_directory) {
  const auto profile =
      uds::load_profile_ini(source / "profiles" / (id + L".ini"));
  check(profile.id == id && profile.flow == id &&
            profile.name == std::wstring(L"长安 ") + device_name +
                                L" ARS1.31" &&
            profile.vendor_name == L"长安" &&
            profile.project_name == device_name &&
            profile.device_name == device_name && !profile.placeholder &&
            !profile.can_fd && !profile.power_control &&
            profile.supports_ft_entry && profile.supports_cal_download &&
            profile.lock_diagnostic_ids &&
            profile.default_entry_mode == L"app" &&
            profile.ft_tx_id == 0x715 && profile.ft_rx_id == 0x71D &&
            !profile.ft_extended_id && !profile.ft_uds_fd &&
            !profile.ft_uds_brs && profile.ft_padding == 0x00 &&
            profile.tx_id == 0x744 &&
            profile.rx_id == 0x74C && profile.functional_id == 0x7DF &&
            profile.channel == 2 && profile.nominal_bitrate == 500000 &&
            profile.data_bitrate == 2000000 && profile.padding == 0x00 &&
            profile.isotp_st_min == 0 && profile.security_level == 0x01 &&
            profile.security_variant.empty() &&
            profile.driver_start == 0x08000000 &&
            profile.driver_length == 0x400 &&
            profile.app_start == 0xC0080000 &&
            profile.app_length == 0xF5000 &&
            profile.cal_file.filename() == L"ICRF_002_003.s19" &&
            profile.cal_start == 0xC0180000 &&
            profile.cal_length == 0x270 &&
            profile.expected_driver_crc16 == 0xCC3C &&
            profile.targets.size() == 2,
        "C857 project packaged profile mismatch");

  const auto& main = profile.targets[0];
  const auto& secondary = profile.targets[1];
  check(main.id == L"main" && main.tx_id == 0x744 &&
            main.rx_id == 0x74C && !main.pending_validation &&
            main.ft_tx_id == 0x715 && main.ft_rx_id == 0x71D &&
            main.display_name == L"主雷达（前雷达 ICRF）" &&
            main.expected_app_crc16 == 0x7587 &&
            main.app_file.filename() ==
                L"ARS1.31C3A_C857AF_V1.6_APP_SWD.00.7_"
                L"CHF0301N_without_boot.s19" &&
            main.cal_file.filename() == L"ICRF_002_003.s19" &&
            main.security_dll.filename() == L"SeedKey_Main.dll" &&
            secondary.id == L"secondary" &&
            secondary.tx_id == 0x760 && secondary.rx_id == 0x768 &&
            secondary.ft_tx_id == 0x714 &&
            secondary.ft_rx_id == 0x71C &&
            secondary.expected_app_crc16 == 0xB5E2 &&
            secondary.app_file.filename() ==
                L"ARS1.31C3A_C857AR_V1.6_APP_SWD.00.7_"
                L"CHF0303N_without_boot.s19" &&
            secondary.cal_file.filename() == L"ICRR_001_003.s19" &&
            secondary.security_dll.filename() == L"SeedKey_Slave.dll",
        "C857 project target endpoint/resource mapping mismatch");
  check(!main.pending_validation,
        id == L"lingyao_b216"
            ? "B216 main radar validation status mismatch: expected "
              "pending_validation=false"
            : "Changan C857 main radar validation status mismatch: expected "
              "pending_validation=false");
  check(!secondary.pending_validation,
        id == L"lingyao_b216"
            ? "B216 secondary radar validation status mismatch: expected "
              "pending_validation=false"
            : "Changan C857 secondary radar validation status mismatch: "
               "expected pending_validation=false");

  const auto profile_path = source / "profiles" / (id + L".ini");
  const auto main_versions =
      uds::load_version_check_plan(profile_path, L"main");
  const auto secondary_versions =
      uds::load_version_check_plan(profile_path, L"secondary");
  check(main_versions.session == 0x01 &&
             main_versions.precondition == L"ars131_0x400" &&
             main_versions.items.size() == 3 &&
             secondary_versions.items.size() == 3,
         "C857/B216 version-check plan shape mismatch");
  check(main_versions.items[0].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x89}) &&
            main_versions.items[1].request ==
                std::vector<std::uint8_t>({0x22, 0xF1, 0x70}) &&
            main_versions.items[2].request ==
                std::vector<std::uint8_t>({0x22, 0xFD, 0x05}) &&
            main_versions.items[0].expected.empty() &&
            main_versions.items[1].expected.empty() &&
            main_versions.items[2].expected.empty() &&
            secondary_versions.items[0].expected.empty() &&
            secondary_versions.items[1].expected.empty() &&
            secondary_versions.items[2].expected.empty(),
        "C857/B216 read-only version plan mismatch");

  const auto root = source / "resources" / resource_directory;
  const auto driver = uds::load_srecord_window(
      root / "Driver" / "ARS1.31C3A_C857_FlashDriver.s19",
      0x08000000, 0x400);
  const auto main_app = uds::load_srecord_window(
      root / "APP" / "ICRF" /
          "ARS1.31C3A_C857AF_V1.6_APP_SWD.00.7_"
          "CHF0301N_without_boot.s19",
      0xC0080000, 0xF5000);
  const auto secondary_app = uds::load_srecord_window(
      root / "APP" / "ICRR" /
          "ARS1.31C3A_C857AR_V1.6_APP_SWD.00.7_"
          "CHF0303N_without_boot.s19",
      0xC0080000, 0xF5000);
  const auto main_cal = uds::load_srecord_window(
      root / "CAL" / "ICRF_002_003.s19", 0xC0180000, 0x270);
  const auto secondary_cal = uds::load_srecord_window(
      root / "CAL" / "ICRR_001_003.s19", 0xC0180000, 0x270);
  check(uds::longma_ars131_crc16_ccitt_false(driver) == 0xCC3C &&
            uds::longma_ars131_crc16_ccitt_false(main_app) == 0x7587 &&
            uds::longma_ars131_crc16_ccitt_false(secondary_app) == 0xB5E2 &&
            uds::longma_ars131_crc16_ccitt_false(main_cal) == 0xE957 &&
            uds::longma_ars131_crc16_ccitt_false(secondary_cal) == 0x483A,
        "C857 project packaged image CRC differs from the target contract");
  check(std::filesystem::file_size(root / "dll" / "SeedKey_Main.dll") ==
                777728 &&
            std::filesystem::file_size(root / "dll" /
                                       "SeedKey_Slave.dll") == 933376 &&
            std::filesystem::file_size(root / "CDD" /
                                       "EP32_V1.7.100.cdd") == 1168531,
        "C857 project DLL/CDD/CAL provenance resources mismatch");

  const auto workflow = uds::create_flash_workflow(id);
  check(workflow && workflow->id() == id &&
            workflow->report_title(profile).find("Main Radar") !=
                std::string::npos,
        "C857 project workflow factory/report mapping mismatch");
  };
  validate(L"changan_c857", L"C857", L"changan_c857");
  validate(L"lingyao_b216", L"B216", L"lingyao_b216");
}

struct ShidaixinanUdsRequest {
  std::uint32_t id{};
  std::vector<std::uint8_t> payload;
};

class ScriptedShidaixinanBus final : public uds::ICanBus {
public:
  explicit ScriptedShidaixinanBus(bool ft_mode)
      : ft_mode_(ft_mode) {}

  void open() override { open_ = true; }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }

  void send(const uds::CanFrame& frame) override {
    const auto payload = decode(frame.data);
    if (payload.empty()) return;
    requests.push_back({frame.id, payload});
    handle(frame.id, payload);
  }

  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds) override {
    if (rx_.empty()) return std::nullopt;
    auto response = std::move(rx_.front());
    rx_.pop_front();
    return response;
  }

  std::vector<ShidaixinanUdsRequest> requests;

private:
  static std::vector<std::uint8_t> decode(
      std::span<const std::uint8_t> frame) {
    if (frame.empty()) return {};
    if (frame[0] == 0x00 && frame.size() > 8U) {
      if (frame.size() < 2U ||
          frame.size() < 2U + frame[1]) {
        return {};
      }
      return {frame.begin() + 2,
              frame.begin() + 2 + frame[1]};
    }
    if ((frame[0] >> 4U) != 0U) return {};
    const auto length =
        static_cast<std::size_t>(frame[0] & 0x0FU);
    if (length == 0U || frame.size() < 1U + length) return {};
    return {frame.begin() + 1,
            frame.begin() + 1 +
                static_cast<std::ptrdiff_t>(length)};
  }

  void reply(std::uint32_t id,
             std::initializer_list<std::uint8_t> payload) {
    std::vector<std::uint8_t> data(8U, 0xAA);
    data[0] = static_cast<std::uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), data.begin() + 1);
    rx_.push_back(
        uds::CanFrame{id, std::move(data), false, true, false});
  }

  void handle(std::uint32_t id,
              const std::vector<std::uint8_t>& payload) {
    if (id == 0x7DFU) {
      if (payload == std::vector<std::uint8_t>({0x10, 0x03})) {
        if (reset_seen_) {
          ++post_reset_session_attempts_;
          if (ft_mode_ && post_reset_session_attempts_ == 1U) {
            return;
          }
          reply(0x7AC, {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
        } else {
          reply(ft_mode_ ? 0x761U : 0x7ACU,
                {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
        }
        return;
      }
      if (payload == std::vector<std::uint8_t>({0x10, 0x02})) {
        reply(0x761, {0x7F, 0x10, 0x78});
        return;
      }
      if (payload ==
          std::vector<std::uint8_t>({0x85, 0x02})) {
        reply(0x7AC, {0xC5, 0x02});
        return;
      }
      if (payload ==
          std::vector<std::uint8_t>({0x28, 0x03, 0x03})) {
        reply(0x7AC, {0x68, 0x03});
        return;
      }
      if (payload ==
          std::vector<std::uint8_t>({0x28, 0x00, 0x03})) {
        reply(0x7AC, {0x68, 0x00});
        return;
      }
      if (payload ==
          std::vector<std::uint8_t>({0x10, 0x01})) {
        reply(0x7AC, {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4});
        return;
      }
    }
    if (id != 0x7A4U || payload.empty()) return;
    switch (payload[0]) {
    case 0x10:
      if (payload.size() >= 2U && payload[1] == 0x02) {
        reply(0x7AC, {0x50, 0x02, 0x00, 0x32, 0x01, 0xF4});
      }
      break;
    case 0x27:
      if (payload.size() < 2U) break;
      if (payload[1] == 0x01) {
        reply(0x7AC, {0x67, 0x01, 0x7A, 0x45, 0xFA, 0x55});
      } else if (payload[1] == 0x02) {
        reply(0x7AC, {0x67, 0x02});
      } else if (payload[1] == 0x03) {
        reply(0x7AC, {0x67, 0x03, 0xE9, 0x3F, 0xDF, 0xD0});
      } else if (payload[1] == 0x04) {
        reply(0x7AC, {0x67, 0x04});
      }
      break;
    case 0x2E:
      reply(0x7AC, {0x6E, 0xF1, 0x84});
      break;
    case 0x31:
      if (payload.size() < 4U) break;
      if (payload[2] == 0xF0 && payload[3] == 0x02) {
        reply(0x7AC, {0x71, 0x01, 0xF0, 0x02, 0x00});
      } else if (payload[2] == 0xF1 && payload[3] == 0xA0) {
        reply(0x7AC, {0x71, 0x01, 0xF1, 0xA0, 0x00});
      } else if (payload[2] == 0xFF && payload[3] == 0x00) {
        reply(0x7AC, {0x71, 0x01, 0xFF, 0x00, 0x00});
      } else if (payload[2] == 0xFF && payload[3] == 0x01) {
        reply(0x7AC, {0x71, 0x01, 0xFF, 0x01, 0x00});
      }
      break;
    case 0x34:
      reply(0x7AC, {0x74, 0x20, 0x08, 0x02});
      break;
    case 0x36:
      if (payload.size() >= 2U) {
        reply(0x7AC, {0x76, payload[1]});
      }
      break;
    case 0x37:
      reply(0x7AC, {0x77});
      break;
    case 0x11:
      reset_seen_ = true;
      reply(0x7AC, {0x51, 0x01});
      break;
    case 0x85:
      reply(0x7AC, {0xC5, 0x01});
      break;
    case 0x14:
      reply(0x7AC, {0x54});
      break;
    default:
      break;
    }
  }

  bool ft_mode_{};
  bool open_{true};
  bool reset_seen_{};
  unsigned post_reset_session_attempts_{};
  std::deque<uds::CanFrame> rx_;
};

bool request_equals(const ShidaixinanUdsRequest& request,
                    std::uint32_t id,
                    std::initializer_list<std::uint8_t> payload) {
  return request.id == id &&
         request.payload ==
             std::vector<std::uint8_t>(payload);
}

std::size_t find_request_after(
    const std::vector<ShidaixinanUdsRequest>& requests,
    std::size_t begin, std::uint32_t id,
    std::initializer_list<std::uint8_t> payload) {
  const auto found = std::find_if(
      requests.begin() + static_cast<std::ptrdiff_t>(begin),
      requests.end(), [id, payload](const auto& request) {
        return request_equals(request, id, payload);
      });
  return found == requests.end()
             ? requests.size()
             : static_cast<std::size_t>(
                   std::distance(requests.begin(), found));
}

void run_shidaixinan_flow_mode_test(
    uds::ShidaixinanHjzjFmrEntryMode mode) {
  const bool ft_mode =
      mode == uds::ShidaixinanHjzjFmrEntryMode::ft;
  ScriptedShidaixinanBus bus(ft_mode);
  uds::IsoTpConfig physical_config{
      0x7A4, 0x7AC, 0x00, 0, 0,
      std::chrono::milliseconds(2),
      std::chrono::milliseconds(2),
      false, false, true, false};
  physical_config.tx_data_length = 64;
  physical_config.batch_consecutive_frames = false;
  uds::IsoTpSession physical_transport(bus, physical_config);
  auto functional_config = physical_config;
  functional_config.tx_id = 0x7DF;
  uds::IsoTpSession functional_transport(bus, functional_config);
  auto ft_functional_config = physical_config;
  ft_functional_config.tx_id =
      uds::kShidaixinanHjzjFtFunctionalTxId;
  ft_functional_config.rx_id =
      uds::kShidaixinanHjzjFtFunctionalRxId;
  uds::IsoTpSession ft_functional_transport(
      bus, ft_functional_config);
  uds::UdsClient physical(physical_transport);
  uds::UdsClient functional(functional_transport);
  uds::UdsClient ft_functional(ft_functional_transport);

  uds::ShidaixinanHjzjFmrTiming timing;
  timing.p2 = std::chrono::milliseconds(2);
  timing.p2_star = std::chrono::milliseconds(2);
  timing.app_step_delay = std::chrono::milliseconds(0);
  timing.ft_step_delay = std::chrono::milliseconds(0);
  timing.ft_entry_window = std::chrono::milliseconds(20);
  timing.ft_retry_delay = std::chrono::milliseconds(0);
  timing.ft_post_reset_ready_window =
      std::chrono::milliseconds(20);
  std::vector<std::string> logs;
  uds::ShidaixinanHjzjFmrFlow flow(
      physical, functional, ft_functional,
      [&logs](int, const std::string& line) {
        logs.push_back(line);
      },
      {},
      [](std::span<const std::uint8_t>, unsigned) {
        return std::vector<std::uint8_t>(
            {0x11, 0x22, 0x33, 0x44});
      },
      {}, timing);
  uds::ShidaixinanHjzjFmrImages images;
  images.driver = {0x10200400, {0x10, 0x20, 0x30}};
  images.app = {0x000C0000, {0x40, 0x50, 0x60, 0x70}};
  flow.run(images, mode, {});
  check(flow.core_programming_completed(),
        "Shidaixinan flow did not mark the shared programming body complete");

  const auto& requests = bus.requests;
  check(!requests.empty() &&
            request_equals(requests.front(), 0x7DF,
                           {0x10, 0x03}),
        "Shidaixinan flow did not start with functional 10 03");
  if (ft_mode) {
    check(requests.size() >= 3U &&
              request_equals(requests[1], 0x7DF,
                             {0x10, 0x02}) &&
              request_equals(requests[2], 0x7A4,
                             {0x10, 0x02}),
          "Shidaixinan FT entry did not use 7DF send-only 10 02 then physical 10 02");
    const auto reset =
        find_request_after(requests, 0, 0x7A4, {0x11, 0x01});
    const auto first_ready = find_request_after(
        requests, reset + 1U, 0x7DF, {0x10, 0x03});
    const auto second_ready = find_request_after(
        requests, first_ready + 1U, 0x7DF, {0x10, 0x03});
    const auto enable_communication = find_request_after(
        requests, second_ready + 1U, 0x7DF,
        {0x28, 0x00, 0x03});
    const auto enable_dtc = find_request_after(
        requests, enable_communication + 1U, 0x7A4,
        {0x85, 0x01});
    const auto default_session = find_request_after(
        requests, enable_dtc + 1U, 0x7DF,
        {0x10, 0x01});
    const auto clear_dtc = find_request_after(
        requests, default_session + 1U, 0x7A4,
        {0x14, 0xFF, 0xFF, 0xFF});
    check(reset < first_ready && first_ready < second_ready &&
              second_ready < enable_communication &&
              enable_communication < enable_dtc &&
              enable_dtc < default_session &&
              default_session < clear_dtc &&
              clear_dtc + 1U == requests.size(),
          "Shidaixinan FT post-reset retry/cleanup order mismatch");
    check(std::any_of(
              logs.begin(), logs.end(), [](const auto& line) {
                return line.find("send-only") != std::string::npos;
              }) &&
              std::any_of(
                  logs.begin(), logs.end(), [](const auto& line) {
                    return line.find("readiness attempt 2") !=
                           std::string::npos;
                  }),
          "Shidaixinan FT flow did not log send-only semantics or post-reset retry");
  } else {
    check(std::none_of(
              requests.begin(), requests.end(),
              [](const auto& request) {
                return request_equals(request, 0x7DF,
                                      {0x10, 0x02});
              }) &&
              request_equals(requests.back(), 0x7A4,
                             {0x11, 0x01}),
          "Shidaixinan APP flow changed to the FT entry/cleanup sequence");
  }
}

void test_shidaixinan_hjzj_fmr_mode_sequences() {
  run_shidaixinan_flow_mode_test(
      uds::ShidaixinanHjzjFmrEntryMode::app);
  run_shidaixinan_flow_mode_test(
      uds::ShidaixinanHjzjFmrEntryMode::ft);
}

void test_shidaixinan_hjzj_fmr_protocol_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile = uds::load_profile_ini(
      source / "profiles" / "shidaixinan_hjzj_fmr.ini");
  check(profile.id == L"shidaixinan_hjzj_fmr" &&
            profile.flow == L"shidaixinan_hjzj_fmr" &&
            profile.vendor_name == L"时代新安" &&
            profile.project_name == L"HJZJ" &&
            profile.device_name == L"FMR 主雷达" &&
            !profile.placeholder && profile.can_fd &&
            !profile.power_control && !profile.extended_id &&
            profile.uds_fd && !profile.uds_brs &&
            profile.supports_ft_entry &&
            profile.default_entry_mode == L"app" &&
            profile.lock_diagnostic_ids &&
            profile.tx_id == 0x7A4 &&
            profile.rx_id == 0x7AC &&
            profile.functional_id == 0x7DF &&
            profile.ft_tx_id ==
                uds::kShidaixinanHjzjFtFunctionalTxId &&
            profile.ft_rx_id ==
                uds::kShidaixinanHjzjFtFunctionalRxId &&
            !profile.ft_extended_id && profile.ft_uds_fd &&
            !profile.ft_uds_brs && profile.ft_padding == 0x00 &&
            profile.channel == 2 &&
            profile.nominal_bitrate == 500000 &&
            profile.data_bitrate == 2000000 &&
            profile.padding == 0x00 &&
            profile.isotp_st_min == 0 &&
            profile.security_level == 0x01 &&
            profile.security_variant.empty() &&
            profile.driver_start == 0 &&
            profile.driver_length == 0 &&
            profile.app_start == 0 &&
            profile.app_length == 0,
        "Shidaixinan HJZJ_FMR packaged profile mismatch");

  const auto root =
      source / "resources" / "shidaixinan_hjzj_fmr";
  const auto driver = uds::load_single_srecord_segment(
      root / "Driver" / "ARF2_32_ERadar_FlashDrv.s19");
  const auto app = uds::load_single_srecord_segment(
      root / "APP" /
      "ARF2.32CC3_SDTWCA_BV1.02_APP_SW1.17_CHF0385N_without_boot.s19");
  check(driver.address == 0x10200400 &&
            driver.data.size() == 0x400 &&
            app.address == 0x000C0000 &&
            app.data.size() == 0x17C000,
        "Shidaixinan S19 automatic address/length analysis mismatch");
  check(uds::shidaixinan_hjzj_crc32(driver.data) ==
                uds::kShidaixinanReferenceDriverCrc32 &&
            uds::shidaixinan_hjzj_crc32(app.data) ==
                uds::kShidaixinanReferenceAppCrc32,
        "Shidaixinan CRC32 differs from complete success trace");

  check(
      uds::shidaixinan_hjzj_request_download(
          driver.address,
          static_cast<std::uint32_t>(driver.data.size())) ==
          std::vector<std::uint8_t>(
              {0x34, 0x00, 0x44, 0x10, 0x20, 0x04, 0x00,
               0x00, 0x00, 0x04, 0x00}) &&
          uds::shidaixinan_hjzj_erase_memory(
              app.address,
              static_cast<std::uint32_t>(app.data.size())) ==
              std::vector<std::uint8_t>(
                  {0x31, 0x01, 0xFF, 0x00, 0x44,
                   0x00, 0x0C, 0x00, 0x00,
                   0x00, 0x17, 0xC0, 0x00}) &&
          uds::shidaixinan_hjzj_max_block_length(
              std::array<std::uint8_t, 4>{
                  0x74, 0x20, 0x08, 0x02}) == 0x802,
      "Shidaixinan RequestDownload/Erase/block-length contract mismatch");

  const auto wakeup = uds::shidaixinan_hjzj_wakeup_frame();
  const auto tester =
      uds::shidaixinan_hjzj_tester_present_frame();
  check(wakeup.id == 0x425 && !wakeup.extended && wakeup.fd &&
            wakeup.brs && wakeup.data ==
                std::vector<std::uint8_t>(8, 0x00) &&
            tester.id == 0x7DF && !tester.extended &&
            tester.fd && !tester.brs &&
            tester.data ==
                std::vector<std::uint8_t>(
                    {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0}) &&
            uds::kShidaixinanHjzjWakeupPeriod ==
                std::chrono::milliseconds(10) &&
            uds::kShidaixinanHjzjTesterPresentPeriod ==
                std::chrono::milliseconds(3000),
        "Shidaixinan periodic wake-up/TesterPresent baseline mismatch");

  check(std::filesystem::file_size(root / "dll" / "FMR.dll") ==
                353792 &&
            std::filesystem::file_size(
                root / "integrity" / "HJZJ_CRC32.dll") ==
                12288 &&
            std::filesystem::file_size(
                root / "CDD" /
                "FMR_II_V1.4.120_12Byte.cdd") ==
                1068776,
        "Shidaixinan DLL/CDD provenance resource mismatch");
  const auto workflow =
      uds::create_flash_workflow(L"shidaixinan_hjzj_fmr");
  check(workflow &&
            workflow->id() == L"shidaixinan_hjzj_fmr" &&
            workflow->report_title(profile).find("Shidaixinan") !=
                std::string::npos,
        "Shidaixinan workflow factory/report mapping mismatch");
}

void test_shidaixinan_arf232_project_profiles_and_resources() {
  struct ProjectCase {
    const wchar_t* profile_file;
    const wchar_t* profile_id;
    const wchar_t* project_name;
    const wchar_t* resource_directory;
    const wchar_t* pls_file;
  };
  constexpr std::array<ProjectCase, 3> projects{{
      {L"shidaixinan_tianwangxing_fmr.ini",
       L"shidaixinan_tianwangxing_fmr", L"天王星",
       L"shidaixinan_tianwangxing_fmr",
       L"ARF2.32CC3_SDTWXP_BV1.01_PLS_V2.0.00_CHF0355N_without_boot.s19"},
      {L"shidaixinan_muxing2_fmr.ini",
       L"shidaixinan_muxing2_fmr", L"木星2代",
       L"shidaixinan_muxing2_fmr",
       L"ARF2.32CC3_SDMX2P_BV1.01_PLS_V2.0.00_CHF0357N_without_boot.s19"},
      {L"shidaixinan_qingling_fmr.ini",
       L"shidaixinan_qingling_fmr", L"庆铃",
       L"shidaixinan_qingling_fmr",
       L"ARF2.32CC3_SLAQLP_BV1.01_PLS_V2.0.00_CHF0362N_without_boot.s19"},
  }};

  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  std::array<std::uint8_t, 32> reference_pls_hash{};
  bool have_reference_hash = false;
  for (const auto& project : projects) {
    const auto profile = uds::load_profile_ini(
        source / "profiles" / project.profile_file);
    check(profile.id == project.profile_id &&
              profile.flow == L"shidaixinan_hjzj_fmr" &&
              profile.vendor_name == L"时代新安" &&
              profile.project_name == project.project_name &&
              profile.device_name == L"FMR" &&
              !profile.placeholder && profile.can_fd &&
              !profile.power_control && !profile.extended_id &&
              profile.uds_fd && !profile.uds_brs &&
              profile.supports_ft_entry &&
              !profile.supports_cal_download &&
              profile.lock_diagnostic_ids &&
              profile.default_entry_mode == L"app" &&
              profile.tx_id == 0x7A4 && profile.rx_id == 0x7AC &&
              profile.functional_id == 0x7DF &&
              profile.ft_tx_id ==
                  uds::kShidaixinanHjzjFtFunctionalTxId &&
              profile.ft_rx_id ==
                  uds::kShidaixinanHjzjFtFunctionalRxId &&
              !profile.ft_extended_id && profile.ft_uds_fd &&
              !profile.ft_uds_brs && profile.ft_padding == 0x00 &&
              profile.channel == 2 &&
              profile.nominal_bitrate == 500000 &&
              profile.data_bitrate == 2000000 &&
              profile.padding == 0x00 && profile.isotp_st_min == 0 &&
              profile.security_level == 0x01 &&
              profile.security_variant.empty() &&
              profile.driver_start == 0 && profile.driver_length == 0 &&
              profile.app_start == 0 && profile.app_length == 0 &&
              profile.app_file.empty() &&
              profile.driver_file == std::filesystem::path(
                  L"resources\\shidaixinan_arf232_common\\Driver\\ARF2_32_ERadar_FlashDrv.s19") &&
              profile.security_dll == std::filesystem::path(
                  L"resources\\shidaixinan_arf232_common\\dll\\FMR.dll"),
          "Shidaixinan ARF2.32 project profile mismatch");

    const auto pls = uds::load_single_srecord_segment(
        source / "resources" / project.resource_directory /
        "Reference" / "PLS" / project.pls_file);
    check(pls.address == 0x000C0000 &&
              pls.data.size() == 0x17C000,
          "Shidaixinan project PLS layout mismatch");
    const auto pls_hash = uds::sha256(pls.data);
    if (!have_reference_hash) {
      reference_pls_hash = pls_hash;
      have_reference_hash = true;
    } else {
      check(pls_hash == reference_pls_hash,
            "Shidaixinan project PLS sources are no longer byte-identical");
    }
  }

  const auto catalog = uds::discover_flash_profiles(source / "profiles");
  check(catalog.errors.empty() && catalog.profiles.size() == 22,
        "Shidaixinan project profiles were not discovered cleanly");
  for (const auto& project : projects) {
    check(std::any_of(
              catalog.profiles.begin(), catalog.profiles.end(),
              [&project](const auto& record) {
                return record.profile.id == project.profile_id;
              }),
          "Shidaixinan project profile missing from catalog");
  }
}

void test_lp_arc_protocol_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile =
      uds::load_profile_ini(source / "profiles" / "lp_arc.ini");
  check(profile.id == L"lp_arc" && profile.flow == L"lp_arc" &&
            profile.vendor_name == L"零跑" &&
            profile.project_name == L"ARC" &&
            profile.name == L"ARC" &&
            profile.device_name == L"设备 0（0x772 / 0x77A）" &&
            !profile.placeholder && profile.can_fd &&
            !profile.power_control && !profile.extended_id &&
            !profile.uds_fd && !profile.uds_brs &&
            profile.supports_ft_entry &&
            !profile.supports_cal_download &&
            profile.lock_diagnostic_ids &&
            profile.default_entry_mode == L"app" &&
            profile.app_entry_label == L"APP" &&
            profile.ft_entry_label == L"FT" &&
            profile.tx_id == 0x772 && profile.rx_id == 0x77A &&
            profile.functional_id == 0x7DF &&
            profile.ft_tx_id == 0x701 && profile.ft_rx_id == 0x761 &&
            !profile.ft_extended_id && !profile.ft_uds_fd &&
            !profile.ft_uds_brs && profile.ft_padding == 0x55 &&
            profile.channel == 1 &&
            profile.nominal_bitrate == 500000 &&
            profile.padding == 0x55 &&
            profile.isotp_st_min == 0 &&
            profile.security_level == 0x11 &&
            profile.security_variant == L"lingpao" &&
            profile.driver_start == uds::kLpArcDriverAddress &&
            profile.driver_length == uds::kLpArcDriverLength &&
            profile.app_start == uds::kLpArcAppAddress &&
            profile.app_length == uds::kLpArcAppLength &&
            profile.app_verify_file.empty() &&
            profile.app_verify_label == L"Certificate" &&
            profile.targets.size() == 4 &&
            profile.targets[0].tx_id == 0x772 &&
            profile.targets[0].rx_id == 0x77A &&
            profile.targets[1].tx_id == 0x773 &&
            profile.targets[1].rx_id == 0x77B &&
            profile.targets[2].tx_id == 0x771 &&
            profile.targets[2].rx_id == 0x779 &&
            profile.targets[3].tx_id == 0x770 &&
            profile.targets[3].rx_id == 0x778 &&
            std::none_of(profile.targets.begin(), profile.targets.end(),
                         [](const auto& target) {
                           return target.pending_validation;
                         }),
        "ARC merged four-target packaged profile mismatch");

  check(uds::resolve_lp_arc_entry_mode(L"app") ==
                uds::LpArcEntryMode::app_to_app &&
            uds::resolve_lp_arc_entry_mode(L"ft") ==
                uds::LpArcEntryMode::pls_to_app,
        "LP-ARC APP/PLS entry mapping mismatch");
  auto configurable_profile = profile;
  configurable_profile.tx_id = 0x72E;
  configurable_profile.rx_id = 0x72F;
  const auto configurable_spec =
      uds::lp_arc_radar_spec(configurable_profile);
  check(configurable_spec.app_tx_id == 0x72E &&
            configurable_spec.app_rx_id == 0x72F &&
            configurable_spec.raw_boot_transition_tx_id == 0 &&
            configurable_spec.security.seed_subfunction == 0x11 &&
            configurable_spec.security.seed_length == 4 &&
            configurable_spec.security.key_length == 4 &&
            configurable_spec.allow_empty_certificate &&
            configurable_spec.skip_certificate_routines_when_empty &&
            configurable_spec.certificate_response_policy ==
                uds::CertificateResponsePolicy::require_positive &&
            configurable_spec.security.known_answers.empty() &&
            configurable_spec.security.self_test_description.empty(),
        "LP-ARC configurable APP endpoint/target-aware boot transition mismatch");

  const auto chuneng_profile = uds::load_profile_ini(
      source / "profiles" / "chuneng_331_left_rear.ini");
  const auto right_spec = uds::chuneng_arc331_radar_spec(chuneng_profile);
  auto left_profile = chuneng_profile;
  left_profile.tx_id = 0x72E;
  left_profile.rx_id = 0x72F;
  const auto left_spec = uds::chuneng_arc331_radar_spec(left_profile);
  check(chuneng_profile.flow == L"chuneng_arc331" &&
            chuneng_profile.targets.size() == 2 &&
            chuneng_profile.driver_file.filename() ==
                L"driver_712345678AB.cbf" &&
            chuneng_profile.app_file.filename() ==
                L"7052A5023002AB.cbf" &&
            chuneng_profile.driver_verify_file.empty() &&
            chuneng_profile.app_verify_file.empty() &&
            right_spec.app_tx_id == 0x72C && right_spec.app_rx_id == 0x72D &&
            left_spec.app_tx_id == 0x72E && left_spec.app_rx_id == 0x72F &&
            !right_spec.send_raw_boot_transition &&
            !left_spec.send_raw_boot_transition &&
            right_spec.raw_boot_transition_tx_id == 0 &&
            left_spec.raw_boot_transition_tx_id == 0 &&
            right_spec.periodic_wakeup_id == 0x520U &&
            left_spec.periodic_wakeup_id == 0x520U &&
            right_spec.periodic_wakeup_period ==
                std::chrono::milliseconds(10) &&
            right_spec.security.seed_subfunction == 0x11 &&
            right_spec.security.seed_length == 16 &&
            right_spec.security.key_length == 16 &&
            right_spec.security.known_answers.size() == 1 &&
            left_spec.security.seed_length == 16 &&
            left_spec.security.key_length == 16 &&
            chuneng_profile.security_variant == L"chuneng" &&
            chuneng_profile.security_dll.filename() ==
                L"ChuNeng_D7_SeednKey_V1.0.dll",
        "ChuNeng ARC331 target or no-transition contract mismatch");
  const auto chuneng_workflow =
      uds::create_flash_workflow(L"chuneng_arc331");
  check(chuneng_workflow &&
            dynamic_cast<uds::ChunengArc331Workflow*>(
                chuneng_workflow.get()) != nullptr &&
            chuneng_workflow->id() == L"chuneng_arc331" &&
            chuneng_workflow->report_title(chuneng_profile) ==
                "ChuNeng ARC331 Radar Download Report",
        "ChuNeng ARC331 workflow registration mismatch");
  const auto chuneng_root =
      source / "resources" / "chuneng_d7_arc331_zip";
  const auto driver_cbf = uds::load_chuneng_cbf(
      chuneng_root / "CBF" / "Driver" / "driver_712345678AB.cbf");
  const auto app_cbf = uds::load_chuneng_cbf(
      chuneng_root / "CBF" / "APP" / "7052A5023002AB.cbf");
  const auto s19_root = chuneng_root / "S19";
  const auto driver_s19 = uds::load_srecord_window(
      s19_root / "Driver.s19", driver_cbf.main.address,
      driver_cbf.main.data.size());
  const auto app_s19 = uds::load_srecord_window(
      s19_root / "APP.s19", app_cbf.main.address, app_cbf.main.data.size());
  const auto driver_s19_abt =
      uds::load_asc_hex(s19_root / "Driver_ABT.asc", 0x2C, 0x2C);
  const auto app_s19_abt =
      uds::load_asc_hex(s19_root / "APP_ABT.asc", 0x2C, 0x2C);
  const auto driver_s19_signature =
      uds::load_asc_hex(s19_root / "Driver_Ver.asc", 256, 256);
  const auto app_s19_signature =
      uds::load_asc_hex(s19_root / "APP_Ver.asc", 256, 256);
  const auto driver_s19_metadata =
      uds::validate_chuneng_331_abt(driver_s19_abt, driver_s19);
  const auto app_s19_metadata =
      uds::validate_chuneng_331_abt(app_s19_abt, app_s19);
  constexpr std::string_view kApprovedDriverMarker{
      "FAKE_CN2944_FLASH_DRIVER_RAW_0x4000"};
  check(std::filesystem::file_size(
            chuneng_root / "dll" / "ChuNeng_D7_SeednKey_V1.0.dll") ==
                939520 &&
            std::filesystem::file_size(
                chuneng_root / "Reference" /
                "LeapMotor_UDS27_SeedKey_HexDumpVar.cdd") == 1047726 &&
            std::filesystem::file_size(
                chuneng_root / "CBF" / "Driver" /
                    "driver_712345678AB.cbf") ==
                17322 &&
            std::filesystem::file_size(
                chuneng_root / "CBF" / "APP" /
                    "7052A5023002AB.cbf") ==
                1573818 &&
            driver_cbf.software_type == "SBL" &&
            driver_cbf.main.address == 0x10280000U &&
            driver_cbf.main.data.size() == 0x4000U &&
            driver_cbf.abt.data.size() == 0x2CU &&
            driver_cbf.device_signature.size() == 256U &&
            driver_cbf.main.data.size() >= kApprovedDriverMarker.size() &&
            std::equal(kApprovedDriverMarker.begin(),
                       kApprovedDriverMarker.end(),
                       driver_cbf.main.data.begin()) &&
            app_cbf.software_type == "APP" &&
            app_cbf.main.address == 0x000C0000U &&
            app_cbf.main.data.size() == 0x180000U &&
            app_cbf.abt.data.size() == 0x2CU &&
            app_cbf.device_signature.size() == 256U &&
            driver_s19 == driver_cbf.main.data &&
            app_s19 == app_cbf.main.data &&
            driver_s19_abt == driver_cbf.abt.data &&
            app_s19_abt == app_cbf.abt.data &&
            driver_s19_signature == driver_cbf.device_signature &&
            app_s19_signature == app_cbf.device_signature &&
            driver_s19_metadata.source_address == 0x10280000U &&
            app_s19_metadata.source_address == 0x000C0000U,
        "ChuNeng ARC331 paired CBF main/ABT/signature or SeedKey resources mismatch");
  uds::FlashJob s19_preflight_job;
  s19_preflight_job.profile = chuneng_profile;
  s19_preflight_job.executable_directory = source;
  s19_preflight_job.driver_file =
      L"resources/chuneng_d7_arc331_zip/S19/Driver.s19";
  s19_preflight_job.app_file =
      L"resources/chuneng_d7_arc331_zip/S19/APP.s19";
  s19_preflight_job.driver_verify_file =
      L"resources/chuneng_d7_arc331_zip/S19/Driver_Ver.asc";
  s19_preflight_job.app_verify_file =
      L"resources/chuneng_d7_arc331_zip/S19/APP_Ver.asc";
  s19_preflight_job.can_bus_provider.reset();
  std::vector<std::string> s19_preflight_reports;
  uds::FlashWorkflowCallbacks s19_preflight_callbacks;
  s19_preflight_callbacks.report =
      [&s19_preflight_reports](std::string step, std::string verdict,
                               std::string detail) {
        s19_preflight_reports.push_back(step + ":" + verdict + ":" + detail);
      };
  std::stop_source stopped_preflight;
  stopped_preflight.request_stop();
  bool stopped_before_can = false;
  try {
    uds::ChunengArc331Workflow s19_workflow;
    s19_workflow.run(s19_preflight_job, s19_preflight_callbacks,
                     stopped_preflight.get_token());
  } catch (const std::runtime_error& error) {
    stopped_before_can =
        std::string(error.what()).find("cancelled") != std::string::npos;
  }
  check(stopped_before_can &&
            std::any_of(s19_preflight_reports.cbegin(),
                        s19_preflight_reports.cend(),
                        [](const std::string& line) {
                          return line.find("Preflight:PASS:Files validated: "
                                           "Driver=0x4000+ABT") !=
                                 std::string::npos;
                        }),
        "ChuNeng packaged S19 pair did not pass the real workflow preflight "
        "before CAN access");
  auto mixed_preflight_job = s19_preflight_job;
  mixed_preflight_job.driver_file =
      L"resources/chuneng_d7_arc331_zip/CBF/Driver/driver_712345678AB.cbf";
  mixed_preflight_job.driver_verify_file.clear();
  mixed_preflight_job.app_verify_file.clear();
  std::vector<std::string> mixed_preflight_reports;
  std::vector<std::string> mixed_preflight_logs;
  uds::FlashWorkflowCallbacks mixed_preflight_callbacks;
  mixed_preflight_callbacks.report =
      [&mixed_preflight_reports](std::string step, std::string verdict,
                                 std::string detail) {
        mixed_preflight_reports.push_back(step + ":" + verdict + ":" +
                                          detail);
      };
  mixed_preflight_callbacks.log =
      [&mixed_preflight_logs](const std::string& line) {
        mixed_preflight_logs.push_back(line);
      };
  bool mixed_stopped_before_can = false;
  try {
    uds::ChunengArc331Workflow mixed_workflow;
    mixed_workflow.run(mixed_preflight_job, mixed_preflight_callbacks,
                       stopped_preflight.get_token());
  } catch (const std::runtime_error& error) {
    mixed_stopped_before_can =
        std::string(error.what()).find("cancelled") != std::string::npos;
  }
  check(mixed_stopped_before_can &&
            std::any_of(mixed_preflight_reports.cbegin(),
                        mixed_preflight_reports.cend(),
                        [](const std::string& line) {
                          return line.find("Preflight:PASS:Files validated: "
                                           "Driver=0x4000+ABT") !=
                                 std::string::npos;
                        }) &&
            std::any_of(mixed_preflight_logs.cbegin(),
                        mixed_preflight_logs.cend(),
                        [](const std::string& line) {
                          return line.find("without local S-record binding "
                                           "validation") !=
                                 std::string::npos;
                        }),
        "ChuNeng Driver CBF + APP S19 did not pass workflow preflight "
        "without a manually selected APP ASC before CAN access");
  bool rejected = false;
  try {
    static_cast<void>(uds::resolve_lp_arc_entry_mode(L"auto"));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "LP-ARC accepted an unapproved automatic entry mode");

  check(
      uds::lp_arc_request_download(0x000C0000, 0x00180000) ==
              std::vector<std::uint8_t>(
                  {0x34, 0x00, 0x44, 0x00, 0x0C, 0x00, 0x00,
                   0x00, 0x18, 0x00, 0x00}) &&
          uds::lp_arc_erase_memory(0x000C0000, 0x00180000) ==
              std::vector<std::uint8_t>(
                  {0x31, 0x01, 0xFF, 0x00, 0x44,
                   0x00, 0x0C, 0x00, 0x00,
                   0x00, 0x18, 0x00, 0x00}) &&
          uds::lp_arc_driver_crc_request(0x8509E388) ==
              std::vector<std::uint8_t>(
                  {0x31, 0x01, 0x02, 0x02,
                   0x85, 0x09, 0xE3, 0x88}) &&
          uds::lp_arc_max_block_length(
              std::array<std::uint8_t, 4>{
                  0x74, 0x20, 0x08, 0x02}) == 0x802,
      "LP-ARC RequestDownload/Erase/CRC/block contract mismatch");
  std::tm captured_date{};
  captured_date.tm_year = 126;
  captured_date.tm_mon = 6;
  captured_date.tm_mday = 31;
  check(uds::lp_arc_programming_date(captured_date) ==
            std::vector<std::uint8_t>({0x20, 0x26, 0x07, 0x31}),
        "LP-ARC 2E F199 BCD date encoding mismatch");

  const auto root = source / "resources" / "lp_arc";
  const auto driver = uds::load_single_srecord_segment(
      root / "Driver" / "FlashDriver.srec");
  const auto app = uds::load_single_srecord_segment(
      root / "APP" /
      "ARC2.36BC3_LEE35A_B1.01.00_APP_20260708V1_CHF0330N_Es2.s19");
  const auto certificate = uds::load_asc_hex(
      root / "Verification" /
          "LP-BSD080-BA_V9.99.99_R_RL_20250506.asc",
      uds::kLpArcCertificateLength, uds::kLpArcCertificateLength);
  check(driver.address == uds::kLpArcDriverAddress &&
            driver.data.size() == uds::kLpArcDriverLength &&
            app.address == uds::kLpArcAppAddress &&
            app.data.size() == uds::kLpArcAppLength &&
            uds::lp_arc_crc32(driver.data) ==
                uds::kLpArcReferenceDriverCrc32 &&
            uds::lp_arc_crc32(app.data) ==
                uds::kLpArcReferenceAppCrc32 &&
            certificate.size() == uds::kLpArcCertificateLength,
        "LP-ARC resources differ from the 2026-07-31 success baseline");
  check(std::filesystem::file_size(
            root / "dll" /
            "66272f124ced1_lingpao_SeednKey_cdd.dll") == 777216 &&
            std::filesystem::file_size(
                root / "Reference" / "Flash20230727.can") == 165795,
        "LP-ARC SeedKey DLL/CAPL provenance resources mismatch");

  const auto workflow = uds::create_flash_workflow(L"lp_arc");
  check(workflow && workflow->id() == L"lp_arc" &&
            workflow->report_title(profile).find("LP-ARC") !=
                std::string::npos,
        "LP-ARC workflow factory/report mapping mismatch");
}

void test_lp_arf_protocol_and_resources() {
  const auto source = std::filesystem::path(UDS_SOURCE_DIR);
  const auto profile =
      uds::load_profile_ini(source / "profiles" / "lp_arf.ini");
  check(profile.id == L"lp_arf" && profile.flow == L"lp_arf" &&
            profile.vendor_name == L"零跑" &&
            profile.project_name == L"ARF" &&
            profile.device_name == L"ARF 雷达" &&
            !profile.placeholder && profile.can_fd &&
            !profile.power_control && !profile.extended_id &&
            !profile.uds_fd && !profile.uds_brs &&
            profile.supports_ft_entry &&
            !profile.supports_cal_download &&
            profile.supports_app_tmp_package &&
            profile.lock_diagnostic_ids &&
            profile.default_entry_mode == L"app" &&
            profile.app_entry_label == L"APP" &&
            profile.ft_entry_label == L"FT" &&
            profile.tx_id == 0x751 && profile.rx_id == 0x759 &&
            profile.functional_id == 0x7DF &&
            profile.ft_tx_id == 0x701 && profile.ft_rx_id == 0x761 &&
            !profile.ft_extended_id && !profile.ft_uds_fd &&
            !profile.ft_uds_brs && profile.ft_padding == 0x55 &&
            profile.channel == 1 &&
            profile.nominal_bitrate == 500000 &&
            profile.data_bitrate == 2000000 &&
            profile.padding == 0x55 && profile.isotp_st_min == 0 &&
            profile.security_level == 0x11 &&
            profile.security_variant == L"lingpao" &&
            profile.driver_length == 0 && profile.driver_file.empty() &&
            profile.app_start == uds::kLpArfAppAddress &&
            profile.app_length == uds::kLpArfAppLength &&
            profile.app_file.filename() ==
                L"LP-MRS050-BA_V2.10.16_R_20240802.tmp" &&
            profile.app_verify_file.empty() &&
            profile.app_verify_label == L"Certificate",
        "LP-ARF packaged profile mismatch");

  check(uds::resolve_lp_arf_entry_mode(L"app") ==
                uds::LpArfEntryMode::app_to_app &&
            uds::resolve_lp_arf_entry_mode(L"ft") ==
                uds::LpArfEntryMode::pls_to_app,
        "LP-ARF APP/FT entry mapping mismatch");
  bool rejected = false;
  try {
    static_cast<void>(uds::resolve_lp_arf_entry_mode(L"auto"));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "LP-ARF accepted an unapproved automatic entry mode");

  const auto spec = uds::lp_arf_radar_spec();
  check(spec.app_tx_id == 0x751 && spec.app_rx_id == 0x759 &&
            spec.pls_tx_id == 0x701 && spec.pls_rx_id == 0x761 &&
            spec.functional_id == 0x7DF && !spec.driver_address &&
            !spec.driver_length &&
            spec.pls_programming_final_on_app &&
            !spec.send_raw_boot_transition &&
            spec.allow_empty_certificate &&
            spec.skip_certificate_routines_when_empty &&
            spec.certificate_response_policy ==
                uds::CertificateResponsePolicy::require_positive &&
            spec.app_address == uds::kLpArfAppAddress &&
            spec.app_length == uds::kLpArfAppLength,
        "LP-ARF flow spec lost its APP-final transition route or imported an ARC-only phase");

  const auto custom_id_spec = uds::lp_arf_radar_spec(0x701, 0x761);
  check(custom_id_spec.app_tx_id == 0x701 &&
            custom_id_spec.app_rx_id == 0x761,
        "LP-ARF flow spec ignored configured APP diagnostic IDs");

  const auto root = source / "resources" / "lp_arf";
  const auto app = uds::load_single_srecord_segment(
      root / "APP" /
      "ARF6.31V1.0_PF4T4R_B1.00.01_APP_V1.00.04_CHF0383N_without_boot.s19");
  const auto certificate = uds::load_asc_hex(
      root / "Verification" / "ARF6.31 V1.00.04.asc",
      uds::kLpArfCertificateLength, uds::kLpArfCertificateLength);
  const auto b01_certificate = uds::load_asc_hex(
      root / "Verification" / "B01_ARF2.31_PASS0011_certificate.asc",
      uds::kLpArfCertificateLength, uds::kLpArfCertificateLength);
  constexpr std::array<std::uint8_t, 32> kB01AppSha256{
      0xF8, 0x58, 0x45, 0xD8, 0x0C, 0x32, 0x47, 0xC4,
      0xF3, 0xA5, 0x89, 0xF1, 0x87, 0xF3, 0xC1, 0x4C,
      0x28, 0x48, 0xF2, 0xFE, 0xF4, 0x14, 0x06, 0x0B,
      0x3A, 0xB4, 0xCD, 0x44, 0x22, 0x46, 0x68, 0x58};
  const auto app_hash = uds::sha256(app.data);
  check(app.address == uds::kLpArfAppAddress &&
            app.data.size() == uds::kLpArfAppLength &&
            certificate.size() == uds::kLpArfCertificateLength &&
            std::equal(app_hash.begin(), app_hash.end(),
                       certificate.begin()) &&
            b01_certificate.size() == uds::kLpArfCertificateLength &&
            std::equal(kB01AppSha256.begin(), kB01AppSha256.end(),
                       b01_certificate.begin()),
        "LP-ARF APP layout, identity or certificate binding mismatch");
  check(std::filesystem::file_size(
            root / "dll" /
            "66272f124ced1_lingpao_SeednKey_cdd.dll") == 777216 &&
            std::filesystem::file_size(
                root / "Reference" / "Flash20260603_withBD.can") ==
                172803 &&
            std::filesystem::file_size(
                root / "Reference" / "RaderID.can") == 999 &&
            std::filesystem::file_size(
                root / "Reference" / "B01" /
                "Flash_report0011.xml") == 1672366 &&
            std::filesystem::file_size(
                root / "Reference" / "B01" /
                "lingpaoB01.110.cdd") == 1119154 &&
            std::filesystem::exists(root / "SOURCE_MANIFEST.sha256"),
        "LP-ARF SeedKey/CAPL provenance resources mismatch");
  check(!std::filesystem::exists(
            source / "profiles" / "lp_arf_placeholder.ini"),
        "retired LP-ARF placeholder profile still exists");
  check(!std::filesystem::exists(
            source / "profiles" / "lp_arf_231_b01.ini"),
        "retired independent B01 profile still exists");

  const auto workflow = uds::create_flash_workflow(L"lp_arf");
  check(workflow && workflow->id() == L"lp_arf" &&
            workflow->report_title(profile).find("Leapmotor ARF") !=
                std::string::npos,
        "LP-ARF workflow factory/report mapping mismatch");

  auto custom_endpoint_profile = profile;
  custom_endpoint_profile.tx_id = 0x701;
  custom_endpoint_profile.rx_id = 0x761;
  bool custom_endpoint_accepted = true;
  try {
    uds::validate_lp_arf_profile_contract(custom_endpoint_profile);
  } catch (const std::runtime_error&) {
    custom_endpoint_accepted = false;
  }
  check(custom_endpoint_accepted,
        "LP-ARF custom APP 701/761 endpoint was rejected");

  custom_endpoint_profile.tx_id = 0;
  bool zero_endpoint_rejected = false;
  try {
    uds::validate_lp_arf_profile_contract(custom_endpoint_profile);
  } catch (const std::runtime_error& error) {
    zero_endpoint_rejected =
        std::string(error.what()).find("non-zero configurable APP IDs") !=
        std::string::npos;
  }
  check(zero_endpoint_rejected,
        "LP-ARF accepted a zero APP diagnostic endpoint");
}

} // namespace

int main() {
  try {
    const auto run = [](const char* name, auto test) {
      std::cout << "RUN " << name << std::endl;
      test();
    };
    run("hex", test_hex);
    run("sha256", test_sha256);
    run("html_report_navigation_and_transfer_aggregation",
        test_html_report_navigation_and_transfer_aggregation);
    run("asc_trace_can_bus", test_asc_trace_can_bus);
    run("bus_monitor_trace_session", test_bus_monitor_trace_session);
    run("can_id_filter", test_can_id_filter);
    run("uds_single_frame", test_uds_single_frame);
    run("isotp_drains_stale_receive_queue_before_request",
        test_isotp_drains_stale_receive_queue_before_request);
    run("uds_response_pending_and_nrc", test_uds_response_pending_and_nrc);
    run("chuneng_ft_pending_switches_to_selected_app_response",
        test_chuneng_ft_pending_switches_to_selected_app_response);
    run("uds_nrc_diagnostics", test_uds_nrc_diagnostics);
    run("uds_wait_can_be_cancelled", test_uds_wait_can_be_cancelled);
    run("isotp_multiframe_receive", test_isotp_multiframe_receive);
    run("isotp_reorders_adjacent_consecutive_frames",
        test_isotp_reorders_adjacent_consecutive_frames);
    run("uds_observe_consumes_non_gating_responses",
        test_uds_observe_consumes_non_gating_responses);
    run("isotp_multiframe_send", test_isotp_multiframe_send);
    run("isotp_mixed_can_fd_adaptation", test_isotp_mixed_can_fd_adaptation);
    run("isotp_can_fd_64_byte_frames",
        test_isotp_can_fd_64_byte_frames);
    run("flash_data", test_flash_data);
    run("chuneng_cbf_software_type_compatibility",
        test_chuneng_cbf_software_type_compatibility);
    run("profile_round_trip", test_profile_round_trip);
    run("profile_discovery", test_profile_discovery);
    run("workflow_registry", test_workflow_registry);
    run("baic_radar_protocol", test_baic_radar_protocol);
    run("chuneng_331_updated_protocol_contract",
        test_chuneng_331_updated_protocol_contract);
    run("xizhong_rsmr_profile_and_resources", test_xizhong_rsmr_profile_and_resources);
    run("xizhong_lsmr_profile_and_resources", test_xizhong_lsmr_profile_and_resources);
    run("xizhong_rsmr_protocol_baseline", test_xizhong_rsmr_protocol_baseline);
    run("chery_ars133_protocol_and_resources", test_chery_ars133_protocol_and_resources);
    run("chery_kp31_protocol_and_resources", test_chery_kp31_protocol_and_resources);
    run("chery_ars131_project_contracts",
        test_chery_ars131_project_contracts);
    run("longma_ars131_protocol_and_resources", test_longma_ars131_protocol_and_resources);
    run("c857_project_profiles_and_resources",
        test_c857_project_profiles_and_resources);
    run("shidaixinan_hjzj_fmr_mode_sequences",
        test_shidaixinan_hjzj_fmr_mode_sequences);
    run("shidaixinan_hjzj_fmr_protocol_and_resources",
        test_shidaixinan_hjzj_fmr_protocol_and_resources);
    run("shidaixinan_arf232_project_profiles_and_resources",
        test_shidaixinan_arf232_project_profiles_and_resources);
    run("lp_arc_protocol_and_resources",
        test_lp_arc_protocol_and_resources);
    run("lp_arf_protocol_and_resources",
        test_lp_arf_protocol_and_resources);
    run("lp_arf_tmp_packages", test_lp_arf_tmp_packages);
    std::cout << "core_tests PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "core_tests FAIL: " << error.what() << '\n';
    return 1;
  }
}
