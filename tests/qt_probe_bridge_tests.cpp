#include "ui/qt/controller_bridge.hpp"

#include "core/can_bus.hpp"
#include "flash/flash_workflow.hpp"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct BusCapture {
  std::atomic_bool opened{};
  std::atomic_int sent_count{};
  std::atomic<std::uint32_t> sent_id{};
  std::atomic<std::uint8_t> sent_service{};
  std::atomic<std::uint8_t> sent_subfunction{};
};

struct FlashCapture {
  std::atomic_bool ran{};
  uds::FlashJob job;
};

class FakeFlashWorkflow final : public uds::FlashWorkflow {
public:
  explicit FakeFlashWorkflow(std::shared_ptr<FlashCapture> capture)
      : capture_(std::move(capture)) {}

  std::wstring_view id() const noexcept override { return L"qt_probe_test"; }
  std::string report_title(const uds::FlashProfile&) const override {
    return "Qt Controller Bridge Test Report";
  }
  void run(const uds::FlashJob& job,
           const uds::FlashWorkflowCallbacks& callbacks,
           std::stop_token) override {
    capture_->job = job;
    capture_->ran = true;
    if (callbacks.log) callbacks.log("fake flash log");
    if (callbacks.progress) callbacks.progress(88, "fake flash progress");
    if (callbacks.report) {
      callbacks.report("FakeFlash", "PASS", "bridge test");
    }
  }

private:
  std::shared_ptr<FlashCapture> capture_;
};

class FakeProbeBus final : public uds::ICanBus {
public:
  FakeProbeBus(std::shared_ptr<BusCapture> capture, std::uint32_t response_id)
      : capture_(std::move(capture)), response_id_(response_id) {}

  void open() override {
    opened_ = true;
    capture_->opened = true;
  }
  void close() noexcept override { opened_ = false; }
  bool is_open() const noexcept override { return opened_; }
  void send(const uds::CanFrame& frame) override {
    capture_->sent_id = frame.id;
    capture_->sent_service = frame.data.size() > 1 ? frame.data[1] : 0;
    capture_->sent_subfunction = frame.data.size() > 2 ? frame.data[2] : 0;
    ++capture_->sent_count;
  }
  std::optional<uds::CanFrame> receive(
      std::chrono::milliseconds timeout) override {
    if (!response_sent_) {
      response_sent_ = true;
      return uds::CanFrame{
          response_id_,
          {0x02, 0x50, capture_->sent_subfunction.load(), 0x00, 0x00, 0x00,
           0x00, 0x00},
          false, false, false};
    }
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(5)));
    return std::nullopt;
  }

private:
  std::shared_ptr<BusCapture> capture_;
  std::uint32_t response_id_{};
  bool response_sent_{};
  bool opened_{};
};

uds::FlashProfileRecord makeProfile() {
  uds::FlashProfile profile;
  profile.id = L"qt_probe_test";
  profile.flow = L"qt_probe_test";
  profile.name = L"Qt Probe Test ECU";
  profile.power_control = false;
  profile.lock_diagnostic_ids = true;
  profile.can_fd = false;
  profile.supports_ft_entry = true;
  profile.default_entry_mode = L"app";
  profile.channel = 2;
  profile.tx_id = 0x772;
  profile.rx_id = 0x77A;
  profile.functional_id = 0x7DF;
  profile.ft_tx_id = 0x701;
  profile.ft_rx_id = 0x761;
  profile.ft_padding = 0x55;
  profile.nominal_bitrate = 500000;
  profile.data_bitrate = 2000000;
  profile.padding = 0x00;
  return {{}, std::move(profile)};
}

uds::FlashProfileRecord makeLongmaProfile() {
  auto record = makeProfile();
  record.profile.id = L"longma_ars1_31";
  record.profile.flow = L"longma_ars1_31";
  record.profile.name = L"长马 1.31";
  record.profile.vendor_name = L"长马";
  record.profile.project_name = L"ARS1.31";
  record.profile.device_name = L"主雷达";
  record.profile.supports_ft_entry = true;
  record.profile.lock_diagnostic_ids = true;
  record.profile.ft_tx_id = 0x714;
  record.profile.ft_rx_id = 0x71C;
  record.profile.ft_padding = 0x00;
  record.profile.tx_id = 0x744;
  record.profile.rx_id = 0x74C;
  record.profile.targets = {
      {L"main", L"主雷达", 0x744, 0x74C, false},
      {L"secondary", L"从雷达（待验证）", 0x760, 0x768, true}};
  for (auto& target : record.profile.targets) {
    target.ft_tx_id = 0x714;
    target.ft_rx_id = 0x71C;
  }
  return record;
}

