#include "app/flash_request.hpp"
#include "ui/qt/main_window.hpp"
#include "ui/qt/bus_monitor_page.hpp"
#include "ui/qt/version_confirmation_page.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextFragment>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void send_wheel(QWidget* target, int angle_delta_y) {
  const QPointF local_position(target->rect().center());
  const QPointF global_position(
      target->mapToGlobal(local_position.toPoint()));
  QWheelEvent event(local_position, global_position, QPoint{},
                    QPoint(0, angle_delta_y), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(target, &event);
}

void checkpoint(const char* name) {
  std::cerr << "qt_main_window_tests: checkpoint " << name << std::endl;
}

int find_text(const QComboBox* combo, const QString& text) {
  for (int index = 0; index < combo->count(); ++index) {
    if (combo->itemText(index) == text) return index;
  }
  return -1;
}

QString target_id(const QComboBox* combo, int index) {
  return combo->itemData(index)
      .toMap()
      .value(QStringLiteral("target_id"))
      .toString();
}

int find_target(const QComboBox* combo, const QString& id) {
  for (int index = 0; index < combo->count(); ++index) {
    if (target_id(combo, index) == id) return index;
  }
  return -1;
}

void check_all_flash_projects_present(const QComboBox* projects) {
  check(find_text(projects, QStringLiteral("奇瑞")) >= 0,
        "Qt project selector is missing Chery");
  check(find_text(projects, QStringLiteral("楚能")) >= 0,
        "Qt project selector is missing Chuneng");
  check(find_text(projects, QStringLiteral("长马")) >= 0,
        "Qt project selector is missing Longma");
  check(find_text(projects, QStringLiteral("长安")) >= 0,
        "Qt project selector is missing Changan");
  check(find_text(projects, QStringLiteral("长安C857")) < 0 &&
            find_text(projects, QStringLiteral("铃耀_B216")) < 0,
        "C857 and B216 must be grouped under the Changan vendor");
  check(find_text(projects, QStringLiteral("时代新安")) >= 0,
        "Qt project selector is missing Shidaixinan");

  check(find_text(projects, QStringLiteral("犀重")) >= 0,
        "Qt project selector is missing exact Xizhong name");
  check(find_text(projects, QStringLiteral("零跑")) >= 0,
        "Qt project selector is missing Leapmotor");
  check(find_text(projects, QStringLiteral("吉利")) >= 0,
        "Qt project selector is missing Geely");
}

void check_project_devices(QApplication& application, QComboBox* projects,
                           QComboBox* devices, const QString& project,
                           const QStringList& expected_devices) {
  const auto project_index = find_text(projects, project);
  check(project_index >= 0, "Expected Qt project was not found");
  projects->setCurrentIndex(project_index);
  application.processEvents();
  check(devices->count() == expected_devices.size(),
        "Qt project device count mismatch");
  for (const auto& device : expected_devices) {
    check(find_text(devices, device) >= 0,
          "Qt project is missing its configured device");
  }
}

void check_file_row_geometry(uds::ui::qt::MainWindow& window) {
  auto* group = window.findChild<QGroupBox*>(QStringLiteral("filesGroupBox"));
  auto* scroll = window.findChild<QScrollArea*>(
      QStringLiteral("configurationScrollArea"));
  check(group && scroll && scroll->widgetResizable() &&
            scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
        "Flash-file scroll container is missing or misconfigured");

  const std::array<const char*, 7> edits{
      "driverPathLineEdit", "driverVerifyPathLineEdit", "appPathLineEdit",
      "appVerifyPathLineEdit", "calPathLineEdit", "calVerifyPathLineEdit",
      "seedKeyDllPathLineEdit"};
  const std::array<const char*, 7> buttons{
      "driverBrowseButton", "driverVerifyBrowseButton", "appBrowseButton",
      "appVerifyBrowseButton", "calBrowseButton", "calVerifyBrowseButton",
      "seedKeyDllBrowseButton"};
  int previous_bottom = -1;
  for (std::size_t index = 0; index < edits.size(); ++index) {
    auto* edit = window.findChild<QLineEdit*>(QString::fromLatin1(edits[index]));
    auto* button =
        window.findChild<QPushButton*>(QString::fromLatin1(buttons[index]));
    check(edit && button && edit->height() >= 24 && button->height() >= 24,
          "Flash-file row controls lost their compact minimum height");
    const auto edit_rect = QRect(edit->mapTo(group, QPoint{}), edit->size());
    const auto button_rect =
        QRect(button->mapTo(group, QPoint{}), button->size());
    check(!edit_rect.intersects(button_rect),
          "Flash-file field overlaps its browse button");
    check(edit_rect.top() > previous_bottom,
          "Flash-file rows overlap or changed vertical order");
    previous_bottom = edit_rect.bottom();
  }
}

void run_ui_monkey_test(QApplication& application,
                        uds::ui::qt::MainWindow& window,
                        QComboBox* projects, QComboBox* devices,
                        QComboBox* entries, QComboBox* radar,
                        QComboBox* channels, QSpinBox* repeat_count,
                        QTabWidget* workspace_tabs,
                        const QList<QAction*>& backend_actions) {
  check(projects && devices && entries && radar && channels && repeat_count &&
            workspace_tabs && !backend_actions.isEmpty(),
        "UI monkey prerequisites are missing");
  const auto project_count = projects->count();
  check(project_count > 0, "UI monkey has no project to exercise");
  QSettings settings;
  QVariantMap original_settings;
  for (const auto& key : settings.allKeys())
    original_settings.insert(key, settings.value(key));
  const auto original_backend =
      std::find_if(backend_actions.cbegin(), backend_actions.cend(),
                   [](const QAction* action) { return action->isChecked(); });

  // Exercise both repeat-count boundaries for every configured project before
  // starting the randomized selector sequence.
  for (int project = 0; project < project_count; ++project) {
    projects->setCurrentIndex(project);
    application.processEvents();
    repeat_count->setValue(
        static_cast<int>(uds::app::kMinFlashRepeatCount));
    check(repeat_count->value() == 1,
          "project rejected the minimum flash repeat count");
    repeat_count->setValue(
        static_cast<int>(uds::app::kMaxFlashRepeatCount));
    check(repeat_count->value() == 10000,
          "project rejected the maximum flash repeat count");
  }

  std::mt19937 random{0x554453U};
  std::uniform_int_distribution<int> operation(0, 6);
  std::uniform_int_distribution<int> repeat_value(
      static_cast<int>(uds::app::kMinFlashRepeatCount),
      static_cast<int>(uds::app::kMaxFlashRepeatCount));
  const auto random_index = [&random](int count) {
    std::uniform_int_distribution<int> index(0, count - 1);
    return index(random);
  };

  bool monkey_operations_ok = false;
  const auto configured_monkey_operations =
      qEnvironmentVariableIntValue("UDS_UI_MONKEY_OPERATIONS",
                                   &monkey_operations_ok);
  // Random clicking is an optional exploratory stress aid, not acceptance
  // evidence. Regular regressions use deterministic operator sequences and
  // explicit UI/result assertions; opt in through the environment only when
  // investigating broad state-space stability.
  const auto monkey_operations =
      monkey_operations_ok ? std::max(0, configured_monkey_operations)
                           : 0;
  std::cout << "qt_main_window_tests: optional exploratory UI random operations="
            << monkey_operations << '\n';
  for (int iteration = 0; iteration < monkey_operations; ++iteration) {
    switch (operation(random)) {
    case 0:
      projects->setCurrentIndex(random_index(projects->count()));
      break;
    case 1:
      if (devices->count() > 0)
        devices->setCurrentIndex(random_index(devices->count()));
      break;
    case 2:
      if (entries->count() > 0)
        entries->setCurrentIndex(random_index(entries->count()));
      break;
    case 3:
      if (radar->count() > 0)
        radar->setCurrentIndex(random_index(radar->count()));
      break;
    case 4:
      channels->setCurrentIndex(random_index(channels->count()));
      break;
    case 5:
      repeat_count->setValue(repeat_value(random));
      break;
    case 6:
      if ((iteration & 1) == 0) {
        workspace_tabs->setCurrentIndex(
            random_index(workspace_tabs->count()));
      } else {
        backend_actions[random_index(backend_actions.size())]->trigger();
      }
      break;
    }
    if ((iteration % 64) == 0) application.processEvents();
    check(repeat_count->value() >= 1 && repeat_count->value() <= 10000,
          "UI monkey escaped the flash repeat-count bounds");
  }
  application.processEvents();

  const auto checked_backends =
      std::count_if(backend_actions.cbegin(), backend_actions.cend(),
                    [](const QAction* action) { return action->isChecked(); });
  check(projects->count() == project_count && channels->count() == 4 &&
            workspace_tabs->count() == 4 && checked_backends == 1,
        "UI monkey changed selector structure or backend exclusivity");
  workspace_tabs->setCurrentIndex(0);
  projects->setCurrentIndex(0);
  repeat_count->setValue(1);
  if (original_backend != backend_actions.cend() &&
      !(*original_backend)->isChecked()) {
    (*original_backend)->trigger();
  }
  application.processEvents();
  settings.clear();
  for (auto it = original_settings.cbegin(); it != original_settings.cend();
       ++it) {
    settings.setValue(it.key(), it.value());
  }
  settings.sync();
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    checkpoint("startup");
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("UDSToolsTests"));
    QApplication::setApplicationName(QStringLiteral("uds_tool_qt_state_test"));
    application.setQuitOnLastWindowClosed(false);
    const auto settings_path = QDir::temp().filePath(
        QStringLiteral("uds_tool_qt_state_test_settings_%1")
            .arg(QCoreApplication::applicationPid()));
    QDir(settings_path).removeRecursively();
    QDir().mkpath(settings_path);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_path);

    QSettings settings;
    settings.clear();
    settings.sync();

    {
      uds::ui::qt::MainWindow window;
      window.show();
      for (const auto& size : {QSize(900, 620), QSize(1100, 720),
                               QSize(1280, 850)}) {
        window.resize(size);
        application.processEvents();
        check_file_row_geometry(window);
      }
      window.resize(1100, 720);
      application.processEvents();
      checkpoint("window-ready");
      const auto screenshot_directory =
          qEnvironmentVariable("UDS_UI_SCREENSHOT_DIR");
      if (!screenshot_directory.isEmpty()) {
        QDir().mkpath(screenshot_directory);
        const auto screenshot_project =
            qEnvironmentVariable("UDS_UI_SCREENSHOT_PROJECT");
        if (!screenshot_project.isEmpty()) {
          auto* screenshot_projects = window.findChild<QComboBox*>(
              QStringLiteral("projectComboBox"));
          const auto screenshot_project_index =
              find_text(screenshot_projects, screenshot_project);
          check(screenshot_project_index >= 0,
                "Requested UI screenshot project was not found");
          screenshot_projects->setCurrentIndex(screenshot_project_index);
          application.processEvents();
        }
        check(window.grab().save(
                  QDir(screenshot_directory)
                      .filePath(QStringLiteral("flash-page.png"))),
              "Failed to save flash-page UI screenshot");
        auto* tabs = window.findChild<QTabWidget*>(
            QStringLiteral("workspaceTabWidget"));
        check(tabs && tabs->count() >= 2,
              "Version page unavailable for UI screenshot");
        auto* preview_projects = window.findChild<QComboBox*>(
            QStringLiteral("projectComboBox"));
        auto* preview_devices = window.findChild<QComboBox*>(
            QStringLiteral("deviceComboBox"));
        auto* preview_radar = window.findChild<QComboBox*>(
            QStringLiteral("radarComboBox"));
        auto* preview_page =
            window.findChild<uds::ui::qt::VersionConfirmationPage*>(
                QStringLiteral("versionConfirmationPage"));
        const auto preview_project =
            find_text(preview_projects, QStringLiteral("长安"));
        check(preview_project >= 0 && preview_devices && preview_page,
              "Changan B216 version preview context unavailable");
        preview_projects->setCurrentIndex(preview_project);
        application.processEvents();
        const auto preview_device =
            find_text(preview_devices, QStringLiteral("B216"));
        check(preview_device >= 0,
              "Changan B216 project is unavailable for version preview");
        preview_devices->setCurrentIndex(preview_device);
        application.processEvents();
        const auto secondary =
            find_target(preview_radar, QStringLiteral("secondary"));
        check(secondary >= 0, "Changan B216 secondary target unavailable");
        preview_radar->setCurrentIndex(secondary);
        application.processEvents();
        const std::array<std::array<QString, 3>, 3> preview_rows{{
            {QStringLiteral("22 F1 89"), QStringLiteral("软件版本号"),
             QStringLiteral("SWD.00.7")},
            {QStringLiteral("22 F1 70"), QStringLiteral("FBL版本（Boot）"),
             QStringLiteral("V1.6")},
            {QStringLiteral("22 FD 05"), QStringLiteral("标定软件版本号"),
             QStringLiteral("00.00.00")},
        }};
        preview_page->clearResults();
        for (const auto& row : preview_rows) {
          preview_page->appendResult(
              QStringLiteral("成功"), row[0], row[1], row[2],
              QStringLiteral("62 ..."));
        }
        preview_page->finish(
            true, false,
            QStringLiteral("读取完成：全部必读版本信息读取成功"));
        tabs->setCurrentIndex(1);
        application.processEvents();
        check(window.grab().save(
                  QDir(screenshot_directory)
                      .filePath(QStringLiteral("version-page.png"))),
              "Failed to save version-page UI screenshot");
        tabs->setCurrentIndex(0);
        application.processEvents();
      }
      check(window.windowTitle() == QStringLiteral("UDS 通用刷写工具"),
            "Visible window title still exposes Qt");
      const auto flags = window.windowFlags();
      check(window.windowType() == Qt::Window &&
                flags.testFlag(Qt::WindowMinimizeButtonHint) &&
                flags.testFlag(Qt::WindowMaximizeButtonHint) &&
                flags.testFlag(Qt::WindowCloseButtonHint),
            "Window did not restore native minimize/maximize controls");
      auto* backend_group = window.findChild<QActionGroup*>(
          QStringLiteral("canBackendActionGroup"));
      auto* vector_backend = window.findChild<QAction*>(
          QStringLiteral("vectorCanBackendAction"));
      auto* tosun_backend = window.findChild<QAction*>(
          QStringLiteral("tosunCanBackendAction"));
      auto* zlg_backend = window.findChild<QAction*>(
          QStringLiteral("zlgCanBackendAction"));
      auto* kvaser_backend = window.findChild<QAction*>(
          QStringLiteral("kvaserCanBackendAction"));
      const auto backend_actions =
          backend_group ? backend_group->actions() : QList<QAction*>{};
      check(backend_group && backend_group->isExclusive() && vector_backend &&
                tosun_backend && zlg_backend && kvaser_backend &&
                vector_backend->isChecked() &&
                tosun_backend->text().contains(QStringLiteral("TSMaster")) &&
                zlg_backend->text().contains(QStringLiteral("ZCANPRO")) &&
                kvaser_backend->text().contains(QStringLiteral("Kvaser")) &&
                backend_actions.size() == 4 &&
                backend_actions[0] == vector_backend &&
                backend_actions[1] == zlg_backend &&
                backend_actions[2] == tosun_backend &&
                backend_actions[3] == kvaser_backend,
            "Device menu CAN backend selector is missing or mislabeled");
      checkpoint("backend-menu");
      zlg_backend->trigger();
      application.processEvents();
      check(zlg_backend->isChecked() &&
                QSettings()
                        .value(QStringLiteral("hardware/can_vendor"))
                        .toString() == QStringLiteral("zlg"),
            "ZLG backend menu selection was not applied and persisted");
      auto* projects = window.findChild<QComboBox*>(
          QStringLiteral("projectComboBox"));
      auto* project_label =
          window.findChild<QLabel*>(QStringLiteral("projectLabel"));
      auto* devices = window.findChild<QComboBox*>(
          QStringLiteral("deviceComboBox"));
      auto* device_label =
          window.findChild<QLabel*>(QStringLiteral("deviceLabel"));
      auto* entries = window.findChild<QComboBox*>(
          QStringLiteral("entryModeComboBox"));
      auto* radar = window.findChild<QComboBox*>(
          QStringLiteral("radarComboBox"));
      auto* radar_label =
          window.findChild<QLabel*>(QStringLiteral("radarLabel"));
      auto* channels = window.findChild<QComboBox*>(
          QStringLiteral("vectorChannelComboBox"));
      auto* repeat_count = window.findChild<QSpinBox*>(
          QStringLiteral("repeatCountSpinBox"));
      auto* start_flash = window.findChild<QPushButton*>(
          QStringLiteral("startFlashButton"));
      auto* probe = window.findChild<QPushButton*>(
          QStringLiteral("probeButton"));
      auto* workspace_tabs = window.findChild<QTabWidget*>(
          QStringLiteral("workspaceTabWidget"));
      auto* flash_page = window.findChild<QWidget*>(
          QStringLiteral("flashWorkspacePage"));
      auto* version_page = window.findChild<QWidget*>(
          QStringLiteral("versionConfirmationPage"));
      auto* version_table = window.findChild<QTableWidget*>(
          QStringLiteral("versionResultTable"));
      auto* version_button = window.findChild<QPushButton*>(
          QStringLiteral("versionCheckButton"));
      auto* version_selection = window.findChild<QLabel*>(
          QStringLiteral("versionSelectionSummary"));
      auto* version_address = window.findChild<QLabel*>(
          QStringLiteral("versionAddressSummary"));
      auto* diagnostic_page = window.findChild<QWidget*>(
          QStringLiteral("diagnosticRequestPage"));
      auto* diagnostic_context = window.findChild<QLabel*>(
          QStringLiteral("diagnosticRequestContext"));
      auto* diagnostic_addressing = window.findChild<QComboBox*>(
          QStringLiteral("diagnosticAddressingComboBox"));
      auto* diagnostic_payload = window.findChild<QLineEdit*>(
          QStringLiteral("diagnosticPayloadLineEdit"));
      auto* diagnostic_timeout = window.findChild<QSpinBox*>(
          QStringLiteral("diagnosticTimeoutSpinBox"));
      auto* diagnostic_send = window.findChild<QPushButton*>(
          QStringLiteral("diagnosticSendButton"));
      auto* diagnostic_stop = window.findChild<QPushButton*>(
          QStringLiteral("diagnosticStopButton"));
      auto* diagnostic_raw = window.findChild<QPlainTextEdit*>(
          QStringLiteral("diagnosticRawCommunication"));
      auto* bus_monitor_page =
          window.findChild<uds::ui::qt::BusMonitorPage*>(
          QStringLiteral("busMonitorWorkspacePage"));
      auto* bus_monitor_table = window.findChild<QTableWidget*>(
          QStringLiteral("busMonitorTable"));
      auto* bus_monitor_tx_filter = window.findChild<QCheckBox*>(
          QStringLiteral("busMonitorTxFilter"));
      auto* bus_monitor_rx_filter = window.findChild<QCheckBox*>(
          QStringLiteral("busMonitorRxFilter"));
      auto* bus_monitor_diagnostic_filter = window.findChild<QCheckBox*>(
          QStringLiteral("busMonitorDiagnosticOnlyFilter"));
      auto* bus_monitor_clear = window.findChild<QPushButton*>(
          QStringLiteral("busMonitorClearButton"));
      auto* bus_monitor_export = window.findChild<QPushButton*>(
          QStringLiteral("busMonitorExportButton"));
      auto* bus_monitor_context = window.findChild<QLabel*>(
          QStringLiteral("busMonitorContextLabel"));
      auto* bus_monitor_trace_status = window.findChild<QLabel*>(
          QStringLiteral("busMonitorTraceStatusLabel"));
      auto* bus_monitor_total = window.findChild<QLabel*>(
          QStringLiteral("busMonitorTotalCount"));
      auto* bus_monitor_displayed = window.findChild<QLabel*>(
          QStringLiteral("busMonitorDisplayedCount"));
      auto* bus_monitor_evicted = window.findChild<QLabel*>(
          QStringLiteral("busMonitorEvictedCount"));
      auto* bus_monitor_id_filter = window.findChild<QLineEdit*>(
          QStringLiteral("busMonitorIdFilter"));
      auto* bus_monitor_id_error = window.findChild<QLabel*>(
          QStringLiteral("busMonitorIdFilterError"));
      auto* bus_monitor_project_shortcut = window.findChild<QPushButton*>(
          QStringLiteral("busMonitorProjectDiagnosticShortcut"));
      auto* bus_monitor_functional_shortcut = window.findChild<QPushButton*>(
          QStringLiteral("busMonitorFunctionalShortcut"));
      auto* bus_monitor_physical_shortcut = window.findChild<QPushButton*>(
          QStringLiteral("busMonitorPhysicalShortcut"));
      auto* bus_monitor_periodic_shortcut = window.findChild<QPushButton*>(
          QStringLiteral("busMonitorPeriodicShortcut"));
      check(workspace_tabs && workspace_tabs->count() == 4 &&
                workspace_tabs->currentIndex() == 0 &&
                 workspace_tabs->currentWidget() == flash_page &&
                 version_page && version_table &&
                 version_table->columnCount() == 5 && version_button &&
                 version_selection && version_address &&
                 version_button->text() == QStringLiteral("一键读取") &&
                diagnostic_page && diagnostic_context &&
                diagnostic_context->text().contains(QStringLiteral("CH")) &&
                diagnostic_addressing && diagnostic_addressing->count() == 2 &&
                diagnostic_payload && diagnostic_timeout &&
                diagnostic_timeout->value() == 2000 && diagnostic_send &&
                diagnostic_send->text() == QStringLiteral("发送并等待响应") &&
                diagnostic_stop && !diagnostic_stop->isEnabled() &&
                diagnostic_raw && diagnostic_raw->isReadOnly() &&
                bus_monitor_page && bus_monitor_table &&
                bus_monitor_table->columnCount() == 7 &&
                bus_monitor_table->horizontalHeaderItem(6)->text() ==
                    QStringLiteral("诊断提示") &&
                !window.findChild<QWidget*>(
                    QStringLiteral("reportWorkspacePage")) &&
                !window.findChild<QWidget*>(
                    QStringLiteral("fileToolsWorkspacePage")),
            "Workspace tabs are missing or flash is not the default page");
      check(workspace_tabs->tabText(0) == QStringLiteral("刷写作业") &&
                workspace_tabs->tabText(1) == QStringLiteral("版本读取") &&
                workspace_tabs->tabText(2) == QStringLiteral("诊断报文") &&
                workspace_tabs->tabText(3) == QStringLiteral("总线监听"),
            "Workspace tab labels or ordering changed");
      check(projects && project_label && devices && device_label && entries &&
                bus_monitor_tx_filter && bus_monitor_tx_filter->isChecked() &&
                bus_monitor_rx_filter && bus_monitor_rx_filter->isChecked() &&
                bus_monitor_diagnostic_filter &&
                bus_monitor_diagnostic_filter->isChecked() &&
                 bus_monitor_clear &&
                 bus_monitor_clear->text() == QStringLiteral("清空列表") &&
                 !window.findChild<QPushButton*>(
                     QStringLiteral("busMonitorStartStopButton")) &&
                 bus_monitor_export &&
                 bus_monitor_export->text() == QStringLiteral("导出 ASC") &&
                 bus_monitor_context &&
                 !bus_monitor_context->text().isEmpty() &&
                 bus_monitor_context->text().contains(QStringLiteral("CH")) &&
                bus_monitor_trace_status &&
                bus_monitor_trace_status->text().contains(
                    QStringLiteral("完整 Trace")) &&
                bus_monitor_total &&
                bus_monitor_total->text() == QStringLiteral("总接收：0") &&
                bus_monitor_displayed &&
                bus_monitor_displayed->text() ==
                    QStringLiteral("当前显示：0") &&
                bus_monitor_evicted &&
                bus_monitor_evicted->text() ==
                    QStringLiteral("内存已淘汰：0") &&
                bus_monitor_id_filter && bus_monitor_id_error &&
                bus_monitor_project_shortcut &&
                bus_monitor_functional_shortcut &&
                bus_monitor_physical_shortcut &&
                bus_monitor_periodic_shortcut &&
                radar && radar_label &&
                 channels && repeat_count,
               "Qt selectors were not created");
      check(channels->isEnabled(),
            "Idle CAN channel selector must remain operator-selectable");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "runningChanged", Qt::DirectConnection,
                Q_ARG(bool, true)),
            "Failed to simulate automatic bus-monitor startup");
      check(channels->isEnabled(),
            "Automatic passive monitoring must not lock the CAN channel selector");
      check(backend_group && backend_group->isEnabled(),
            "Passive monitoring must allow safe CAN backend switching");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "runningChanged", Qt::DirectConnection,
                Q_ARG(bool, false)),
            "Failed to restore simulated bus-monitor state");
      check(backend_group && backend_group->isEnabled(),
            "Stopping bus monitor must unlock CAN backend switching");
      bus_monitor_page->setDiagnosticAddressing({0x72E, 0x72F}, {0x7DF});
      bus_monitor_page->appendObservedFrame(
          uds::CanFrame{0x123,
                        {0x01, 0x02, 0x03, 0x04, 0, 0, 0, 0},
                        false, false, false, false});
      check(bus_monitor_table->rowCount() == 0,
            "Default diagnostic-ID filter did not hide a non-diagnostic frame");
      bus_monitor_page->appendObservedFrame(
          uds::CanFrame{0x72F,
                        {0x03, 0x7F, 0x31, 0x31, 0, 0, 0, 0},
                        false, false, false, false});
      check(bus_monitor_table->rowCount() == 1 &&
                bus_monitor_table->item(0, 0)->data(Qt::UserRole) ==
                    QStringLiteral("failure") &&
                bus_monitor_table->item(0, 0)->foreground().color() ==
                    QColor(QStringLiteral("#B71C1C")) &&
                bus_monitor_table->item(0, 6)->text().contains(
                    QStringLiteral("NRC 0x31 RequestOutOfRange")),
            "Bus monitor did not render final NRC31 as a red failure row");
      bus_monitor_page->appendObservedFrame(
          uds::CanFrame{0x72F,
                        {0x03, 0x7F, 0x31, 0x78, 0, 0, 0, 0},
                        false, false, false, false});
      check(bus_monitor_table->rowCount() == 2 &&
                bus_monitor_table->item(1, 0)->data(Qt::UserRole) ==
                    QStringLiteral("normal") &&
                bus_monitor_table->item(1, 6)->text().isEmpty(),
            "Bus monitor should retain NRC78 as an unannotated raw frame");
      bus_monitor_page->appendObservedFrame(
          uds::CanFrame{0x72F,
                        {0x05, 0x71, 0x01, 0x02, 0x02, 0x05, 0x55, 0x55},
                        false, false, false, false});
      check(bus_monitor_table->rowCount() == 3 &&
                bus_monitor_table->item(2, 0)->data(Qt::UserRole) ==
                    QStringLiteral("failure") &&
                bus_monitor_table->item(2, 0)->foreground().color() ==
                    QColor(QStringLiteral("#B71C1C")) &&
                bus_monitor_table->item(2, 6)->text().contains(
                    QStringLiteral("数据/软件签名校验")) &&
                bus_monitor_table->item(2, 6)->text().contains(
                    QStringLiteral("状态 0x05")),
            "Bus monitor did not render RoutineControl 0202 status 05 as a "
            "red explained failure row");
      bus_monitor_diagnostic_filter->setChecked(false);
      check(bus_monitor_table->rowCount() == 4 &&
                bus_monitor_table->item(0, 2)->text().compare(
                    QStringLiteral("0x123"), Qt::CaseInsensitive) == 0,
            "Disabling the diagnostic-ID filter did not restore retained raw frames");
      check(bus_monitor_total->text() == QStringLiteral("总接收：4") &&
                bus_monitor_displayed->text() ==
                    QStringLiteral("当前显示：4") &&
                bus_monitor_evicted->text() ==
                    QStringLiteral("内存已淘汰：0"),
            "Bus monitor evidence counters did not track received/displayed frames");
      bus_monitor_id_filter->setText(QStringLiteral("72F"));
      check(bus_monitor_table->rowCount() == 3 &&
                bus_monitor_id_error->text().isEmpty(),
            "Exact CAN ID filter did not match three 0x72F rows");
      bus_monitor_id_filter->setText(QStringLiteral("120-130"));
      check(bus_monitor_table->rowCount() == 1,
            "CAN ID range filter did not match 0x123");
      bus_monitor_id_filter->setText(QStringLiteral("72x"));
      check(bus_monitor_table->rowCount() == 3,
            "CAN ID nibble-mask filter did not match 0x72F");
      bus_monitor_id_filter->setText(QStringLiteral("!72F"));
      check(bus_monitor_table->rowCount() == 1,
            "CAN ID exclusion filter did not remove 0x72F");
      bus_monitor_id_filter->setText(QStringLiteral("123、72F"));
      check(bus_monitor_table->rowCount() == 4,
            "Chinese-dunhao CAN ID delimiter was not accepted");
      bus_monitor_id_filter->setText(QStringLiteral("7FF-700"));
      check(bus_monitor_table->rowCount() == 4 &&
                bus_monitor_id_error->text().contains(
                    QStringLiteral("范围起点不能大于终点")) &&
                bus_monitor_id_filter->styleSheet().contains(
                    QStringLiteral("C62828")),
            "Invalid CAN ID filter was not explained and highlighted");
      bus_monitor_functional_shortcut->click();
      check(bus_monitor_id_filter->text().compare(
                    QStringLiteral("7df"), Qt::CaseInsensitive) == 0 &&
                bus_monitor_table->rowCount() == 0,
            "Functional-addressing shortcut did not select 0x7DF");
      bus_monitor_physical_shortcut->click();
      check(bus_monitor_table->rowCount() == 3 &&
                bus_monitor_id_filter->text().contains(
                    QStringLiteral("72e"), Qt::CaseInsensitive) &&
                bus_monitor_id_filter->text().contains(
                    QStringLiteral("72f"), Qt::CaseInsensitive),
            "Physical-addressing shortcut did not select project endpoints");
      bus_monitor_periodic_shortcut->click();
      check(bus_monitor_table->rowCount() == 1 &&
                bus_monitor_id_filter->text().contains(
                    QStringLiteral("!7df"), Qt::CaseInsensitive),
            "Periodic-frame shortcut did not exclude project diagnostic IDs");
      bus_monitor_clear->click();
      check(bus_monitor_table->rowCount() == 0 &&
                bus_monitor_total->text() == QStringLiteral("总接收：4") &&
                bus_monitor_displayed->text() ==
                    QStringLiteral("当前显示：0"),
            "Clearing the bus monitor view also cleared evidence counters");
      check(project_label->text() == QStringLiteral("厂商") &&
                device_label->text() == QStringLiteral("项目选择") &&
                radar_label->text() == QStringLiteral("设备选择"),
            "Generic selector labels must be 厂商, 项目选择 and 设备选择");
      check(repeat_count->minimum() ==
                    static_cast<int>(uds::app::kMinFlashRepeatCount) &&
                repeat_count->maximum() ==
                    static_cast<int>(uds::app::kMaxFlashRepeatCount) &&
                repeat_count->value() == 1 && window.width() <= 1100 &&
                window.height() <= 720,
            "Generic flash count or compact window geometry mismatch");
      auto* repeat_editor = repeat_count->findChild<QLineEdit*>();
      check(!repeat_count->isReadOnly() &&
                repeat_count->focusPolicy() == Qt::StrongFocus && repeat_editor &&
                !repeat_editor->isReadOnly(),
            "Flash repeat count must accept direct keyboard input");
      const auto original_project_index = projects->currentIndex();
      check(projects->count() > 2,
            "Wheel mutation guard needs multiple project choices");
      projects->setCurrentIndex(1);
      application.processEvents();
      const auto guarded_project_index = projects->currentIndex();
      send_wheel(projects, -120);
      application.processEvents();
      check(projects->currentIndex() == guarded_project_index,
            "Mouse wheel changed the selected project");
      projects->setCurrentIndex(original_project_index);
      application.processEvents();

      repeat_count->setValue(2);
      const auto guarded_repeat_count = repeat_count->value();
      auto* configuration_scroll = window.findChild<QScrollArea*>(
          QStringLiteral("configurationScrollArea"));
      configuration_scroll->verticalScrollBar()->setValue(0);
      const auto scroll_before =
          configuration_scroll->verticalScrollBar()->value();
      send_wheel(repeat_count, -120);
      application.processEvents();
      check(repeat_count->value() == guarded_repeat_count,
            "Mouse wheel changed the flash repeat count");
      if (configuration_scroll->verticalScrollBar()->maximum() > 0) {
        check(configuration_scroll->verticalScrollBar()->value() >
                  scroll_before,
              "Wheel guard did not preserve configuration-page scrolling");
      }
      configuration_scroll->verticalScrollBar()->setValue(0);
      repeat_count->setValue(1);
      check(start_flash && start_flash->text() == QStringLiteral("开始刷写") &&
                probe && probe->text() == QStringLiteral("能否刷写") &&
                window.styleSheet().isEmpty(),
            "Flash action labels or native application style mismatch");
      const auto selected_project = projects->currentText();
      const auto selected_device = devices->currentText();
      const auto flash_button_text = start_flash->text();
      for (int index = 1; index < workspace_tabs->count(); ++index) {
        workspace_tabs->setCurrentIndex(index);
        application.processEvents();
      }
      workspace_tabs->setCurrentIndex(0);
      application.processEvents();
      check(projects->currentText() == selected_project &&
                devices->currentText() == selected_device &&
                start_flash->text() == flash_button_text &&
                flash_page->isAncestorOf(start_flash),
            "Switching workspace pages changed the flash page state");
      auto* app_data_browse = window.findChild<QPushButton*>(
          QStringLiteral("appVerifyBrowseButton"));
      auto* driver_browse = window.findChild<QPushButton*>(
          QStringLiteral("driverBrowseButton"));
      check(app_data_browse && driver_browse &&
                app_data_browse->property("fileDialogFilter")
                    .toString()
                    .contains(QStringLiteral("*.s19")) &&
                driver_browse->property("fileDialogFilter")
                    .toString()
                    .contains(QStringLiteral("*.s19")),
            "Qt file dialogs still hide S19 resources");
      const auto check_file_panel_is_stable = [&window]() {
        const auto* driver_label =
            window.findChild<QLabel*>(QStringLiteral("driverPathLabel"));
        const auto* app_verify_label = window.findChild<QLabel*>(
            QStringLiteral("appVerifyPathLabel"));
        const auto* app_verify_path = window.findChild<QLineEdit*>(
            QStringLiteral("appVerifyPathLineEdit"));
        check(driver_label &&
                  driver_label->text() == QStringLiteral("Driver 文件") &&
                  app_verify_label &&
                  app_verify_label->text() ==
                      QStringLiteral("APP 校验文件") &&
                   app_verify_path,
               "Flash-file labels changed between projects");
        for (const auto& name :
             {"driverPathLineEdit", "driverVerifyPathLineEdit",
              "appPathLineEdit", "appVerifyPathLineEdit",
              "calPathLineEdit", "calVerifyPathLineEdit",
              "seedKeyDllPathLineEdit"}) {
          const auto* path_edit = window.findChild<QLineEdit*>(
              QString::fromLatin1(name));
          check(path_edit &&
                    (path_edit->property("fullPath").toString().isEmpty()
                         ? path_edit->placeholderText().isEmpty()
                         : !path_edit->placeholderText().isEmpty()),
                "Unused flash-file row still shows a selection placeholder");
        }
        for (const auto& name :
             {"driverPathLabel", "driverPathLineEdit", "driverBrowseButton",
              "driverVerifyPathLabel", "driverVerifyPathLineEdit",
              "driverVerifyBrowseButton", "appPathLabel", "appPathLineEdit",
              "appBrowseButton", "appVerifyPathLabel",
              "appVerifyPathLineEdit", "appVerifyBrowseButton",
              "calPathLabel", "calPathLineEdit", "calBrowseButton",
              "calVerifyPathLabel", "calVerifyPathLineEdit",
              "calVerifyBrowseButton", "seedKeyDllPathLabel",
              "seedKeyDllPathLineEdit", "seedKeyDllBrowseButton"}) {
          const auto* widget = window.findChild<QWidget*>(
              QString::fromLatin1(name));
          check(widget && !widget->isHidden(),
                "A generic flash-file row changed visibility by profile");
        }
      };
      auto* log_view = window.findChild<QPlainTextEdit*>(
          QStringLiteral("logPlainTextEdit"));
      auto* clear_log = window.findChild<QAction*>(
          QStringLiteral("clearLogAction"));
      auto* left_work_panel = window.findChild<QWidget*>(
          QStringLiteral("leftWorkPanel"));
      auto* log_group = window.findChild<QGroupBox*>(
          QStringLiteral("logGroupBox"));
      auto* progress_status = window.findChild<QLabel*>(
          QStringLiteral("progressStatusLabel"));
      check(left_work_panel && log_group && progress_status &&
                progress_status->wordWrap() &&
                progress_status->sizePolicy().horizontalPolicy() ==
                    QSizePolicy::Ignored,
            "Shared runtime status does not constrain long text to its column");
      const auto left_width = left_work_panel->width();
      const auto log_width = log_group->width();
      for (const auto& status : {
               QStringLiteral("在线探测运行中……"),
               QStringLiteral(
                   "APP入口不可用：ECU很可能处于Boot/SBL恢复态（常见于擦除中断）；普通发布版不提供Boot恢复入口。请停止重复使用APP入口，改用受控恢复版本并由具备授权的人员执行恢复。"),
               QStringLiteral(
                   "完整刷写运行中：正在等待当前UDS请求结束、生成报告并确认ECU恢复状态，请保持供电。")}) {
        progress_status->setText(status);
        application.processEvents();
        check(left_work_panel->width() == left_width &&
                  log_group->width() == log_width,
              "Runtime status text moved the shared workspace columns");
      }
      const auto execution_log =
          window.property("executionLogPath").toString();
      check(log_view && clear_log && !execution_log.isEmpty() &&
                QFile::exists(execution_log),
            "Execution log persistence or clear action is missing");
      QFile persisted(execution_log);
      check(persisted.open(QIODevice::ReadOnly) &&
                persisted.readAll().contains("界面已就绪"),
            "Execution log did not persist UI messages");
      QString long_execution_log;
      for (int line = 0; line < 240; ++line) {
        long_execution_log +=
            QStringLiteral("execution log regression line %1 with wrapped payload data\n")
                .arg(line);
      }
      long_execution_log += QStringLiteral("last");
      log_view->setPlainText(long_execution_log);
      application.processEvents();
      QKeyEvent home_event(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
      QApplication::sendEvent(log_view, &home_event);
      check(log_view->textCursor().position() == 0,
            "Home did not navigate to the beginning of the execution log");
      QKeyEvent end_event(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
      QApplication::sendEvent(log_view, &end_event);
       check(log_view->textCursor().position() ==
                 log_view->document()->characterCount() - 1,
             "End did not navigate to the end of the execution log");
      const auto invoke_flash_result = [&](bool success, bool cancelled) {
        return QMetaObject::invokeMethod(
            &window, "handleFlashFinished", Qt::DirectConnection,
            Q_ARG(bool, success), Q_ARG(bool, cancelled),
            Q_ARG(QString, QStringLiteral("test flash result")),
            Q_ARG(QString, QString{}));
      };
      check(invoke_flash_result(true, false),
            "Flash result handler is not invokable");
      application.processEvents();
      check(log_view->verticalScrollBar()->value() ==
                log_view->verticalScrollBar()->maximum(),
            "Execution log did not follow appended output after End");
      auto result_block = log_view->document()->lastBlock();
      auto result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(QStringLiteral("刷写成功")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#16803C")) &&
                result_format.fontWeight() == QFont::Bold,
            "Successful flash result is not a bold green log entry");
      check(invoke_flash_result(false, false),
            "Flash failure handler is not invokable");
      application.processEvents();
      check(log_view->verticalScrollBar()->value() ==
                log_view->verticalScrollBar()->maximum(),
            "Execution log did not keep following consecutive output");
      result_block = log_view->document()->lastBlock();
      result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(QStringLiteral("刷写失败")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#C62828")) &&
                result_format.fontWeight() == QFont::Bold,
            "Failed flash result is not a bold red log entry");
      check(QMetaObject::invokeMethod(
                &window, "handleFlashFinished", Qt::DirectConnection,
                Q_ARG(bool, false), Q_ARG(bool, false),
                Q_ARG(QString,
                      QStringLiteral("31 01 60 00 CertificateDownload: "
                                     "NRC/timeout NRC 0x31")),
                Q_ARG(QString, QString{})),
            "NRC flash failure handler is not invokable");
      auto nrc_reason_block =
          log_view->document()->lastBlock().previous().previous();
      auto nrc_reason_format =
          nrc_reason_block.begin().fragment().charFormat();
      check(nrc_reason_block.text().contains(
                QStringLiteral("NRC 0x31 RequestOutOfRange")) &&
                nrc_reason_block.text().contains(
                    QStringLiteral("请求超出范围")) &&
                !nrc_reason_block.text().contains(
                    QStringLiteral("NRC/timeout")) &&
                nrc_reason_format.foreground().color() ==
                    QColor(QStringLiteral("#C62828")) &&
                nrc_reason_format.fontWeight() == QFont::Bold,
            "Execution log did not explain and emphasize the concrete NRC31");
      const auto blocks_before_pending = log_view->document()->blockCount();
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString,
                      QStringLiteral("RX [0x72F] 7F 31 78"))),
            "Pending log injection failed");
      check(log_view->document()->blockCount() == blocks_before_pending &&
                !log_view->document()->lastBlock().text().contains(
                    QStringLiteral("NRC 0x78")),
            "Execution log should suppress expected NRC78 wait states");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString,
                      QStringLiteral(
                          "RX [0x72F] 67 11 45 85 25 FF E1 D9 A4 59 9F "
                          "40 7B 8D 3E 7F 1A CB"))),
            "Security seed log injection failed");
      result_block = log_view->document()->lastBlock();
      result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(QStringLiteral("67 11")) &&
                !result_block.text().contains(QStringLiteral("NRC 0xCB")) &&
                result_format.foreground().color() !=
                    QColor(QStringLiteral("#C62828")),
            "Security seed payload bytes were misclassified as an NRC");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString,
                      QStringLiteral("RX [0x72F] 71 01 02 03 05"))),
            "Routine 0203 warning log injection failed");
      result_block = log_view->document()->lastBlock();
      result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(
                QStringLiteral("71 01 02 03 05")) &&
                !result_block.text().contains(
                    QStringLiteral("校验/执行失败")) &&
                result_format.foreground().color() !=
                    QColor(QStringLiteral("#C62828")),
            "Execution log misclassified ARC331 0203 status 05 as a red "
            "final failure");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString,
                      QStringLiteral("RX [0x72F] 71 01 02 02 05"))),
            "Routine failure log injection failed");
      result_block = log_view->document()->lastBlock();
      result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(
                QStringLiteral("RoutineControl 0x0202")) &&
                result_block.text().contains(
                    QStringLiteral("数据/软件签名校验")) &&
                result_block.text().contains(QStringLiteral("状态 0x05")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#C62828")) &&
                result_format.fontWeight() == QFont::Bold,
            "Execution log did not explain and emphasize routine 0202 "
            "status 05");
      const auto invoke_probe_result = [&](bool success) {
        return QMetaObject::invokeMethod(
            &window, "handleProbeFinished", Qt::DirectConnection,
            Q_ARG(bool, success), Q_ARG(bool, false),
            Q_ARG(QString, success ? QStringLiteral("设备在线：响应 50 03")
                                   : QStringLiteral("在线探测失败")));
      };
      check(invoke_probe_result(true),
            "Probe result handler is not invokable");
      result_block = log_view->document()->lastBlock();
      result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(QStringLiteral("● 在线：")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#16803C")) &&
                result_format.fontWeight() == QFont::Bold,
            "Online probe result is not a bold green log entry");
      check(invoke_probe_result(false),
            "Probe failure handler is not invokable");
      result_block = log_view->document()->lastBlock();
      result_format = result_block.begin().fragment().charFormat();
      check(result_block.text().contains(QStringLiteral("● 不在线：")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#C62828")) &&
                result_format.fontWeight() == QFont::Bold,
            "Offline probe result is not a bold red log entry");
      QApplication::sendEvent(log_view, &home_event);
      application.processEvents();
      const auto review_scroll_position =
          log_view->verticalScrollBar()->value();
      check(review_scroll_position < log_view->verticalScrollBar()->maximum(),
            "Execution log review position did not leave the tail");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("tail follow paused regression"))),
            "Paused-tail log injection failed");
      application.processEvents();
      check(log_view->verticalScrollBar()->value() == review_scroll_position,
            "Execution log stole the view while tail following was paused");
      QApplication::sendEvent(log_view, &end_event);
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("tail follow resumed regression"))),
            "Resumed-tail log injection failed");
      application.processEvents();
      check(log_view->verticalScrollBar()->value() ==
                log_view->verticalScrollBar()->maximum(),
            "Execution log did not resume tail following after End");
      auto* tx_id =
          window.findChild<QLineEdit*>(QStringLiteral("txIdLineEdit"));
      auto* rx_id =
          window.findChild<QLineEdit*>(QStringLiteral("rxIdLineEdit"));
      auto* chuneng_seed_key = window.findChild<QLineEdit*>(
          QStringLiteral("seedKeyDllPathLineEdit"));
      auto* chuneng_driver_path = window.findChild<QLineEdit*>(
          QStringLiteral("driverPathLineEdit"));
      auto* chuneng_app_path = window.findChild<QLineEdit*>(
          QStringLiteral("appPathLineEdit"));
      check(tx_id && rx_id && chuneng_seed_key && chuneng_driver_path &&
                chuneng_app_path,
            "Qt diagnostic ID, CBF, or SeedKey fields are missing");
      check_all_flash_projects_present(projects);
      check_project_devices(application, projects, devices,
                            QStringLiteral("楚能"),
                            {QStringLiteral("ARC331")});
      check(entries->findData(QStringLiteral("boot")) < 0 &&
                entries->findData(QStringLiteral("app")) >= 0 &&
                entries->findData(QStringLiteral("ft")) >= 0,
            "Professional dist exposed the bench-only ARC331 Boot recovery entry");
      check(radar->count() == 2 &&
                radar->currentText() == QStringLiteral("右后雷达") &&
                target_id(radar, radar->currentIndex()) ==
                    QStringLiteral("right_rear") &&
                tx_id->isReadOnly() && rx_id->isReadOnly() &&
                tx_id->text() == QStringLiteral("0x72C") &&
                rx_id->text() == QStringLiteral("0x72D") &&
                chuneng_driver_path->text() ==
                    QStringLiteral("driver_712345678AB.cbf") &&
                chuneng_app_path->text() ==
                    QStringLiteral("7052A5023002AB.cbf") &&
                chuneng_seed_key->text() ==
                    QStringLiteral("ChuNeng_D7_SeednKey_V1.0.dll"),
            "Chuneng ARC331 endpoint or 16-byte SeedKey resource mismatch");
      check(version_selection->text().contains(QStringLiteral("楚能")) &&
                version_selection->text().contains(QStringLiteral("ARC331")) &&
                version_selection->text().contains(QStringLiteral("右后雷达")) &&
                version_address->text().contains(
                    QStringLiteral("CH%1").arg(channels->currentData().toUInt())) &&
                version_address->text().contains(QStringLiteral("0X72C")) &&
                version_address->text().contains(QStringLiteral("0X72D")) &&
                version_table->rowCount() == 5 &&
                version_table->item(0, 1)->text() == QStringLiteral("F187") &&
                version_table->item(0, 3)->text() ==
                    QStringLiteral("ECU零件号"),
            "Version page did not mirror the Chuneng selection and DID plan");
      radar->setCurrentIndex(find_target(radar, QStringLiteral("left_rear")));
      application.processEvents();
      check(radar->currentText() == QStringLiteral("左后雷达") &&
                tx_id->text() == QStringLiteral("0x72E") &&
                rx_id->text() == QStringLiteral("0x72F"),
            "Chuneng ARC331 left-rear endpoint mismatch");
      check(version_selection->text().contains(QStringLiteral("左后雷达")) &&
                version_address->text().contains(QStringLiteral("0X72E")) &&
                version_address->text().contains(QStringLiteral("0X72F")),
            "Version page did not follow the selected ARC331 device");
      check(tx_id->isReadOnly() && rx_id->isReadOnly(),
            "Locked Chuneng diagnostic IDs became editable");
      radar->setCurrentIndex(find_target(radar, QStringLiteral("right_rear")));
      application.processEvents();
      check(tx_id->text() == QStringLiteral("0x72C") &&
                rx_id->text() == QStringLiteral("0x72D"),
            "Target selection did not restore its default diagnostic IDs");
      check_project_devices(application, projects, devices,
                            QStringLiteral("吉利"),
                            {QStringLiteral("P416")});
      check(radar->count() == 1 &&
                radar->currentText() == QStringLiteral("ARS1.31L"),
            "Geely P416 device mapping mismatch");
      check(version_selection->text().contains(QStringLiteral("吉利")) &&
                version_selection->text().contains(QStringLiteral("P416")) &&
                version_selection->text().contains(QStringLiteral("ARS1.31L")) &&
                version_table->rowCount() == 13 &&
                version_table->item(0, 1)->text() == QStringLiteral("F180") &&
                version_table->item(0, 3)->text().contains(
                    QStringLiteral("Boot软件标识")),
            "Version page did not follow P416 or show its DID meanings");
      const auto chery_log_project =
          find_text(projects, QStringLiteral("奇瑞"));
      const auto xizhong_log_project =
          find_text(projects, QStringLiteral("犀重"));
      check(chery_log_project >= 0 && xizhong_log_project >= 0,
            "Projects required by target-scoped log test were not found");
      projects->setCurrentIndex(xizhong_log_project);
      application.processEvents();
      radar->setCurrentIndex(find_text(radar, QStringLiteral("RSMR")));
      application.processEvents();
      const auto xizhong_log_snapshot = log_view->toPlainText();
      check((xizhong_log_snapshot.contains(QStringLiteral("RSMR")) ||
             xizhong_log_snapshot.contains(QStringLiteral("LSMR"))) &&
                !xizhong_log_snapshot.contains(QStringLiteral("ARS1.33")),
            "Switching to Xizhong leaked the Chery target log");
      projects->setCurrentIndex(chery_log_project);
      application.processEvents();
      const auto chery_log_snapshot = log_view->toPlainText();
      check(chery_log_snapshot.contains(QStringLiteral("ARS1.33")) &&
                !log_view->toPlainText().contains(
                    QStringLiteral("RSMR")),
            "Switching to Chery leaked the Xizhong target log");
      projects->setCurrentIndex(xizhong_log_project);
      application.processEvents();
      radar->setCurrentIndex(find_text(radar, QStringLiteral("RSMR")));
      application.processEvents();
      check(log_view->toPlainText().startsWith(xizhong_log_snapshot) &&
                !log_view->toPlainText().contains(QStringLiteral("ARS1.33")),
            "Switching back to Xizhong did not restore only its own log");
      check_project_devices(application, projects, devices,
                             QStringLiteral("奇瑞"),
                             {QStringLiteral("ARS1.33"),
                              QStringLiteral("E0Y"),
                              QStringLiteral("KP31"),
                              QStringLiteral("T1EJ"),
                              QStringLiteral("T22")});
      devices->setCurrentIndex(find_text(devices, QStringLiteral("ARS1.33")));
      application.processEvents();
      check(radar->count() == 2 &&
                radar->currentText() == QStringLiteral("从雷达") &&
                target_id(radar, radar->currentIndex()) ==
                    QStringLiteral("secondary") &&
                !tx_id->isReadOnly() && !rx_id->isReadOnly() &&
                tx_id->text() == QStringLiteral("0x6C4") &&
                rx_id->text() == QStringLiteral("0x6C5"),
            "Chery ARS1.33 project/device mapping mismatch");
      const auto chery_app = entries->findData(QStringLiteral("app"));
      const auto chery_cal = entries->findData(QStringLiteral("cal"));
      const auto chery_app_cal =
          entries->findData(QStringLiteral("app_cal"));
      check(entries->count() == 3 && chery_app >= 0 && chery_cal >= 0 &&
                chery_app_cal >= 0 &&
                entries->itemText(chery_app) ==
                    QStringLiteral("APP") &&
                entries->itemText(chery_cal) ==
                    QStringLiteral("CAL") &&
                entries->itemText(chery_app_cal) ==
                    QStringLiteral("APP+CAL") &&
                entries->currentData().toString() ==
                    QStringLiteral("app_cal"),
            "Chery ARS1.33 concise flashing modes are not exposed");
      radar->setCurrentIndex(find_text(radar, QStringLiteral("主雷达")));
      application.processEvents();
      check(target_id(radar, radar->currentIndex()) ==
                    QStringLiteral("main") &&
                tx_id->text() == QStringLiteral("0x71F") &&
                rx_id->text() == QStringLiteral("0x79F"),
            "Chery ARS1.33 main-radar endpoint mismatch");
      radar->setCurrentIndex(find_text(radar, QStringLiteral("从雷达")));
      application.processEvents();
      check(tx_id->text() == QStringLiteral("0x6C4") &&
                 rx_id->text() == QStringLiteral("0x6C5"),
             "Chery ARS1.33 secondary-radar endpoint was not restored");
      devices->setCurrentIndex(find_text(devices, QStringLiteral("KP31")));
      application.processEvents();
      const auto kp31_app = entries->findData(QStringLiteral("app"));
      const auto kp31_cal = entries->findData(QStringLiteral("cal"));
      const auto kp31_app_cal =
          entries->findData(QStringLiteral("app_cal"));
      check(radar->count() == 1 &&
                radar->currentText() == QStringLiteral("雷达") &&
                target_id(radar, radar->currentIndex()) ==
                    QStringLiteral("radar") &&
                tx_id->isReadOnly() && rx_id->isReadOnly() &&
                tx_id->text() == QStringLiteral("0x70D") &&
                rx_id->text() == QStringLiteral("0x78D") &&
                entries->count() == 3 && kp31_app >= 0 && kp31_cal >= 0 &&
                kp31_app_cal >= 0 &&
                entries->itemText(kp31_app) == QStringLiteral("APP") &&
                entries->itemText(kp31_cal) == QStringLiteral("CAL") &&
                entries->itemText(kp31_app_cal) ==
                    QStringLiteral("APP+CAL") &&
                entries->currentData().toString() == QStringLiteral("app") &&
                entries->currentText() == QStringLiteral("APP"),
            "Chery KP31 project/endpoint/mode mapping mismatch");
      check_file_panel_is_stable();
      check_project_devices(application, projects, devices,
                             QStringLiteral("长马"),
                             {QStringLiteral("J90K")});
      const auto longma_app = entries->findData(QStringLiteral("app"));
      const auto longma_ft = entries->findData(QStringLiteral("ft"));
      check(entries->count() == 2 && longma_app >= 0 && longma_ft >= 0 &&
                entries->itemText(longma_app) ==
                    QStringLiteral("APP") &&
                entries->itemText(longma_ft) ==
                    QStringLiteral("FT") &&
                entries->currentData().toString() == QStringLiteral("app"),
            "Longma APP/FT operation modes are incomplete or changed the APP default");
      check(!radar->isHidden() && !radar_label->isHidden() &&
                radar->count() == 2 &&
                target_id(radar, radar->currentIndex()) ==
                    QStringLiteral("main") &&
                radar->currentText() == QStringLiteral("1.31 主雷达"),
            "Longma J90K device selector did not default to the main radar");
      check(tx_id && rx_id && tx_id->text() == QStringLiteral("0x744") &&
                rx_id->text() == QStringLiteral("0x74C") &&
                !tx_id->isReadOnly() && !rx_id->isReadOnly(),
            "Longma main-radar endpoint was not applied and editable");
      const auto secondary_radar =
          find_target(radar, QStringLiteral("secondary"));
      check(secondary_radar >= 0 &&
                radar->itemText(secondary_radar).contains(
                    QStringLiteral("待验证")),
            "Longma secondary radar is missing its validation warning");
      radar->setCurrentIndex(secondary_radar);
      application.processEvents();
      check(tx_id->text() == QStringLiteral("0x760") &&
                rx_id->text() == QStringLiteral("0x768"),
            "Longma secondary-radar endpoint did not follow the selector");
      check_file_panel_is_stable();
      check_project_devices(application, projects, devices,
                            QStringLiteral("长安"),
                            {QStringLiteral("B216"),
                             QStringLiteral("C857")});
      check(devices->itemText(0) == QStringLiteral("B216") &&
                devices->itemText(1) == QStringLiteral("C857"),
            "Changan projects must be ordered as B216 then C857");
      const auto c857_project = devices->findText(QStringLiteral("C857"));
      check(c857_project >= 0, "Changan C857 project is unavailable");
      devices->setCurrentIndex(c857_project);
      application.processEvents();
      check(!radar->isHidden() && radar->count() == 2 &&
                target_id(radar, radar->currentIndex()) ==
                    QStringLiteral("main") &&
                radar->currentText().contains(QStringLiteral("前雷达 ICRF")) &&
                !radar->currentText().contains(QStringLiteral("待验证")) &&
                tx_id->text() == QStringLiteral("0x744") &&
                rx_id->text() == QStringLiteral("0x74C"),
            "C857 main/front radar selector or endpoint mismatch");
      check(entries->currentData().toString() == QStringLiteral("app") &&
                entries->findData(QStringLiteral("ft")) >= 0 &&
                entries->findData(QStringLiteral("cal")) >= 0 &&
                entries->findData(QStringLiteral("app_cal")) >= 0 &&
                entries->itemText(entries->findData(QStringLiteral("ft")))
                    == QStringLiteral("FT"),
            "C857 operation modes are incomplete or changed the APP default");
      auto* c857_app_path = window.findChild<QLineEdit*>(
          QStringLiteral("appPathLineEdit"));
      auto* c857_cal_path = window.findChild<QLineEdit*>(
          QStringLiteral("calPathLineEdit"));
      auto* c857_seed_key = window.findChild<QLineEdit*>(
          QStringLiteral("seedKeyDllPathLineEdit"));
      check(c857_app_path && c857_cal_path && c857_seed_key &&
                c857_app_path->text().contains(QStringLiteral("C857AF")) &&
                c857_app_path->text().contains(QStringLiteral("CHF0301N")) &&
                c857_seed_key->text() == QStringLiteral("SeedKey_Main.dll") &&
                c857_cal_path->text() ==
                    QStringLiteral("ICRF_002_003.s19"),
            "C857 main radar APP/CAL/SeedKey resources mismatch");
      const auto c857_secondary =
          find_target(radar, QStringLiteral("secondary"));
      check(c857_secondary >= 0 &&
                radar->itemText(c857_secondary).contains(
                    QStringLiteral("后雷达 ICRR")) &&
                !radar->itemText(c857_secondary).contains(
                    QStringLiteral("待验证")),
            "C857 successful secondary/rear radar option mismatch");
      radar->setCurrentIndex(c857_secondary);
      application.processEvents();
      check(tx_id->text() == QStringLiteral("0x760") &&
                rx_id->text() == QStringLiteral("0x768") &&
                c857_app_path->text().contains(QStringLiteral("C857AR")) &&
                c857_app_path->text().contains(QStringLiteral("CHF0303N")) &&
                c857_seed_key->text() == QStringLiteral("SeedKey_Slave.dll") &&
                c857_cal_path->text() ==
                    QStringLiteral("ICRR_001_003.s19"),
            "C857 rear radar did not switch endpoint, APP, CAL and SeedKey together");
      check_file_panel_is_stable();
      const auto b216_project = devices->findText(QStringLiteral("B216"));
      check(b216_project >= 0, "Changan B216 project is unavailable");
      devices->setCurrentIndex(b216_project);
      application.processEvents();
      check(radar->count() == 2 &&
                entries->currentData().toString() == QStringLiteral("app") &&
                entries->findData(QStringLiteral("ft")) >= 0 &&
                entries->findData(QStringLiteral("cal")) >= 0 &&
                entries->findData(QStringLiteral("app_cal")) >= 0 &&
                entries->itemText(entries->findData(QStringLiteral("ft"))) ==
                    QStringLiteral("FT") &&
                c857_app_path->property("fullPath").toString().contains(
                    QStringLiteral("lingyao_b216")) &&
                !c857_app_path->property("fullPath").toString().contains(
                    QStringLiteral("changan_c857")),
            "Changan B216 did not use its independent resource directory");
      check_file_panel_is_stable();
      check_project_devices(application, projects, devices,
                            QStringLiteral("犀重"),
                            {QStringLiteral("HQ001")});
      radar->setCurrentIndex(find_text(radar, QStringLiteral("RSMR")));
      application.processEvents();
      check(!radar->isHidden() && !radar_label->isHidden() &&
                radar->count() == 2 &&
                radar->currentText() == QStringLiteral("RSMR") &&
                tx_id->isReadOnly() && rx_id->isReadOnly(),
            "Xizhong locked diagnostic IDs became editable");
      check_file_panel_is_stable();
      check(entries->currentData().toString() == QStringLiteral("app"),
            "Xizhong must default to the passing APP entry");
      check(entries->findData(QStringLiteral("auto")) < 0,
            "Xizhong unsafe automatic APP-to-FT fallback is still offered");
      check(entries->findData(QStringLiteral("ft")) >= 0,
            "Xizhong explicit FT recovery entry is missing");
      auto* app_path = window.findChild<QLineEdit*>(
          QStringLiteral("appPathLineEdit"));
      check(app_path &&
                app_path->text() ==
                    QStringLiteral("RSMR_AA_APP1_V09.13.00.s19") &&
                !app_path->text().contains(QLatin1Char('\\')) &&
                app_path->toolTip().contains(
                    QStringLiteral("RSMR_AA_APP1_V09.13.00.s19")) &&
                app_path->property("fullPath").toString() == app_path->toolTip(),
            "Flash-file field did not hide the path or retain it in the tooltip");
      auto* cal_browse = window.findChild<QPushButton*>(
          QStringLiteral("calBrowseButton"));
      auto* driver_verify_browse = window.findChild<QPushButton*>(
          QStringLiteral("driverVerifyBrowseButton"));
      auto* cal_verify_browse = window.findChild<QPushButton*>(
          QStringLiteral("calVerifyBrowseButton"));
      check(cal_browse && driver_verify_browse && cal_verify_browse &&
                 !cal_browse->isHidden() &&
                 !driver_verify_browse->isHidden() &&
                 !cal_verify_browse->isHidden() &&
                 !driver_browse->isHidden(),
             "Generic file rows changed visibility for the Xizhong profile");
      radar->setCurrentIndex(find_text(radar, QStringLiteral("LSMR")));
      application.processEvents();
      check(radar->currentText() == QStringLiteral("LSMR") &&
                tx_id->text() == QStringLiteral("0x18DAB6F1") &&
                rx_id->text() == QStringLiteral("0x18DAF1B6") &&
                entries->currentData().toString() == QStringLiteral("app") &&
                entries->findData(QStringLiteral("ft")) < 0 && app_path &&
                app_path->text().isEmpty(),
            "Xizhong LSMR profile endpoint or APP-only entry mismatch");
      check_project_devices(application, projects, devices,
                             QStringLiteral("时代新安"),
                             {QStringLiteral("HJZJ"),
                              QStringLiteral("天王星"),
                              QStringLiteral("木星2代"),
                              QStringLiteral("庆铃")});
      auto* driver_path = window.findChild<QLineEdit*>(
          QStringLiteral("driverPathLineEdit"));
      std::cerr
          << "qt_main_window_tests: Shidaixinan UI radarHidden="
          << radar->isHidden() << " txReadOnly=" << tx_id->isReadOnly()
          << " rxReadOnly=" << rx_id->isReadOnly()
          << " tx=" << tx_id->text().toStdString()
          << " rx=" << rx_id->text().toStdString()
          << " entries=" << entries->count()
          << " mode="
          << entries->currentData().toString().toStdString()
          << " driver="
          << (driver_path ? driver_path->text().toStdString() : "<null>")
          << " app=" << app_path->text().toStdString()
          << " seed=" << c857_seed_key->text().toStdString()
          << std::endl;
      check(!radar->isHidden() && !radar_label->isHidden() &&
                radar->count() == 1 &&
                radar->currentText() == QStringLiteral("FMR 主雷达") &&
                tx_id->isReadOnly() && rx_id->isReadOnly() &&
                tx_id->text() == QStringLiteral("0x7A4") &&
                rx_id->text() == QStringLiteral("0x7AC") &&
                entries->count() == 2 &&
                entries->currentData().toString() == QStringLiteral("app") &&
                entries->findData(QStringLiteral("ft")) >= 0 &&
                entries->itemText(
                    entries->findData(QStringLiteral("ft"))) ==
                    QStringLiteral("FT") &&
                driver_path &&
                driver_path->text() ==
                    QStringLiteral("ARF2_32_ERadar_FlashDrv.s19") &&
                app_path->text().contains(QStringLiteral("without_boot")) &&
                c857_seed_key->text() == QStringLiteral("FMR.dll"),
             "Shidaixinan FMR UI endpoint, mode or resources mismatch");
      check_file_panel_is_stable();
      for (const auto& project_name :
           {QStringLiteral("天王星"), QStringLiteral("木星2代"),
            QStringLiteral("庆铃")}) {
        const auto project_index = devices->findText(project_name);
        check(project_index >= 0,
              "Shidaixinan ARF2.32 project is missing from the UI");
        devices->setCurrentIndex(project_index);
        application.processEvents();
        check(!radar->isHidden() && radar->count() == 1 &&
                  radar->currentText() == QStringLiteral("FMR") &&
                  tx_id->isReadOnly() && rx_id->isReadOnly() &&
                  tx_id->text() == QStringLiteral("0x7A4") &&
                  rx_id->text() == QStringLiteral("0x7AC") &&
                  entries->count() == 2 &&
                  entries->currentData().toString() ==
                      QStringLiteral("app") &&
                  entries->findData(QStringLiteral("ft")) >= 0 &&
                  driver_path->text() ==
                      QStringLiteral("ARF2_32_ERadar_FlashDrv.s19") &&
                  app_path->text().isEmpty() &&
                  c857_seed_key->text() == QStringLiteral("FMR.dll"),
              "Shidaixinan ARF2.32 project did not keep the shared flow "
              "contract or fail-closed APP selection");
        check_file_panel_is_stable();
      }
      check_project_devices(application, projects, devices,
                             QStringLiteral("零跑"),
                             {QStringLiteral("A12"),
                              QStringLiteral("ARC"),
                              QStringLiteral("ARF631"),
                              QStringLiteral("B11")});
      devices->setCurrentIndex(find_text(devices, QStringLiteral("ARC")));
      application.processEvents();
      auto* app_verify_label = window.findChild<QLabel*>(
          QStringLiteral("appVerifyPathLabel"));
      check(!radar->isHidden() && radar->count() == 4 &&
                radar->itemText(0) == QStringLiteral("设备 0（0x772 / 0x77A）") &&
                radar->itemText(1) == QStringLiteral("设备 1（0x773 / 0x77B）") &&
                radar->itemText(2) == QStringLiteral("设备 2（0x771 / 0x779）") &&
                radar->itemText(3) == QStringLiteral("设备 3（0x770 / 0x778）") &&
                tx_id->text() == QStringLiteral("0x772") &&
                rx_id->text() == QStringLiteral("0x77A") &&
                radar->isEnabled() &&
                entries->isEnabled() && entries->count() == 2 &&
                entries->itemText(entries->findData(QStringLiteral("app"))) ==
                    QStringLiteral("APP") &&
                entries->itemText(entries->findData(QStringLiteral("ft"))) ==
                    QStringLiteral("FT") &&
                window.findChild<QPushButton*>(
                    QStringLiteral("driverBrowseButton"))->isEnabled() &&
                window.findChild<QPushButton*>(
                    QStringLiteral("appBrowseButton"))->isEnabled() &&
                window.findChild<QPushButton*>(
                    QStringLiteral("versionCheckButton"))->isEnabled() &&
                driver_path->text() ==
                    QStringLiteral("FlashDriver.srec") &&
                app_path->text().contains(QStringLiteral("ARC2.36BC3")) &&
                c857_seed_key->text().contains(
                    QStringLiteral("lingpao_SeednKey")) &&
                app_verify_label &&
                app_verify_label->text() ==
                    QStringLiteral("APP 校验文件"),
            "ARC merged four-target UI or preset resources mismatch");
      const std::array<std::pair<QString, QString>, 4> arc_endpoints{{
          {QStringLiteral("0x772"), QStringLiteral("0x77A")},
          {QStringLiteral("0x773"), QStringLiteral("0x77B")},
          {QStringLiteral("0x771"), QStringLiteral("0x779")},
          {QStringLiteral("0x770"), QStringLiteral("0x778")},
      }};
      for (int index = 0; index < radar->count(); ++index) {
        radar->setCurrentIndex(index);
        application.processEvents();
        check(tx_id->text() == arc_endpoints[static_cast<std::size_t>(index)].first &&
                  rx_id->text() == arc_endpoints[static_cast<std::size_t>(index)].second,
              "ARC target selection did not lock the expected endpoint");
      }
      entries->setCurrentIndex(entries->findData(QStringLiteral("ft")));
      application.processEvents();
      check(entries->currentData().toString() == QStringLiteral("ft") &&
                entries->currentText() == QStringLiteral("FT"),
            "ARC FT entry selection mismatch");
      entries->setCurrentIndex(entries->findData(QStringLiteral("app")));
      application.processEvents();
      check_file_panel_is_stable();
      devices->setCurrentIndex(find_text(devices, QStringLiteral("ARF631")));
      application.processEvents();
      check(!radar->isHidden() && radar->count() == 1 &&
                radar->currentText() == QStringLiteral("LP-ARF") &&
                tx_id->isReadOnly() &&
                 rx_id->isReadOnly() &&
                tx_id->text() == QStringLiteral("0x751") &&
                rx_id->text() == QStringLiteral("0x759") &&
                entries->count() == 2 &&
                entries->currentData().toString() ==
                    QStringLiteral("app") &&
                 entries->itemText(
                     entries->findData(QStringLiteral("app"))) ==
                     QStringLiteral("APP") &&
                 entries->itemText(
                     entries->findData(QStringLiteral("ft"))) ==
                     QStringLiteral("FT") &&
                driver_path->text().isEmpty() &&
                app_path->text().contains(QStringLiteral("ARF6.31")) &&
                c857_seed_key->text().contains(
                    QStringLiteral("lingpao_SeednKey")) &&
                app_verify_label &&
                app_verify_label->text() ==
                    QStringLiteral("APP 校验文件"),
            "LP-ARF UI endpoint, entry names or resources mismatch");
      check_file_panel_is_stable();
      for (const auto& project_name :
           {QStringLiteral("A12"), QStringLiteral("B11")}) {
        devices->setCurrentIndex(find_text(devices, project_name));
        application.processEvents();
        check(radar->count() == 1 && tx_id->isReadOnly() &&
                  rx_id->isReadOnly() &&
                  tx_id->text() == QStringLiteral("0x751") &&
                  rx_id->text() == QStringLiteral("0x759") &&
                  entries->count() == 2 &&
                  entries->itemText(entries->findData(QStringLiteral("app"))) ==
                      QStringLiteral("APP") &&
                  entries->itemText(entries->findData(QStringLiteral("ft"))) ==
                      QStringLiteral("PLS") &&
                  app_path->text().contains(project_name) &&
                  window.findChild<QLineEdit*>(
                      QStringLiteral("appVerifyPathLineEdit"))
                      ->text()
                      .endsWith(QStringLiteral(".tmp"),
                                Qt::CaseInsensitive) &&
                  c857_seed_key->text().contains(
                      QStringLiteral("SeednKey"), Qt::CaseInsensitive),
              "A12/B11 ARF2.31 independent UI resources or entry contract mismatch");
      }

      checkpoint("profile-ui");
      run_ui_monkey_test(application, window, projects, devices, entries,
                         radar, channels, repeat_count, workspace_tabs,
                         backend_actions);
      checkpoint("monkey");

      check(channels->count() == 4, "Qt channel selector count mismatch");
      for (int index = 0; index < channels->count(); ++index) {
        check(channels->itemData(index).toUInt() ==
                  static_cast<unsigned>(index + 1),
              "Qt channel selector is not one-based");
      }

      const auto chuneng_index = find_text(projects, QStringLiteral("楚能"));
      check(chuneng_index >= 0, "Chuneng project was not found");
      projects->setCurrentIndex(chuneng_index);
      application.processEvents();
      check(devices->count() > 0, "Chuneng device selector is empty");
      check_file_panel_is_stable();

      const auto channel_four = channels->findData(4U);
      const auto ft_entry = entries->findData(QStringLiteral("ft"));
      check(channel_four >= 0 && ft_entry >= 0,
            "Chuneng persisted selector choices are unavailable");
      channels->setCurrentIndex(channel_four);
      entries->setCurrentIndex(ft_entry);
      repeat_count->setValue(3);
      application.processEvents();
      tosun_backend->trigger();
      application.processEvents();
      check(channels->currentData().toUInt() == 1U,
            "TOSUN did not start from its independent CH1 default");
      check(version_address->text().contains(QStringLiteral("TOSUN")) &&
                version_address->text().contains(QStringLiteral("CH1")),
            "Version page did not follow the restored TOSUN backend/channel");
      check(bus_monitor_page->matchesContext(
                uds::CanVendor::Tosun, 1U, 500000U, 2000000U, true),
            "Bus monitor did not follow the restored TOSUN backend/channel");
      check(bus_monitor_context->text().contains(QStringLiteral("TOSUN")) &&
                bus_monitor_context->text().contains(QStringLiteral("CH1")),
            "Bus monitor did not display the restored TOSUN backend/channel");
      channels->setCurrentIndex(channels->findData(3U));
      vector_backend->trigger();
      application.processEvents();
      channels->setCurrentIndex(channels->findData(2U));
      zlg_backend->trigger();
      application.processEvents();
      check(channels->currentData().toUInt() == 4U,
            "Switching CAN drivers did not restore the ZLG channel");
      check(version_address->text().contains(QStringLiteral("ZLG")) &&
                version_address->text().contains(QStringLiteral("CH4")),
            "Version page retained a stale backend/channel after switching to ZLG");
      check(bus_monitor_page->matchesContext(
                uds::CanVendor::Zlg, 4U, 500000U, 2000000U, true),
            "Bus monitor retained the previous backend/channel after switching to ZLG");
      check(bus_monitor_context->text().contains(QStringLiteral("ZLG")) &&
                bus_monitor_context->text().contains(QStringLiteral("CH4")),
            "Bus monitor did not display the restored ZLG backend/channel");
      clear_log->trigger();
      check(log_view->toPlainText().isEmpty(),
            "Clear-log action did not clear the current display");
    }
    checkpoint("first-window-destroyed");

    settings.sync();
    check(settings.value(QStringLiteral("selectors/vendor")).toString() ==
                  QStringLiteral("楚能") &&
              settings.value(QStringLiteral("selectors/project_name"))
                      .toString() == QStringLiteral("ARC331"),
          "Qt vendor/project hierarchy was not saved");
    check(settings.value(QStringLiteral("selectors/project")).toString() ==
              QStringLiteral("楚能"),
          "Qt legacy vendor selection alias was not saved");
    check(settings.value(QStringLiteral("selectors/profile_id")).toString() ==
              QStringLiteral("chuneng_331_left_rear"),
          "Qt device selection was not saved by stable profile id");
    check(settings.value(QStringLiteral("hardware/channel/zlg")).toUInt() ==
              4U,
          "Qt ZLG channel selection was not saved independently");
    check(settings.value(QStringLiteral("hardware/channel/tosun")).toUInt() ==
                  3U &&
              settings.value(QStringLiteral("hardware/channel/vector"))
                      .toUInt() == 2U,
          "Qt channels leaked between CAN driver backends");
    check(settings.value(QStringLiteral("selectors/entry_mode")).toString() ==
              QStringLiteral("ft"),
          "Qt entry selection was not saved");
    check(settings.value(QStringLiteral("selectors/repeat_count")).toInt() == 3,
          "Qt generic flash repeat count was not saved");
    check(settings.value(QStringLiteral(
                             "selectors/target/longma_ars1_31"))
                  .toString() ==
              QStringLiteral("secondary"),
          "Longma radar selection was not saved");
    check(settings.value(QStringLiteral(
                             "selectors/target/changan_c857"))
                  .toString() ==
              QStringLiteral("secondary"),
          "C857 radar selection was not saved");
    checkpoint("settings-verified");

    {
      uds::ui::qt::MainWindow restored;
      auto* restored_zlg = restored.findChild<QAction*>(
          QStringLiteral("zlgCanBackendAction"));
      auto* projects = restored.findChild<QComboBox*>(
          QStringLiteral("projectComboBox"));
      auto* entries = restored.findChild<QComboBox*>(
          QStringLiteral("entryModeComboBox"));
      auto* channels = restored.findChild<QComboBox*>(
          QStringLiteral("vectorChannelComboBox"));
      auto* repeat_count = restored.findChild<QSpinBox*>(
          QStringLiteral("repeatCountSpinBox"));
      check(projects && entries && channels && repeat_count && restored_zlg &&
                restored_zlg->isChecked(),
            "Restored Qt selectors missing");
      check(projects->currentText() == QStringLiteral("楚能"),
            "Qt project selection was not restored");
      check(channels->currentData().toUInt() == 4U,
            "Qt ZLG channel selection was not restored");
      check(entries->currentData().toString() == QStringLiteral("ft"),
            "Qt entry selection was not restored");
      check(repeat_count->value() == 3,
            "Qt flash repeat count was not restored");
    }
    checkpoint("restored-window-destroyed");

    settings.setValue(QStringLiteral("selectors/project"),
                      QStringLiteral("铃耀_B216"));
    settings.setValue(QStringLiteral("selectors/profile_id"),
                      QStringLiteral("lingyao_b216"));
    settings.sync();
    {
      uds::ui::qt::MainWindow migrated;
      auto* projects = migrated.findChild<QComboBox*>(
          QStringLiteral("projectComboBox"));
      auto* devices = migrated.findChild<QComboBox*>(
          QStringLiteral("deviceComboBox"));
      check(projects && devices &&
                projects->currentText() == QStringLiteral("长安") &&
                devices->currentText() == QStringLiteral("B216"),
            "Legacy Lingyao B216 selection was not migrated to Changan/B216");
    }

    settings.setValue(QStringLiteral("selectors/project"),
                      QStringLiteral("长安C857"));
    settings.setValue(QStringLiteral("selectors/profile_id"),
                      QStringLiteral("changan_c857"));
    settings.sync();
    {
      uds::ui::qt::MainWindow migrated;
      auto* projects = migrated.findChild<QComboBox*>(
          QStringLiteral("projectComboBox"));
      auto* devices = migrated.findChild<QComboBox*>(
          QStringLiteral("deviceComboBox"));
      check(projects && devices &&
                projects->currentText() == QStringLiteral("长安") &&
                devices->currentText() == QStringLiteral("C857"),
            "Legacy C857 selection was not migrated to Changan/C857");
    }

    settings.clear();
    settings.sync();
    QDir(settings_path).removeRecursively();
    std::cout << "qt_main_window_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "qt_main_window_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