uds::FlashProfileRecord makeC857Profile(
    const std::wstring& id, const std::wstring& device_name,
    const std::wstring& resource_directory) {
  auto record = makeProfile();
  record.profile.id = id;
  record.profile.flow = id;
  record.profile.name = L"长安 " + device_name + L" ARS1.31";
  record.profile.vendor_name = L"长安";
  record.profile.project_name = device_name;
  record.profile.device_name = device_name;
  record.profile.supports_ft_entry = true;
  record.profile.ft_tx_id = 0x715;
  record.profile.ft_rx_id = 0x71D;
  record.profile.ft_padding = 0x00;
  record.profile.tx_id = 0x744;
  record.profile.rx_id = 0x74C;
  record.profile.driver_file =
      L"resources/" + resource_directory +
      L"/Driver/ARS1.31C3A_C857_FlashDriver.s19";
  record.profile.targets = {
      {L"main", L"主雷达（前雷达 ICRF，待验证）", 0x744, 0x74C, true,
       0x7587, {},
       L"resources/" + resource_directory +
           L"/APP/ICRF/C857AF_CHF0301N.s19",
       {}, {}, {}, {},
       L"resources/" + resource_directory + L"/dll/SeedKey_Main.dll",
       0x715, 0x71D},
      {L"secondary", L"从雷达（后雷达 ICRR）", 0x760, 0x768, false,
       0xB5E2, {},
       L"resources/" + resource_directory +
           L"/APP/ICRR/C857AR_CHF0303N.s19",
       {}, {}, {}, {},
       L"resources/" + resource_directory + L"/dll/SeedKey_Slave.dll",
       0x714, 0x71C}};
  return record;
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    QCoreApplication application(argc, argv);
    auto capture = std::make_shared<BusCapture>();
    auto flash_capture = std::make_shared<FlashCapture>();
    uds::app::ProbeService service(
        [capture](const uds::app::ProbeRequest& request) {
          return std::make_unique<FakeProbeBus>(capture, request.rx_id);
        });
    std::vector<uds::FlashProfileRecord> profiles;
    profiles.push_back(makeProfile());
    profiles.push_back(makeLongmaProfile());
    profiles.push_back(
        makeC857Profile(L"changan_c857", L"C857", L"changan_c857"));
    profiles.push_back(
        makeC857Profile(L"lingyao_b216", L"B216", L"lingyao_b216"));
    uds::ui::qt::ControllerBridge bridge(
        std::move(profiles), std::move(service),
        [flash_capture](std::wstring_view flow_id) {
          check(flow_id == L"qt_probe_test" ||
                    flow_id == L"longma_ars1_31" ||
                    flow_id == L"changan_c857" ||
                    flow_id == L"lingyao_b216",
                "Qt bridge passed the wrong flash flow id");
          return std::make_unique<FakeFlashWorkflow>(flash_capture);
        });
    const auto& profile_options = bridge.profileOptions();
    check(profile_options.size() == 4 &&
              profile_options[1].supports_ft_entry &&
              profile_options[1].target_options.size() == 2 &&
              profile_options[1].target_options[0].tx_id == 0x744 &&
              profile_options[1].target_options[0].rx_id == 0x74C &&
              profile_options[1].target_options[1].tx_id == 0x760 &&
              profile_options[1].target_options[1].rx_id == 0x768,
          "Qt bridge did not publish the Longma APP/FT target endpoint mapping");
    check(profile_options[2].supports_ft_entry &&
              profile_options[2].vendor_name == QStringLiteral("长安") &&
              profile_options[2].project_name == QStringLiteral("C857") &&
              profile_options[2].device_name == QStringLiteral("C857"),
          "Qt bridge did not publish the C857 FT capability");
    check(profile_options[2].target_options.size() == 2 &&
              profile_options[2].target_options[0].app_path.endsWith(
                  QStringLiteral("C857AF_CHF0301N.s19")) &&
              profile_options[2].target_options[0].seed_key_dll_path.endsWith(
                  QStringLiteral("SeedKey_Main.dll")) &&
              profile_options[2].target_options[1].tx_id == 0x760 &&
              profile_options[2].target_options[1].rx_id == 0x768 &&
              profile_options[2].target_options[1].app_path.endsWith(
                  QStringLiteral("C857AR_CHF0303N.s19")) &&
              profile_options[2].target_options[1]
                  .seed_key_dll_path.endsWith(
                      QStringLiteral("SeedKey_Slave.dll")),
          "Qt bridge did not publish the C857 target resource mapping");
    check(profile_options[3].profile_id == QStringLiteral("lingyao_b216") &&
              profile_options[3].vendor_name == QStringLiteral("长安") &&
              profile_options[3].project_name == QStringLiteral("B216") &&
              profile_options[3].device_name == QStringLiteral("B216") &&
              profile_options[3].driver_path.contains(
                  QStringLiteral("lingyao_b216")) &&
              profile_options[2].driver_path.contains(
                  QStringLiteral("changan_c857")),
          "Qt bridge did not keep C857 and B216 resources isolated");

    bool received_log{};
    bool received_progress{};
    bool running_started{};
    bool running_finished{};
    bool finished{};
    bool succeeded{};
    bool cancelled{};
    bool timed_out{};
    bool callbacks_on_ui_thread{true};
    bool flash_running_started{};
    bool flash_running_finished{};
    bool flash_finished{};
    bool flash_succeeded{};
    bool flash_cancelled{};
    QString flash_report;
    QString probe_message;
    QStringList emitted_logs;

    QObject::connect(
        &bridge, &uds::ui::qt::ControllerBridge::logMessage, &application,
        [&](const QString& message) {
          callbacks_on_ui_thread &=
              QThread::currentThread() == application.thread();
          received_log = true;
          emitted_logs.push_back(message);
        });
    QObject::connect(
        &bridge, &uds::ui::qt::ControllerBridge::progressChanged,
        &application, [&](int percent, const QString&) {
          callbacks_on_ui_thread &=
              QThread::currentThread() == application.thread();
          received_progress |= percent == 100;
        });
    QObject::connect(
        &bridge, &uds::ui::qt::ControllerBridge::probeRunningChanged,
        &application, [&](bool running) {
          callbacks_on_ui_thread &=
              QThread::currentThread() == application.thread();
          running_started |= running;
          running_finished |= !running;
        });
    QObject::connect(
        &bridge, &uds::ui::qt::ControllerBridge::probeFinished, &application,
        [&](bool success, bool was_cancelled, const QString& message) {
          callbacks_on_ui_thread &=
              QThread::currentThread() == application.thread();
          finished = true;
          succeeded = success;
          cancelled = was_cancelled;
          probe_message = message;
          application.quit();
        });

    finished = false;
    succeeded = false;
    cancelled = false;
    QObject::connect(
        &bridge, &uds::ui::qt::ControllerBridge::flashRunningChanged,
        &application, [&](bool running) {
          callbacks_on_ui_thread &=
              QThread::currentThread() == application.thread();
          flash_running_started |= running;
          flash_running_finished |= !running;
        });
    QObject::connect(
        &bridge, &uds::ui::qt::ControllerBridge::flashFinished, &application,
        [&](bool success, bool was_cancelled, const QString&,
            const QString& report_path) {
          callbacks_on_ui_thread &=
              QThread::currentThread() == application.thread();
          flash_finished = true;
          flash_succeeded = success;
          flash_cancelled = was_cancelled;
          flash_report = report_path;
          application.quit();
        });

    QTimer::singleShot(0, &bridge,
                       [&bridge] {
                         bridge.startProbe(0, {}, QStringLiteral("app"), 2,
                                           0x702, 0x762);
                       });
    QTimer::singleShot(3000, &application, [&] {
      timed_out = true;
      application.quit();
    });
    application.exec();

    check(!timed_out, "Qt probe bridge test timed out");
    if (!(finished && succeeded && !cancelled)) {
      std::cerr << "PROBE_MESSAGE=" << probe_message.toStdString() << '\n';
    }
    check(finished && succeeded && !cancelled,
          "Qt probe bridge did not report successful completion");
    check(received_log && received_progress,
          "Qt probe bridge did not forward log/progress signals");
    check(running_started && running_finished,
          "Qt probe bridge did not report running state transitions");
    check(callbacks_on_ui_thread,
          "Qt probe bridge delivered a callback outside the UI thread");
    check(capture->opened && capture->sent_count == 1 &&
              capture->sent_id == 0x772 && capture->sent_service == 0x10,
          "Qt bridge did not enforce a locked profile diagnostic endpoint");

    timed_out = false;
    QTimer::singleShot(0, &bridge, [&bridge] {
      bridge.startFlash(0, {}, QStringLiteral("ft"), 1, 2, 0x772, 0x77A,
                        QStringLiteral("driver.s19"),
                        QStringLiteral("app.s19"), {}, {}, {}, {},
                        QStringLiteral("security.dll"));
    });
    QTimer::singleShot(3000, &application, [&] {
      timed_out = true;
      application.quit();
    });
    application.exec();

    check(!timed_out, "Qt flash bridge test timed out");
    check(flash_finished && flash_succeeded && !flash_cancelled,
          "Qt flash bridge did not report successful completion");
    check(flash_running_started && flash_running_finished,
          "Qt flash bridge did not report running state transitions");
    check(flash_capture->ran && flash_capture->job.profile.channel == 2 &&
              flash_capture->job.profile.tx_id == 0x772 &&
              flash_capture->job.profile.rx_id == 0x77A &&
              flash_capture->job.profile.ft_tx_id == 0x701 &&
              flash_capture->job.profile.ft_rx_id == 0x761 &&
              flash_capture->job.entry_mode == L"ft" &&
              flash_capture->job.driver_file ==
                  std::filesystem::path(L"driver.s19"),
          "Qt flash bridge did not assemble the expected FlashJob");
    check(!flash_report.isEmpty() &&
              std::filesystem::is_regular_file(
                  std::filesystem::path(flash_report.toStdWString())),
          "Qt flash bridge did not return a generated report");
    check(callbacks_on_ui_thread,
           "Qt flash bridge delivered a callback outside the UI thread");
    check(!bridge.requestFlashStop(),
          "Qt flash bridge accepted a stop request after the task had ended");

    capture->sent_count = 0;
    capture->sent_id = 0;
    finished = false;
    succeeded = false;
    cancelled = false;
    timed_out = false;
    QTimer::singleShot(0, &bridge, [&bridge] {
      bridge.startProbe(1, QStringLiteral("secondary"), QStringLiteral("app"),
                        2, 0x123, 0x456);
    });
    QTimer::singleShot(3000, &application, [&] {
      timed_out = true;
      application.quit();
    });
    application.exec();
    check(!timed_out && finished && succeeded && !cancelled &&
              capture->sent_id == 0x760,
          "Longma secondary probe did not use the resolved 0x760/0x768 endpoint");

    capture->sent_count = 0;
    capture->sent_id = 0;
    capture->sent_subfunction = 0;
    finished = false;
    succeeded = false;
    cancelled = false;
    timed_out = false;
    QTimer::singleShot(0, &bridge, [&bridge] {
      bridge.startProbe(2, QStringLiteral("secondary"), QStringLiteral("ft"),
                        2, 0x760, 0x768);
    });
    QTimer::singleShot(3000, &application, [&] {
      timed_out = true;
      application.quit();
    });
    application.exec();
    check(!timed_out && finished && succeeded && !cancelled &&
              capture->sent_id == 0x714 &&
              capture->sent_subfunction == 0x03,
          "C857 secondary FT probe did not use target FT endpoint 0x714/0x71C");

    flash_capture->ran = false;
    flash_finished = false;
    flash_succeeded = false;
    flash_cancelled = false;
    timed_out = false;
    QTimer::singleShot(0, &bridge, [&bridge] {
      bridge.startFlash(
          1, QStringLiteral("secondary"), QStringLiteral("app"), 1, 2,
          0x123, 0x456, QStringLiteral("driver.s19"),
          QStringLiteral("app.s19"), {}, {}, {}, {},
          QStringLiteral("security.dll"));
    });
    QTimer::singleShot(3000, &application, [&] {
      timed_out = true;
      application.quit();
    });
    application.exec();
    check(!timed_out && flash_finished && flash_succeeded &&
               !flash_cancelled && flash_capture->ran &&
               flash_capture->job.profile.tx_id == 0x760 &&
               flash_capture->job.profile.rx_id == 0x768,
           "Longma secondary flash did not use the resolved 0x760/0x768 endpoint");
    check(std::any_of(
              emitted_logs.cbegin(), emitted_logs.cend(),
              [](const QString& line) {
                return line.contains(QStringLiteral("Flash target:")) &&
                       line.contains(QStringLiteral("从雷达（待验证）"));
              }),
          "Qt flash audit log did not use the selected target display name");

    flash_capture->ran = false;
    flash_finished = false;
    flash_succeeded = false;
    flash_cancelled = false;
    timed_out = false;
    QTimer::singleShot(0, &bridge, [&bridge] {
      bridge.startFlash(
          2, QStringLiteral("secondary"), QStringLiteral("ft"), 1, 2,
          0x760, 0x768, QStringLiteral("driver.s19"),
          QStringLiteral("secondary_app.s19"), {}, {}, {}, {},
          QStringLiteral("SeedKey_Slave.dll"));
    });
    QTimer::singleShot(3000, &application, [&] {
      timed_out = true;
      application.quit();
    });
    application.exec();
    check(!timed_out && flash_finished && flash_succeeded &&
              !flash_cancelled && flash_capture->ran &&
              flash_capture->job.profile.tx_id == 0x760 &&
              flash_capture->job.profile.rx_id == 0x768 &&
              flash_capture->job.profile.ft_tx_id == 0x714 &&
              flash_capture->job.profile.ft_rx_id == 0x71C &&
              flash_capture->job.entry_mode == L"ft",
          "C857 secondary FT flash did not apply the target-specific "
          "0x714/0x71C recovery endpoint");
    std::cout << "qt_probe_bridge_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "qt_probe_bridge_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
