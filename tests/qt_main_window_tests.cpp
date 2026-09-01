#include "app/flash_request.hpp"
#include "ui/qt/main_window.hpp"
#include "ui/qt/main_window_support.hpp"
#include "ui/qt/startup_window_presenter.hpp"
#include "ui/qt/bus_monitor_page.hpp"
#include "ui/qt/controller_bridge.hpp"
#include "ui/qt/resource_file_store.hpp"
#include "ui/qt/ui_log_message_parser.hpp"
#include "ui/qt/version_confirmation_page.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
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
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include <QUrl>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <random>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void check_main_window_support_contracts() {
  using namespace uds::ui::qt::main_window_support;
  const auto nrc = nrcFromLogLine(QStringLiteral("RX [0x761] 7F 22 31"));
  check(nrc && *nrc == 0x31,
        "split main-window support failed to parse a raw NRC");
  check(!nrcFromLogLine(
             QStringLiteral("RX [0x761] 62 F1 89 7F 22 31")),
        "split main-window support scanned an NRC inside positive payload");

  const auto failed = failedRoutineFromLogLine(
      QStringLiteral("RX [0x761] 71 01 02 04 05"));
  check(failed && failed->routine_id == 0x0204 && failed->status == 0x05,
        "split main-window support lost failed-routine detection");
  check(!failedRoutineFromLogLine(
             QStringLiteral("RX [0x761] 71 01 02 03 05")),
        "split main-window support changed the ARC331 0203 tolerance");

  check(canVendorFromKey(canVendorKey(uds::CanVendor::Tosun)) ==
                uds::CanVendor::Tosun &&
            canChannelSettingsKey(uds::CanVendor::Zlg) ==
                QStringLiteral("hardware/channel/zlg"),
        "split main-window support changed CAN backend settings keys");
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

QTextBlock find_log_block(const QTextDocument* document,
                          const QString& needle) {
  for (auto block = document->begin(); block.isValid(); block = block.next()) {
    if (block.text().contains(needle)) return block;
  }
  return {};
}

QTextCharFormat log_fragment_format(const QTextBlock& block,
                                    const QString& needle) {
  for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
    const auto fragment = iterator.fragment();
    if (fragment.isValid() && fragment.text().contains(needle)) {
      return fragment.charFormat();
    }
  }
  return {};
}

int log_fragment_position(const QTextBlock& block, const QString& needle) {
  for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
    const auto fragment = iterator.fragment();
    const auto offset = fragment.text().indexOf(needle);
    if (fragment.isValid() && offset >= 0) {
      return fragment.position() + offset;
    }
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
    check_main_window_support_contracts();
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("UDSToolsTests"));
    QApplication::setApplicationName(QStringLiteral("uds_tool_qt_state_test"));

    {
      QWidget startup_window;
      startup_window.setWindowTitle(QStringLiteral("startup presentation test"));
      uds::ui::qt::presentWindowOnStartup(startup_window);
      application.processEvents();
      check(startup_window.isVisible(),
            "startup presenter must show the requested window");
      check((startup_window.windowFlags() & Qt::WindowStaysOnTopHint) == 0,
            "startup presenter must not leave the window always on top");
#ifdef Q_OS_WIN
      const auto startup_handle =
          reinterpret_cast<HWND>(startup_window.winId());
      const auto extended_style =
          GetWindowLongPtr(startup_handle, GWL_EXSTYLE);
      check((extended_style & WS_EX_TOPMOST) == 0,
            "startup presenter must clear the native topmost state");
#endif
      startup_window.close();
    }
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
      QTemporaryDir sandbox;
      check(sandbox.isValid(), "Resource replacement sandbox unavailable");
      const auto resources = QDir(sandbox.path()).filePath("resources/demo/APP");
      check(QDir().mkpath(resources), "Resource replacement directory missing");
      const auto selected = QDir(sandbox.path()).filePath("external/new.s19");
      check(QDir().mkpath(QFileInfo(selected).absolutePath()),
            "Resource replacement input directory missing");
      QFile input(selected);
      check(input.open(QIODevice::WriteOnly) &&
                input.write("NEW_RESOURCE", 12) == 12,
            "Resource replacement input write failed");
      input.close();
      const auto configured = QDir(resources).filePath("default.s19");
      QFile old(configured);
      check(old.open(QIODevice::WriteOnly) && old.write("OLD", 3) == 3,
            "Resource replacement default write failed");
      old.close();
      const auto preserved_tmp = QDir(resources).filePath("package.tmp");
      QFile tmp_file(preserved_tmp);
      check(tmp_file.open(QIODevice::WriteOnly) &&
                tmp_file.write("KEEP_TMP", 8) == 8,
            "Different-suffix resource setup failed");
      tmp_file.close();
      const auto unrelated_s19 = QDir(resources).filePath("other-field.s19");
      QFile unrelated_file(unrelated_s19);
      check(unrelated_file.open(QIODevice::WriteOnly) &&
                unrelated_file.write("KEEP_OTHER_FIELD", 16) == 16,
            "Unrelated same-suffix resource setup failed");
      unrelated_file.close();
      const auto replaced = uds::ui::qt::replaceConfiguredResourceFile(
          selected, configured, QDir(sandbox.path()).filePath("resources"));
      const auto stored_path = QDir(resources).filePath("new.s19");
      QFile stored(stored_path);
      check(replaced.success && stored.open(QIODevice::ReadOnly) &&
                stored.readAll() == QByteArray("NEW_RESOURCE", 12) &&
                QFileInfo(replaced.stored_path).fileName() == "new.s19" &&
                QFileInfo::exists(configured) && QFileInfo::exists(selected) &&
                QFileInfo::exists(preserved_tmp) &&
                QFileInfo::exists(unrelated_s19),
            "Replacement removed an existing resource file");
      stored.close();

      const auto next_selected =
          QDir(sandbox.path()).filePath("external/next.s19");
      QFile next_input(next_selected);
      check(next_input.open(QIODevice::WriteOnly) &&
                next_input.write("NEXT_RESOURCE", 13) == 13,
            "Second resource replacement input write failed");
      next_input.close();
      const auto next = uds::ui::qt::replaceConfiguredResourceFile(
          next_selected, configured,
          QDir(sandbox.path()).filePath("resources"));
      QFile next_stored(QDir(resources).filePath("next.s19"));
      check(next.success && next_stored.open(QIODevice::ReadOnly) &&
                next_stored.readAll() == QByteArray("NEXT_RESOURCE", 13) &&
                QFileInfo::exists(configured) &&
                QFileInfo::exists(stored_path) &&
                QFileInfo::exists(next_selected),
            "Second selection removed a previous resource file");
      next_stored.close();

      check(QFileInfo::exists(preserved_tmp) &&
                QFileInfo::exists(unrelated_s19),
            "Second replacement removed an unrelated resource file");

      const auto application_directory =
          QDir(sandbox.path()).filePath("portable-dist");
      const auto portable_driver = QDir(application_directory).filePath(
          "resources/lp_arc/Driver/FlashDriver.srec");
      check(QDir().mkpath(QFileInfo(portable_driver).absolutePath()),
            "Portable resource directory setup failed");
      QFile portable_file(portable_driver);
      check(portable_file.open(QIODevice::WriteOnly) &&
                portable_file.write("DRIVER", 6) == 6,
            "Portable resource setup failed");
      portable_file.close();

      const auto expected_relative = QDir::toNativeSeparators(
          QStringLiteral("resources/lp_arc/Driver/FlashDriver.srec"));
      check(uds::ui::qt::resourcePathForPersistence(
                portable_driver, application_directory) == expected_relative,
            "Managed resource path was not persisted relative to the EXE");

      const auto relative_resolution =
          uds::ui::qt::resolvePersistedResourcePath(
              expected_relative, application_directory);
      check(QFileInfo(relative_resolution.absolute_path).absoluteFilePath() ==
                    QFileInfo(portable_driver).absoluteFilePath() &&
                relative_resolution.persisted_path == expected_relative,
            "Portable resource path did not follow the current EXE directory");

      const auto legacy_driver = QStringLiteral(
          "D:/old/UDS_tools/build/legacy-dist/resources/lp_arc/Driver/"
          "FlashDriver.srec");
      const auto legacy_resolution =
          uds::ui::qt::resolvePersistedResourcePath(
              legacy_driver, application_directory);
      check(legacy_resolution.migrated &&
                QFileInfo(legacy_resolution.absolute_path).absoluteFilePath() ==
                    QFileInfo(portable_driver).absoluteFilePath() &&
                legacy_resolution.persisted_path == expected_relative,
            "Legacy absolute resource path was not migrated to the current EXE");

      const auto external_path =
          QDir(sandbox.path()).filePath("external/keep-absolute.s19");
      check(uds::ui::qt::resourcePathForPersistence(
                external_path, application_directory) ==
                QFileInfo(external_path).absoluteFilePath(),
            "External resource path unexpectedly changed semantics");

      const auto missing_legacy = QStringLiteral(
          "D:/old/UDS_tools/dist/resources/missing/APP/missing.s19");
      const auto missing_resolution =
          uds::ui::qt::resolvePersistedResourcePath(
              missing_legacy, application_directory);
      check(!missing_resolution.migrated &&
                missing_resolution.absolute_path ==
                    QDir::cleanPath(missing_legacy),
            "Missing legacy resource was silently redirected");
    }

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
                vector_backend->text() == QStringLiteral("Vector") &&
                zlg_backend->text() == QStringLiteral("ZLG") &&
                tosun_backend->text() == QStringLiteral("TOSUN") &&
                kvaser_backend->text() == QStringLiteral("Kvaser") &&
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
      auto* tx_id_label =
          window.findChild<QLabel*>(QStringLiteral("txIdLabel"));
      auto* rx_id_label =
          window.findChild<QLabel*>(QStringLiteral("rxIdLabel"));
      auto* repeat_count = window.findChild<QSpinBox*>(
          QStringLiteral("repeatCountSpinBox"));
      auto* update_public_key = window.findChild<QCheckBox*>(
          QStringLiteral("updatePublicKeyCheckBox"));
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
                 !window.findChild<QLabel*>(
                     QStringLiteral("versionSummaryLabel")) &&
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
                 bus_monitor_export->text() == QStringLiteral("导出 BLF") &&
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
      const auto check_file_panel_is_stable =
          [&window](const bool allow_embedded_verification = false,
                    const bool allow_optional_verification = false) {
        const auto* driver_label =
            window.findChild<QLabel*>(QStringLiteral("driverPathLabel"));
        const auto* app_verify_label = window.findChild<QLabel*>(
            QStringLiteral("appVerifyPathLabel"));
        const auto* app_verify_path = window.findChild<QLineEdit*>(
            QStringLiteral("appVerifyPathLineEdit"));
        check(driver_label &&
                  driver_label->text() == QStringLiteral("Driver 文件") &&
                  app_verify_label &&
                  (app_verify_label->text() ==
                       QStringLiteral("APP 校验文件") ||
                   (allow_embedded_verification &&
                    app_verify_label->text() ==
                        QStringLiteral("APP 验签（内置）") &&
                    app_verify_path->property("embeddedVerification")
                        .toBool()) ||
                   (allow_optional_verification &&
                    app_verify_label->text() ==
                        QStringLiteral("APP 验签文件（可选）"))) &&
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
      auto* bridge = window.findChild<uds::ui::qt::ControllerBridge*>();
      check(bridge, "Controller bridge missing for TX/RX color test");
      const auto parsed_prefixed = uds::ui::qt::parseUiLogMessage(
          QStringLiteral("[设备1] [第2/3次] TX [0x72E] 36 A5 FF"));
      check(parsed_prefixed.direction == uds::ui::qt::LogDirection::Tx &&
                parsed_prefixed.leadingPrefix ==
                    QStringLiteral("[设备1] [第2/3次]") &&
                parsed_prefixed.roundPrefix == QStringLiteral("[第2/3次]") &&
                parsed_prefixed.directionAndCanId ==
                    QStringLiteral("TX [0x72E]") &&
                parsed_prefixed.payload == QStringLiteral("36 A5 FF"),
            "Structured UI log parser lost a device/round prefix or TX fields");
      const auto parsed_stage_rx = uds::ui::qt::parseUiLogMessage(
          QStringLiteral("   [Driver下载] [第1/3次] RX [0x72F] 76 FF"));
      check(parsed_stage_rx.direction == uds::ui::qt::LogDirection::Rx &&
                parsed_stage_rx.leadingPrefix ==
                    QStringLiteral("[Driver下载] [第1/3次]") &&
                parsed_stage_rx.roundPrefix == QStringLiteral("[第1/3次]") &&
                parsed_stage_rx.directionAndCanId ==
                    QStringLiteral("RX [0x72F]") &&
                parsed_stage_rx.payload == QStringLiteral("76 FF"),
            "Structured UI log parser lost a stage/round prefix or RX fields");
      check(uds::ui::qt::parseUiLogMessage(
                QStringLiteral("说明文字：TX=0x72E；RX=0x72F"))
                    .direction == uds::ui::qt::LogDirection::None &&
                uds::ui::qt::parseUiLogMessage(
                    QStringLiteral("D:/reports/TX [0x72E]/latest.html"))
                    .direction == uds::ui::qt::LogDirection::None,
            "Structured UI log parser misclassified descriptive prose");
      const auto can_probe_summary = uds::ui::qt::summarizeProbeUiLog(
          QStringLiteral("PASS：CAN硬件物理CH2 已打开（后端：Vector XL）"),
          QStringLiteral("CAN已打开：Vector XL，CH2，TX 0x7E2，RX 0x72F"));
      const auto wire_probe_summary = uds::ui::qt::summarizeProbeUiLog(
          QStringLiteral("RX [0x72F] 71 01 02 03 05"), QString{});
      const auto warning_probe_summary = uds::ui::qt::summarizeProbeUiLog(
          QStringLiteral(
              "WARN：楚能ARC331 APP刷新入口可用，但刷新条件状态为0x05；正式流程将按项目参考策略继续并保留原始响应。"),
          QString{});
      const auto hidden_probe_summary = uds::ui::qt::summarizeProbeUiLog(
          QStringLiteral("ASC原始总线日志：D:/probe/trace.asc"), QString{});
      check(can_probe_summary.kind ==
                    uds::ui::qt::ProbeUiLogKind::CanOpened &&
                can_probe_summary.message == QStringLiteral(
                    "CAN已打开：Vector XL，CH2，TX 0x7E2，RX 0x72F") &&
                wire_probe_summary.kind ==
                    uds::ui::qt::ProbeUiLogKind::WireMessage &&
                warning_probe_summary.kind ==
                    uds::ui::qt::ProbeUiLogKind::RefreshWarning &&
                warning_probe_summary.message.contains(
                    QStringLiteral("刷新条件状态 0x05")) &&
                hidden_probe_summary.kind ==
                    uds::ui::qt::ProbeUiLogKind::Hidden,
            "Online-probe UI summary did not retain only CAN, wire, and refresh-warning messages");
      const auto flash_qualification =
          uds::ui::qt::summarizeFlashPreparationUiLog(QStringLiteral(
              "Pre-flash qualification: Status=PASS; Completed at=2026-08-31T18:18:41.168"));
      const auto flash_can =
          uds::ui::qt::summarizeFlashPreparationUiLog(QStringLiteral(
              "CAN configuration: Hardware backend=Vector XL; Channel=2; Nominal bitrate=500000 bit/s; Data bitrate=2000000 bit/s; CAN FD=yes; Padding=0x55"));
      const auto flash_driver =
          uds::ui::qt::summarizeFlashPreparationUiLog(QStringLiteral(
              "Flash file: Boot Driver=D:/full/driver.cbf; exists=yes; size=17322 bytes"));
      const auto flash_hidden =
          uds::ui::qt::summarizeFlashPreparationUiLog(QStringLiteral(
              "Driver CBF identity: target=7052A5023002AB; software_id=7052A5023002"));
      check(flash_qualification.kind ==
                    uds::ui::qt::FlashPreparationUiLogKind::Qualification &&
                flash_qualification.message ==
                    QStringLiteral("刷写前条件检查：通过") &&
                flash_can.message == QStringLiteral(
                    "CAN配置：Vector XL，CH2，500K/2M，CAN FD") &&
                flash_driver.message == QStringLiteral(
                    "Driver文件检查：已找到（17,322 bytes）") &&
                flash_hidden.kind ==
                    uds::ui::qt::FlashPreparationUiLogKind::Hidden,
            "Flash-preparation UI summary did not compact qualification, CAN, file, or CBF details");

      bridge->logMessage(QStringLiteral("TX [0x716] 34 00 44"));
      bridge->logMessage(QStringLiteral("RX [0x616] 74 20 08 00"));
      bridge->logMessage(
          QStringLiteral("[第2/3次] TX [0x72E] 36 A5 FF"));
      bridge->logMessage(
          QStringLiteral("[设备1] [第2/3次] RX [0x72F] 76 A5"));
      bridge->logMessage(
          QStringLiteral("   [Driver下载] [第1/3次] RX [0x72F] 76 FF"));
      bridge->logMessage(
          QStringLiteral("[第2/3次] RX [0x72F] 7F 10 7E"));
      bridge->logMessage(
          QStringLiteral("说明文字：TX=0x72E；RX=0x72F"));
      bridge->logMessage(
          QStringLiteral("D:/reports/TX [0x72E]/latest.html"));
      application.processEvents();
      const auto tx_block =
          find_log_block(log_view->document(), QStringLiteral("34 00 44"));
      const auto rx_block =
          find_log_block(log_view->document(), QStringLiteral("74 20 08 00"));
      const auto round_tx_block =
          find_log_block(log_view->document(), QStringLiteral("36 A5 FF"));
      const auto device_rx_block =
          find_log_block(log_view->document(), QStringLiteral("76 A5"));
      const auto stage_rx_block =
          find_log_block(log_view->document(), QStringLiteral("76 FF"));
      const auto negative_rx_block = find_log_block(
          log_view->document(), QStringLiteral("7F 10 7E"));
      const auto prose_block = find_log_block(
          log_view->document(), QStringLiteral("说明文字：TX=0x72E"));
      const auto path_block = find_log_block(
          log_view->document(), QStringLiteral("D:/reports/TX"));
      check(tx_block.isValid() && rx_block.isValid() &&
                 round_tx_block.isValid() && device_rx_block.isValid() &&
                 stage_rx_block.isValid() && negative_rx_block.isValid() &&
                 prose_block.isValid() && path_block.isValid(),
             "TX/RX rich-text test messages are missing");

      const auto timestamp_format =
          log_fragment_format(tx_block, tx_block.text().left(10));
      const auto tx_format =
          log_fragment_format(tx_block, QStringLiteral("TX [0x716]"));
      const auto rx_format =
          log_fragment_format(rx_block, QStringLiteral("RX [0x616]"));
      const auto tx_payload_format =
          log_fragment_format(tx_block, QStringLiteral("34 00 44"));
      const auto rx_payload_format =
          log_fragment_format(rx_block, QStringLiteral("74 20 08 00"));
      const auto round_format =
          log_fragment_format(round_tx_block, QStringLiteral("[第2/3次]"));
      const auto round_tx_format = log_fragment_format(
          round_tx_block, QStringLiteral("TX [0x72E]"));
      const auto device_rx_format = log_fragment_format(
          device_rx_block, QStringLiteral("RX [0x72F]"));
      const auto stage_rx_format = log_fragment_format(
          stage_rx_block, QStringLiteral("RX [0x72F]"));
      const auto stage_rx_payload_format =
          log_fragment_format(stage_rx_block, QStringLiteral("76 FF"));
      const auto negative_direction_format = log_fragment_format(
          negative_rx_block, QStringLiteral("RX [0x72F]"));
      const auto negative_payload_format = log_fragment_format(
          negative_rx_block, QStringLiteral("7F 10 7E"));
      const auto prose_format = log_fragment_format(
          prose_block, QStringLiteral("说明文字：TX=0x72E"));
      const auto path_format = log_fragment_format(
          path_block, QStringLiteral("D:/reports/TX"));

      check(timestamp_format.foreground().color() ==
                    QColor(QStringLiteral("#70757A")) &&
                tx_format.foreground().color() ==
                    QColor(QStringLiteral("#1565C0")) &&
                tx_format.fontWeight() == QFont::Bold &&
                rx_format.foreground().color() ==
                    QColor(QStringLiteral("#8E24AA")) &&
                rx_format.fontWeight() == QFont::Bold &&
                tx_payload_format.foreground().color() ==
                    QColor(QStringLiteral("#1565C0")) &&
                tx_payload_format.fontWeight() == QFont::Normal &&
                rx_payload_format.foreground().color() ==
                    QColor(QStringLiteral("#8E24AA")) &&
                rx_payload_format.fontWeight() == QFont::Normal,
            "Plain TX/RX direction, CAN ID, or payload colors are incorrect");
      check(round_format.foreground().color() ==
                    QColor(QStringLiteral("#70757A")) &&
                round_tx_format.foreground().color() ==
                    QColor(QStringLiteral("#1565C0")) &&
                device_rx_format.foreground().color() ==
                    QColor(QStringLiteral("#8E24AA")) &&
                stage_rx_format.foreground().color() ==
                    QColor(QStringLiteral("#8E24AA")) &&
                stage_rx_payload_format.foreground().color() ==
                    QColor(QStringLiteral("#8E24AA")),
            "Round/device-prefixed TX/RX messages lost segmented colors");
      check(negative_direction_format.foreground().color() ==
                    QColor(QStringLiteral("#D93025")) &&
                negative_payload_format.foreground().color() ==
                    QColor(QStringLiteral("#D93025")) &&
                negative_direction_format.fontWeight() == QFont::Bold &&
                prose_format.foreground().color() ==
                    QColor(QStringLiteral("#303030")) &&
                path_format.foreground().color() ==
                    QColor(QStringLiteral("#303030")),
            "NRC priority or descriptive-text misclassification is incorrect");

      persisted.close();
      check(persisted.open(QIODevice::ReadOnly),
            "Execution log could not be reopened after rich-text rendering");
      const auto persisted_after_rich_text = persisted.readAll();
      check(persisted_after_rich_text.contains(
                "[第2/3次] TX [0x72E] 36 A5 FF") &&
                !persisted_after_rich_text.contains("#1565C0"),
            "Rich-text rendering changed or decorated the persisted log");

      clear_log->trigger();
      bridge->probeRunningChanged(true);
      bridge->logMessage(QStringLiteral(
          "ASC原始总线日志：D:/logs/traces/probe/full_probe_trace.asc"));
      bridge->logMessage(QStringLiteral(
          "在线探测：该项目不使用 CANoe DOUT，保持台架现有外部供电状态。"));
      bridge->logMessage(QStringLiteral(
          "PASS：CAN硬件物理CH2 已打开（后端：Vector XL）"));
      bridge->logMessage(QStringLiteral("TX [0x7E2] 10 03"));
      bridge->logMessage(QStringLiteral("RX [0x72F] 50 03 00 32 01 5E"));
      bridge->logMessage(QStringLiteral("TX [0x7E2] 31 01 02 03"));
      bridge->logMessage(QStringLiteral("RX [0x72F] 71 01 02 03 05"));
      bridge->logMessage(QStringLiteral(
          "WARN：楚能ARC331 APP刷新入口可用，但刷新条件状态为0x05；正式流程将按项目参考策略继续并保留原始响应。"));
      application.processEvents();
      const auto warning_block = find_log_block(
          log_view->document(), QStringLiteral("WARN：刷新条件状态 0x05"));
      const auto warning_format = log_fragment_format(
          warning_block, QStringLiteral("WARN：刷新条件状态 0x05"));
      check(warning_block.isValid() &&
                warning_format.foreground().color() ==
                    QColor(QStringLiteral("#C25E00")) &&
                warning_format.fontWeight() == QFont::Bold,
            "WARN log entry did not keep the configured warning color");
      check(QMetaObject::invokeMethod(
                &window, "handleProbeFinished", Qt::DirectConnection,
                Q_ARG(bool, true), Q_ARG(bool, false),
                Q_ARG(QString,
                      QStringLiteral(
                          "设备在线：响应 50 03；ProgrammingPrecondition=71 01 02 03 05"))),
            "Concise probe integration result was not invokable");
      application.processEvents();
      const auto concise_probe_view = log_view->toPlainText();
      check(concise_probe_view.count(QLatin1Char('\n')) + 1 == 7 &&
                concise_probe_view.contains(QStringLiteral("CAN已打开")) &&
                concise_probe_view.contains(
                    QStringLiteral("TX [0x7E2] 31 01 02 03")) &&
                concise_probe_view.contains(
                    QStringLiteral("WARN：刷新条件状态 0x05")) &&
                concise_probe_view.contains(QStringLiteral(
                    "在线探测成功：诊断响应正常，APP刷新入口判定可用")) &&
                !concise_probe_view.contains(QStringLiteral("ASC原始总线日志")) &&
                !concise_probe_view.contains(QStringLiteral("CANoe DOUT")) &&
                !concise_probe_view.contains(
                    QStringLiteral("ProgrammingPrecondition=")),
            "Online-probe runtime view was not reduced to key CAN/UDS/warning/result lines");
      QFile probe_persisted(execution_log);
      check(probe_persisted.open(QIODevice::ReadOnly),
            "Detailed execution log could not be reopened for probe filtering test");
      const auto detailed_probe_log = probe_persisted.readAll();
      check(detailed_probe_log.contains(
                "ASC原始总线日志：D:/logs/traces/probe/full_probe_trace.asc") &&
                detailed_probe_log.contains("CANoe DOUT") &&
                detailed_probe_log.contains(
                    "ProgrammingPrecondition=71 01 02 03 05") &&
                !detailed_probe_log.contains(
                    "在线探测成功：诊断响应正常，APP刷新入口判定可用"),
            "Probe UI filtering removed raw detail from the file log or persisted UI-only summaries");

      clear_log->trigger();
      check(QMetaObject::invokeMethod(&window, "beginFlashUiLog",
                                      Qt::DirectConnection),
            "Flash preparation UI state could not be started");
      bridge->logMessage(QStringLiteral(
          "Flash target: 楚能 / ARC331 / 左后雷达; Profile=chuneng_331_left_rear; Target=left_rear; Flow=chuneng_arc331; Entry=APP"));
      bridge->logMessage(QStringLiteral(
          "Pre-flash qualification: Status=PASS; Completed at=2026-08-31T18:18:41.168; Detail=设备在线"));
      bridge->logMessage(QStringLiteral(
          "CAN configuration: Hardware backend=Vector XL; Channel=2; Nominal bitrate=500000 bit/s; Data bitrate=2000000 bit/s; CAN FD=yes; Padding=0x55"));
      bridge->logMessage(QStringLiteral(
          "Flash file: Boot Driver=D:/project/UDS_tools/dist/resources/driver.cbf; exists=yes; size=17322 bytes"));
      bridge->logMessage(QStringLiteral(
          "Flash file: APP=D:/project/UDS_tools/dist/resources/app.cbf; exists=yes; size=1573818 bytes"));
      bridge->logMessage(
          QStringLiteral("Flash file: APP Data=<not configured>"));
      bridge->logMessage(QStringLiteral(
          "Flash file: SeedKey=D:/project/UDS_tools/dist/resources/key.dll; exists=yes; size=939520 bytes"));
      bridge->logMessage(QStringLiteral(
          "Cycle 1/1 raw ASC PASS: D:/logs/full_trace.asc; raw BLF PASS: D:/logs/full_trace.blf"));
      bridge->logMessage(QStringLiteral("第1/1次完整刷写开始"));
      bridge->logMessage(QStringLiteral(
          "ChuNeng ARC331 dedicated flow selected: Driver+APP CBF pair; 256-byte signature state machine"));
      bridge->logMessage(QStringLiteral(
          "Driver CBF identity: target=7052A5023002AB; software_id=7052A5023002"));
      bridge->logMessage(QStringLiteral(
          "APP CBF identity: target=7052A5023002AB; software_id=7052A5023002"));
      bridge->logMessage(QStringLiteral(
          "ChuNeng ARC331 paired input preflight passed: mode=Driver CBF + APP CBF; both roles enter the same 0202/256-byte-signature state machine"));
      application.processEvents();
      const auto concise_flash_preparation = log_view->toPlainText();
      check(concise_flash_preparation.count(QLatin1Char('\n')) + 1 == 8 &&
                concise_flash_preparation.contains(
                    QStringLiteral("刷写前条件检查：通过")) &&
                concise_flash_preparation.contains(QStringLiteral(
                    "CAN配置：Vector XL，CH2，500K/2M，CAN FD")) &&
                concise_flash_preparation.contains(QStringLiteral(
                    "Driver文件检查：已找到（17,322 bytes）")) &&
                concise_flash_preparation.contains(QStringLiteral(
                    "APP文件检查：已找到（1,573,818 bytes）")) &&
                concise_flash_preparation.contains(
                    QStringLiteral("Driver与APP匹配检查：通过")) &&
                concise_flash_preparation.contains(
                    QStringLiteral("ASC/BLF记录：已启动")) &&
                concise_flash_preparation.endsWith(
                    QStringLiteral("开始第1/1次刷写")) &&
                !concise_flash_preparation.contains(
                    QStringLiteral("D:/project/UDS_tools")) &&
                !concise_flash_preparation.contains(
                    QStringLiteral("CBF identity")) &&
                !concise_flash_preparation.contains(
                    QStringLiteral("signature state machine")) &&
                !concise_flash_preparation.contains(
                    QStringLiteral("<not configured>")),
            "Flash-preparation runtime view was not reduced to eight operator-facing lines");
      bridge->logMessage(QStringLiteral("TX [0x7E2] 10 02"));
      bridge->logMessage(QStringLiteral("RX [0x72F] 50 02 00 32 01 F4"));
      application.processEvents();
      check(log_view->toPlainText().contains(
                QStringLiteral("TX [0x7E2] 10 02")) &&
                log_view->toPlainText().contains(
                    QStringLiteral("RX [0x72F] 50 02 00 32 01 F4")),
            "Flash preparation filter did not release normal UDS runtime logs");
      QFile flash_persisted(execution_log);
      check(flash_persisted.open(QIODevice::ReadOnly),
            "Detailed execution log could not be reopened for flash filtering test");
      const auto detailed_flash_log = flash_persisted.readAll();
      check(detailed_flash_log.contains(
                "D:/project/UDS_tools/dist/resources/driver.cbf") &&
                detailed_flash_log.contains("APP Data=<not configured>") &&
                detailed_flash_log.contains("Driver CBF identity:") &&
                detailed_flash_log.contains("signature state machine") &&
                detailed_flash_log.contains("D:/logs/full_trace.asc") &&
                !detailed_flash_log.contains(
                    "Driver文件检查：已找到（17,322 bytes）"),
            "Flash UI filtering removed raw detail from the file log or persisted UI-only summaries");
      check(QMetaObject::invokeMethod(
                &window, "handleFlashFinished", Qt::DirectConnection,
                Q_ARG(bool, true), Q_ARG(bool, false),
                Q_ARG(QString, QStringLiteral("test concise flash result")),
                Q_ARG(QString, QString{})),
            "Flash filtering integration result was not invokable");

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
      QKeyEvent end_event(QEvent::KeyPress, Qt::Key_End,
                          Qt::KeypadModifier);
      QApplication::sendEvent(log_view, &end_event);
      application.processEvents();
      const auto end_cursor_rect = log_view->cursorRect(log_view->textCursor());
      const auto end_bottom_gap =
          log_view->viewport()->height() - end_cursor_rect.bottom();
      check(log_view->textCursor().position() ==
                log_view->document()->characterCount() - 1 &&
                log_view->verticalScrollBar()->value() ==
                    log_view->verticalScrollBar()->maximum(),
            "End did not navigate to the bottom of the execution log");
      check(end_cursor_rect.isValid() && end_cursor_rect.top() > 0 &&
                end_bottom_gap >= 0 &&
                end_bottom_gap <= end_cursor_rect.height() * 2 &&
                log_view->cursorForPosition(QPoint(0, 0)).blockNumber() <
                    log_view->document()->lastBlock().blockNumber(),
            "End placed the final log at the top or left blank space below it");
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
      auto result_format = log_fragment_format(
          result_block, QStringLiteral("========== 刷写成功 =========="));
      check(result_block.text().contains(QStringLiteral("刷写成功")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#188038")) &&
                result_format.fontWeight() == QFont::Bold,
            "Successful flash result is not a bold green log entry");
      check(invoke_flash_result(false, false),
            "Flash failure handler is not invokable");
      application.processEvents();
      check(log_view->verticalScrollBar()->value() ==
                log_view->verticalScrollBar()->maximum(),
            "Execution log did not keep following consecutive output");
      result_block = log_view->document()->lastBlock();
      result_format = log_fragment_format(
          result_block, QStringLiteral("========== 刷写失败 =========="));
      check(result_block.text().contains(QStringLiteral("刷写失败")) &&
                 result_format.foreground().color() ==
                     QColor(QStringLiteral("#D93025")) &&
                 result_format.fontWeight() == QFont::Bold,
             "Failed flash result is not a bold red log entry");

      QTemporaryDir report_directory;
      check(report_directory.isValid(),
            "Could not create a report-link test directory");
      const auto clickable_report =
          report_directory.filePath(QStringLiteral("latest_report.html"));
      QFile report_file(clickable_report);
      check(report_file.open(QIODevice::WriteOnly) &&
                report_file.write("<!doctype html><title>report</title>") > 0,
            "Could not create the clickable report fixture");
      report_file.close();
      check(QMetaObject::invokeMethod(
                &window, "handleFlashFinished", Qt::DirectConnection,
                Q_ARG(bool, true), Q_ARG(bool, false),
                Q_ARG(QString, QStringLiteral("test report link")),
                Q_ARG(QString, clickable_report)),
            "Flash result handler rejected a report path");
      application.processEvents();
      const auto report_block = find_log_block(
          log_view->document(), QStringLiteral("报告："));
      const auto native_report = QDir::toNativeSeparators(clickable_report);
      const auto report_link_format =
          log_fragment_format(report_block, native_report);
      const auto report_link_position =
          log_fragment_position(report_block, native_report);
      QTextCursor report_link_cursor(log_view->document());
      report_link_cursor.setPosition(report_link_position + 1);
      const auto detected_report =
          uds::ui::qt::main_window_support::localFileLinkAt(
              log_view, log_view->cursorRect(report_link_cursor).center());
      check(report_block.isValid() && report_link_position >= 0 &&
                report_link_format.isAnchor() &&
                QUrl(report_link_format.anchorHref()).isLocalFile() &&
                detected_report &&
                QDir::cleanPath(*detected_report) ==
                    QDir::cleanPath(native_report),
            "Report path is not rendered and detected as a clickable local-file link");

      progress_status->setText(QStringLiteral("最近一次刷写成功"));
      check(QMetaObject::invokeMethod(
                &window, "handleVersionCheckRunningChanged",
                Qt::DirectConnection, Q_ARG(bool, true)) &&
                QMetaObject::invokeMethod(
                    &window, "handleProgressChanged", Qt::DirectConnection,
                    Q_ARG(int, 50),
                    Q_ARG(QString, QStringLiteral("版本读取进行中"))),
            "Version-read UI handlers are not invokable");
      application.processEvents();
      check(progress_status->text() == QStringLiteral("最近一次刷写成功") &&
                version_button->text() == QStringLiteral("读取中") &&
                !version_button->isEnabled() &&
                log_view->toPlainText().contains(
                    QStringLiteral("========== 开始版本读取 ==========")),
            "Version-read progress did not show the running button state, overwrote the flash result, or omitted its log boundary");
      check(QMetaObject::invokeMethod(
                &window, "handleVersionCheckFinished", Qt::DirectConnection,
                Q_ARG(bool, false), Q_ARG(bool, false),
                Q_ARG(QString, QStringLiteral("读取失败：测试错误"))),
            "Version-read result handler is not invokable");
      application.processEvents();
      check(progress_status->text() == QStringLiteral("最近一次刷写成功") &&
                version_button->text() == QStringLiteral("一键读取") &&
                log_view->toPlainText().contains(
                    QStringLiteral("读取失败：测试错误")) &&
                !log_view->toPlainText().contains(
                    QStringLiteral("========== 版本读取失败 ==========")) &&
                log_view->toPlainText().contains(
                    QStringLiteral("========== 刷写成功 ==========")),
            "Concise failed version read hid the latest flash result, duplicated a boundary, or erased log history");
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
      auto nrc_reason_format = log_fragment_format(
          nrc_reason_block, QStringLiteral("NRC 0x31"));
      check(nrc_reason_block.text().contains(
                QStringLiteral("NRC 0x31 RequestOutOfRange")) &&
                nrc_reason_block.text().contains(
                    QStringLiteral("请求超出范围")) &&
                !nrc_reason_block.text().contains(
                    QStringLiteral("NRC/timeout")) &&
                nrc_reason_format.foreground().color() ==
                    QColor(QStringLiteral("#D93025")) &&
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
      result_format =
          log_fragment_format(result_block, QStringLiteral("67 11"));
      check(result_block.text().contains(QStringLiteral("67 11")) &&
                !result_block.text().contains(QStringLiteral("NRC 0xCB")) &&
                result_format.foreground().color() !=
                    QColor(QStringLiteral("#D93025")),
            "Security seed payload bytes were misclassified as an NRC");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString,
                      QStringLiteral("RX [0x72F] 71 01 02 03 05"))),
            "Routine 0203 warning log injection failed");
      result_block = log_view->document()->lastBlock();
      result_format = log_fragment_format(
          result_block, QStringLiteral("71 01 02 03 05"));
      check(result_block.text().contains(
                QStringLiteral("71 01 02 03 05")) &&
                !result_block.text().contains(
                    QStringLiteral("校验/执行失败")) &&
                result_format.foreground().color() !=
                    QColor(QStringLiteral("#D93025")),
            "Execution log misclassified ARC331 0203 status 05 as a red "
            "final failure");
      check(QMetaObject::invokeMethod(
                bus_monitor_page, "monitorMessage", Qt::DirectConnection,
                Q_ARG(QString,
                      QStringLiteral("RX [0x72F] 71 01 02 02 05"))),
            "Routine failure log injection failed");
      result_block = log_view->document()->lastBlock();
      result_format = log_fragment_format(
          result_block, QStringLiteral("71 01 02 02 05"));
      check(result_block.text().contains(
                QStringLiteral("RoutineControl 0x0202")) &&
                result_block.text().contains(
                    QStringLiteral("数据/软件签名校验")) &&
                result_block.text().contains(QStringLiteral("状态 0x05")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#D93025")) &&
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
      result_format =
          log_fragment_format(result_block, QStringLiteral("在线探测成功："));
      check(result_block.text().contains(
                QStringLiteral("在线探测成功：设备在线")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#188038")) &&
                result_format.fontWeight() == QFont::Bold,
            "Concise online-probe result is not a bold green log entry");
      check(invoke_probe_result(false),
            "Probe failure handler is not invokable");
      result_block = log_view->document()->lastBlock();
      result_format =
          log_fragment_format(result_block, QStringLiteral("在线探测失败"));
      check(result_block.text().contains(QStringLiteral("在线探测失败")) &&
                result_format.foreground().color() ==
                    QColor(QStringLiteral("#D93025")) &&
                result_format.fontWeight() == QFont::Bold,
            "Concise failed-probe result is not a bold red log entry");
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

      // User-selected flash files remain isolated per project/target and are
      // persisted so a restart keeps referring to the original selected
      // filename copied into that target's resource directory.
      const auto geely_vendor = find_text(projects, QStringLiteral("吉利"));
      check(geely_vendor >= 0, "Geely vendor missing for runtime file test");
      projects->setCurrentIndex(geely_vendor);
      application.processEvents();
      const auto p611_project = find_text(devices, QStringLiteral("P611"));
      const auto p417_project = find_text(devices, QStringLiteral("P417"));
      const auto p416_project = find_text(devices, QStringLiteral("P416"));
      check(p611_project >= 0 && p417_project >= 0 && p416_project >= 0,
            "Geely P416/P417/P611 projects missing for runtime file test");
      devices->setCurrentIndex(p611_project);
      application.processEvents();

      const std::array<QString, 7> file_field_names{
          QStringLiteral("driverPathLineEdit"),
          QStringLiteral("appPathLineEdit"),
          QStringLiteral("calPathLineEdit"),
          QStringLiteral("driverVerifyPathLineEdit"),
          QStringLiteral("appVerifyPathLineEdit"),
          QStringLiteral("calVerifyPathLineEdit"),
          QStringLiteral("seedKeyDllPathLineEdit")};
      const std::array<QString, 7> runtime_paths{
          QStringLiteral("D:/611/runtime_driver.vbf"),
          QStringLiteral("D:/611/runtime_app.vbf"),
          QStringLiteral("D:/611/runtime_ess.vbf"),
          QStringLiteral("D:/611/runtime_driver_verify.bin"),
          QStringLiteral("D:/611/runtime_app_verify.bin"),
          QStringLiteral("D:/611/runtime_cal_verify.bin"),
          QStringLiteral("D:/611/runtime_seedkey.dll")};
      std::array<QString, 7> profile_defaults;
      for (std::size_t index = 0; index < file_field_names.size(); ++index) {
        auto* field = window.findChild<QLineEdit*>(file_field_names[index]);
        check(field, "Runtime flash-file field missing");
        profile_defaults[index] = field->property("fullPath").toString();
        field->setProperty("fullPath", runtime_paths[index]);
        field->setText(runtime_paths[index]);
      }

      devices->setCurrentIndex(p416_project);
      application.processEvents();
      devices->setCurrentIndex(find_text(devices, QStringLiteral("P611")));
      application.processEvents();
      for (std::size_t index = 0; index < file_field_names.size(); ++index) {
        const auto* field =
            window.findChild<QLineEdit*>(file_field_names[index]);
        check(field && field->property("fullPath").toString() ==
                           QDir::toNativeSeparators(runtime_paths[index]),
              "Runtime flash-file selection was not restored");
      }

      {
        uds::ui::qt::MainWindow persisted_reopen;
        auto* persisted_vendors = persisted_reopen.findChild<QComboBox*>(
            QStringLiteral("projectComboBox"));
        auto* persisted_projects = persisted_reopen.findChild<QComboBox*>(
            QStringLiteral("deviceComboBox"));
        persisted_vendors->setCurrentIndex(
            find_text(persisted_vendors, QStringLiteral("吉利")));
        application.processEvents();
        persisted_projects->setCurrentIndex(
            find_text(persisted_projects, QStringLiteral("P611")));
        application.processEvents();
        for (std::size_t index = 0; index < file_field_names.size(); ++index) {
          const auto* field = persisted_reopen.findChild<QLineEdit*>(
              file_field_names[index]);
          check(field && field->property("fullPath").toString() ==
                             QDir::toNativeSeparators(runtime_paths[index]),
                "A new window did not restore the persisted flash-file selection");
        }
      }

      const std::array<QString, 7> file_label_names{
          QStringLiteral("driverPathLabel"),
          QStringLiteral("appPathLabel"),
          QStringLiteral("calPathLabel"),
          QStringLiteral("driverVerifyPathLabel"),
          QStringLiteral("appVerifyPathLabel"),
          QStringLiteral("calVerifyPathLabel"),
          QStringLiteral("seedKeyDllPathLabel")};
      for (std::size_t index = 0; index < file_label_names.size(); ++index) {
        auto* label = window.findChild<QLabel*>(file_label_names[index]);
        check(label && label->toolTip().contains(QStringLiteral("双击恢复")),
              "Flash-file label has no default-restore affordance");
        QEvent restore_file_event(QEvent::MouseButtonDblClick);
        QCoreApplication::sendEvent(label, &restore_file_event);
        const auto* restored_field =
            window.findChild<QLineEdit*>(file_field_names[index]);
        check(restored_field &&
                  restored_field->property("fullPath").toString() ==
                      profile_defaults[index],
              "Double-clicking a flash-file label did not restore its Profile default");
        if (index + 1U < file_field_names.size()) {
          const auto* untouched_field =
              window.findChild<QLineEdit*>(file_field_names[index + 1U]);
          check(untouched_field &&
                    untouched_field->property("fullPath").toString() ==
                        QDir::toNativeSeparators(runtime_paths[index + 1U]),
                "Restoring one flash-file default changed another field");
        }
      }

      {
        uds::ui::qt::MainWindow reopened;
        for (std::size_t index = 0; index < file_field_names.size(); ++index) {
          const auto* field =
              reopened.findChild<QLineEdit*>(file_field_names[index]);
          check(field && field->property("fullPath").toString() ==
                             profile_defaults[index],
                "A new window persisted a runtime flash-file override");
        }
      }
      checkpoint("runtime-file-selection");

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
                !tx_id->isReadOnly() && !rx_id->isReadOnly() &&
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
      check(!tx_id->isReadOnly() && !rx_id->isReadOnly(),
            "Chuneng diagnostic ID overrides are not editable");
      radar->setCurrentIndex(find_target(radar, QStringLiteral("right_rear")));
      application.processEvents();
      check(tx_id->text() == QStringLiteral("0x72C") &&
                rx_id->text() == QStringLiteral("0x72D"),
            "Target selection did not restore its default diagnostic IDs");
      check_project_devices(application, projects, devices,
                            QStringLiteral("吉利"),
                            {QStringLiteral("P416"),
                             QStringLiteral("P417"),
                             QStringLiteral("P611")});
      devices->setCurrentIndex(find_text(devices, QStringLiteral("P416")));
      application.processEvents();
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
      devices->setCurrentIndex(find_text(devices, QStringLiteral("P611")));
      application.processEvents();
      check(radar->count() == 1 &&
                radar->currentText() == QStringLiteral("ARS1.31L") &&
                entries->findData(QStringLiteral("app")) >= 0 &&
                entries->findData(QStringLiteral("ft")) >= 0,
            "Geely P611 did not reuse the P416 device and entry modes");
      check(version_selection->text().contains(QStringLiteral("吉利")) &&
                version_selection->text().contains(QStringLiteral("P611")) &&
                version_selection->text().contains(QStringLiteral("ARS1.31L")) &&
                version_table->rowCount() == 13 &&
                version_table->item(0, 1)->text() == QStringLiteral("F180"),
            "Version page did not follow P611 or reuse the P416 DID plan");
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
                !tx_id->isReadOnly() && !rx_id->isReadOnly() &&
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
      const std::array chery_editable_endpoints{
          std::pair{QStringLiteral("T1EJ"),
                    std::pair{QStringLiteral("0x7AF"),
                              QStringLiteral("0x7BF")}},
          std::pair{QStringLiteral("T22"),
                    std::pair{QStringLiteral("0x7AF"),
                              QStringLiteral("0x7BF")}},
          std::pair{QStringLiteral("E0Y"),
                    std::pair{QStringLiteral("0x70D"),
                              QStringLiteral("0x78D")}}};
       for (const auto& [project, endpoint] : chery_editable_endpoints) {
        devices->setCurrentIndex(find_text(devices, project));
        application.processEvents();
         check(!tx_id->isReadOnly() && !rx_id->isReadOnly() &&
                  tx_id->text() == endpoint.first &&
                  rx_id->text() == endpoint.second,
               "Chery CANoe default endpoint was not applied as an editable UI value");
         {
           const auto t1ej_app = entries->findData(QStringLiteral("app"));
           const auto t1ej_cal = entries->findData(QStringLiteral("cal"));
           const auto t1ej_app_cal =
               entries->findData(QStringLiteral("app_cal"));
           check(entries->count() == 3 && t1ej_app >= 0 && t1ej_cal >= 0 &&
                     t1ej_app_cal >= 0 &&
                     entries->itemText(t1ej_cal) == QStringLiteral("CAL") &&
                     entries->itemText(t1ej_app_cal) ==
                         QStringLiteral("APP+CAL"),
                 "T1EJ/T22/E0Y TC_7/TC_2 flashing modes are not exposed");
         }
         const auto is_e0y = project == QStringLiteral("E0Y");
         check(update_public_key &&
                    update_public_key->isHidden() != is_e0y &&
                    !update_public_key->isChecked(),
                "Update_PublicKey must be visible only for E0Y and default off");
         if (is_e0y) {
           check(update_public_key->isEnabled(),
                  "E0Y APP did not enable Update_PublicKey");
           update_public_key->setChecked(true);
           entries->setCurrentIndex(
                entries->findData(QStringLiteral("app_cal")));
           application.processEvents();
           check(!update_public_key->isEnabled() &&
                      !update_public_key->isChecked(),
                  "E0Y APP+CAL must reject and clear Update_PublicKey");
           entries->setCurrentIndex(entries->findData(QStringLiteral("cal")));
           application.processEvents();
           check(update_public_key->isEnabled() &&
                      !update_public_key->isChecked(),
                  "E0Y CAL did not safely expose Update_PublicKey");
         }
       }
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
                !tx_id->isReadOnly() && !rx_id->isReadOnly(),
            "Xizhong diagnostic ID overrides are not editable");
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
      radar->setCurrentIndex(find_target(radar, QStringLiteral("lsmr")));
      application.processEvents();
      check(radar->currentText() == QStringLiteral("LSMR 从雷达（待验证）") &&
                tx_id->text() == QStringLiteral("0x18DAB6F1") &&
                rx_id->text() == QStringLiteral("0x18DAF1B6") &&
                entries->currentData().toString() == QStringLiteral("app") &&
                entries->findData(QStringLiteral("ft")) < 0 && app_path &&
                app_path->text().isEmpty() &&
                chuneng_driver_path->text().isEmpty(),
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
                !tx_id->isReadOnly() && !rx_id->isReadOnly() &&
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
                  !tx_id->isReadOnly() && !rx_id->isReadOnly() &&
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
                             {QStringLiteral("ARC"),
                              QStringLiteral("ARF")});
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
                     QStringLiteral("APP 验签文件（可选）") &&
                 window.findChild<QLineEdit*>(
                     QStringLiteral("appVerifyPathLineEdit"))
                     ->property("fullPath")
                     .toString()
                     .isEmpty(),
            "ARC merged four-target UI or preset resources mismatch");
      check(!tx_id->isReadOnly() && !rx_id->isReadOnly() && tx_id_label &&
                rx_id_label &&
                tx_id_label->toolTip().contains(QStringLiteral("双击恢复")) &&
                rx_id_label->toolTip().contains(QStringLiteral("双击恢复")),
            "Diagnostic ID override or default-restore affordance is missing");
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
      tx_id->setText(QStringLiteral("0x123"));
      rx_id->setText(QStringLiteral("0x456"));
      QEvent restore_tx_event(QEvent::MouseButtonDblClick);
      QCoreApplication::sendEvent(tx_id_label, &restore_tx_event);
      check(tx_id->text() == QStringLiteral("0x770") &&
                rx_id->text() == QStringLiteral("0x456"),
            "Double-clicking Tx ID did not restore only the target default");
      QEvent restore_rx_event(QEvent::MouseButtonDblClick);
      QCoreApplication::sendEvent(rx_id_label, &restore_rx_event);
      check(tx_id->text() == QStringLiteral("0x770") &&
                rx_id->text() == QStringLiteral("0x778"),
            "Double-clicking Rx ID did not restore only the target default");
      {
        QSettings isolated_target_settings;
        isolated_target_settings.remove(
            QStringLiteral("selectors/profile_state/lp_arc"));
      }
      radar->setCurrentIndex(0);
      application.processEvents();
      entries->setCurrentIndex(entries->findData(QStringLiteral("ft")));
      application.processEvents();
      check(entries->currentData().toString() == QStringLiteral("ft") &&
                entries->currentText() == QStringLiteral("FT"),
            "ARC FT entry selection mismatch");

      // Each target has independent operator state. A mode chosen for ARC
      // target 0 must not leak to target 3, and returning restores target 0.
      radar->setCurrentIndex(3);
      application.processEvents();
      check(entries->currentData().toString() == QStringLiteral("app"),
            "ARC target 0 FT mode leaked into target 3");
      radar->setCurrentIndex(0);
      application.processEvents();
      check(entries->currentData().toString() == QStringLiteral("ft"),
            "Returning to ARC target 0 did not restore its FT entry mode");

      // Entry mode belongs to the selected Profile/target. A mode chosen for
      // ARC must not leak into ARF, while returning to ARC must restore ARC's
      // own selection.
      {
        QSettings isolated_mode_settings;
        isolated_mode_settings.remove(
            QStringLiteral("selectors/profile_state/lp_arf"));
      }
      devices->setCurrentIndex(find_text(devices, QStringLiteral("ARF")));
      application.processEvents();
      check(entries->currentData().toString() == QStringLiteral("app"),
            "ARC FT mode leaked into the independent ARF Profile");
      devices->setCurrentIndex(find_text(devices, QStringLiteral("ARC")));
      application.processEvents();
      check(entries->currentData().toString() == QStringLiteral("ft"),
            "Returning to ARC did not restore ARC's FT entry mode");

      entries->setCurrentIndex(entries->findData(QStringLiteral("app")));
      application.processEvents();
      check_file_panel_is_stable(false, true);
      devices->setCurrentIndex(find_text(devices, QStringLiteral("ARF")));
      application.processEvents();
      check(!radar->isHidden() && radar->count() == 1 &&
                radar->currentText() == QStringLiteral("ARF 雷达") &&
                !tx_id->isReadOnly() &&
                 !rx_id->isReadOnly() &&
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
                window.findChild<QLabel*>(QStringLiteral("appPathLabel"))
                        ->text() == QStringLiteral("APP 文件/升级包") &&
                app_path->text().endsWith(QStringLiteral(".tmp"),
                                          Qt::CaseInsensitive) &&
                window.findChild<QLineEdit*>(
                          QStringLiteral("appVerifyPathLineEdit"))
                        ->text()
                        .contains(QStringLiteral("TMP 内置 Certificate")) &&
                window.findChild<QLineEdit*>(
                          QStringLiteral("appVerifyPathLineEdit"))
                        ->text()
                        .contains(QStringLiteral("已解析")) &&
                window.findChild<QLineEdit*>(
                          QStringLiteral("appVerifyPathLineEdit"))
                        ->property("embeddedVerification")
                        .toBool() &&
                window.findChild<QLineEdit*>(
                          QStringLiteral("appVerifyPathLineEdit"))
                        ->property("fullPath")
                        .toString()
                        .isEmpty() &&
                window.findChild<QPushButton*>(
                          QStringLiteral("appVerifyBrowseButton"))
                        ->text() == QStringLiteral("详情") &&
                c857_seed_key->text().contains(
                    QStringLiteral("lingpao_SeednKey")) &&
                 app_verify_label &&
                 app_verify_label->text() == QStringLiteral("APP 验签（内置）"),
            "LP-ARF UI endpoint, entry names or resources mismatch");
      check_file_panel_is_stable(true);

      // Keep the green embedded-certificate summary in the UI, but never
      // forward it as an external verification-file path.
      QObject::disconnect(&window, &uds::ui::qt::MainWindow::flashRequested,
                          nullptr, nullptr);
      int arf_flash_request_count{};
      QString arf_app_verify_path;
      QObject::connect(
          &window, &uds::ui::qt::MainWindow::flashRequested, &window,
          [&](int, const QString&, const QString&, bool, unsigned, unsigned,
              quint32, quint32, const QString&, const QString&, const QString&,
              const QString&, const QString& app_verify_path) {
            ++arf_flash_request_count;
            arf_app_verify_path = app_verify_path;
          });
      start_flash->click();
      application.processEvents();
      check(arf_flash_request_count == 1 && arf_app_verify_path.isEmpty(),
            "LP-ARF embedded TMP summary leaked into app_verify_path");

      // Full A -> B -> A regression: the vendor remembers its project, the
      // project remembers its target, and mode/diagnostic-ID overrides remain
      // isolated by Profile/target even when the middle Profile does not
      // support FT.
      const auto geely_state_vendor =
          find_text(projects, QStringLiteral("吉利"));
      const auto baic_state_vendor =
          find_text(projects, QStringLiteral("北汽"));
      check(geely_state_vendor >= 0 && baic_state_vendor >= 0,
            "Geely/BAIC vendors missing for selector-state regression");
      projects->setCurrentIndex(geely_state_vendor);
      application.processEvents();
      const auto p611 = find_text(devices, QStringLiteral("P611"));
      check(p611 >= 0, "Geely P611 missing for selector-state regression");
      devices->setCurrentIndex(p611);
      application.processEvents();
      entries->setCurrentIndex(entries->findData(QStringLiteral("ft")));
      repeat_count->setValue(7);
      channels->setCurrentIndex(channels->findData(2U));
      tx_id->setText(QStringLiteral("0x123"));
      rx_id->setText(QStringLiteral("0x456"));
      QMetaObject::invokeMethod(tx_id, "editingFinished",
                                Qt::DirectConnection);
      QMetaObject::invokeMethod(rx_id, "editingFinished",
                                Qt::DirectConnection);

      projects->setCurrentIndex(baic_state_vendor);
      application.processEvents();
      const auto bqb41 = find_text(devices, QStringLiteral("BQB41"));
      check(bqb41 >= 0, "BAIC BQB41 missing for selector-state regression");
      devices->setCurrentIndex(bqb41);
      application.processEvents();
      check(radar->count() == 4,
            "BAIC BQB41 targets missing for selector-state regression");
      radar->setCurrentIndex(2);
      application.processEvents();
      repeat_count->setValue(8);
      channels->setCurrentIndex(channels->findData(3U));
      check(entries->currentData().toString() == QStringLiteral("app"),
            "Geely FT mode leaked into BAIC BQB41");

      projects->setCurrentIndex(geely_state_vendor);
      application.processEvents();
      check(devices->currentText() == QStringLiteral("P611") &&
                entries->currentData().toString() == QStringLiteral("ft") &&
                repeat_count->value() == 7 &&
                channels->currentData().toUInt() == 2U &&
                tx_id->text() == QStringLiteral("0x123") &&
                rx_id->text() == QStringLiteral("0x456"),
            "Geely P611 project/mode/repeat/channel/diagnostic IDs were not "
            "restored after BAIC");

      projects->setCurrentIndex(baic_state_vendor);
      application.processEvents();
      check(devices->currentText() == QStringLiteral("BQB41") &&
                radar->currentIndex() == 2 &&
                entries->currentData().toString() == QStringLiteral("app") &&
                repeat_count->value() == 8 &&
                channels->currentData().toUInt() == 3U &&
                tx_id->text() == QStringLiteral("0x74A") &&
                rx_id->text() == QStringLiteral("0x7CA"),
            "BAIC BQB41 project/target/repeat/channel state was not restored");

      // Exercise every configured vendor/project/target with an A -> B -> A
      // transition. This turns selector isolation into a catalog-wide contract
      // instead of relying on a few hand-picked Profiles.
      QStringList vendor_names;
      for (int vendor = 0; vendor < projects->count(); ++vendor) {
        vendor_names.push_back(projects->itemText(vendor));
      }
      int selector_state_case{};
      for (const auto& vendor_name : vendor_names) {
        projects->setCurrentIndex(find_text(projects, vendor_name));
        application.processEvents();
        QStringList project_names;
        for (int project = 0; project < devices->count(); ++project) {
          project_names.push_back(devices->itemText(project));
        }
        for (const auto& project_name : project_names) {
          projects->setCurrentIndex(find_text(projects, vendor_name));
          application.processEvents();
          devices->setCurrentIndex(find_text(devices, project_name));
          application.processEvents();
          const auto target_count = radar->count();
          for (int target = 0; target < target_count; ++target) {
            projects->setCurrentIndex(find_text(projects, vendor_name));
            application.processEvents();
            devices->setCurrentIndex(find_text(devices, project_name));
            application.processEvents();
            radar->setCurrentIndex(target);
            application.processEvents();

            auto mode_index = entries->findData(QStringLiteral("ft"));
            if (mode_index < 0) {
              mode_index = entries->findData(QStringLiteral("cal"));
            }
            if (mode_index < 0) mode_index = entries->currentIndex();
            entries->setCurrentIndex(mode_index);
            const auto expected_mode = entries->currentData().toString();
            const auto expected_tx =
                QStringLiteral("0x%1")
                    .arg(0x500 + selector_state_case, 0, 16)
                    .toUpper();
            const auto expected_rx =
                QStringLiteral("0x%1")
                    .arg(0x600 + selector_state_case, 0, 16)
                    .toUpper();
            const auto expected_repeat = selector_state_case + 1;
            const auto expected_channel =
                static_cast<unsigned>((selector_state_case % 4) + 1);
            repeat_count->setValue(expected_repeat);
            channels->setCurrentIndex(
                channels->findData(expected_channel));
            tx_id->setText(expected_tx);
            rx_id->setText(expected_rx);
            QMetaObject::invokeMethod(tx_id, "editingFinished",
                                      Qt::DirectConnection);
            QMetaObject::invokeMethod(rx_id, "editingFinished",
                                      Qt::DirectConnection);

            const auto anchor_vendor =
                vendor_name == QStringLiteral("北汽")
                    ? QStringLiteral("吉利")
                    : QStringLiteral("北汽");
            projects->setCurrentIndex(find_text(projects, anchor_vendor));
            application.processEvents();
            projects->setCurrentIndex(find_text(projects, vendor_name));
            application.processEvents();

            const auto state_error =
                "Catalog-wide selector state was not isolated for " +
                vendor_name.toStdString() + "/" + project_name.toStdString() +
                "/" + std::to_string(target);
            check(devices->currentText() == project_name &&
                      radar->currentIndex() == target &&
                      entries->currentData().toString() == expected_mode &&
                      repeat_count->value() == expected_repeat &&
                      channels->currentData().toUInt() == expected_channel &&
                      tx_id->text() == expected_tx &&
                      rx_id->text() == expected_rx,
                  state_error.c_str());
            ++selector_state_case;
          }
        }
      }
      check(selector_state_case >= 22,
            "Catalog-wide selector-state matrix did not cover all Profiles");

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
      // Establish explicit per-backend values for this exact device before
      // verifying backend switches. The catalog matrix may have already saved
      // a different ZLG channel for the same target.
      zlg_backend->trigger();
      application.processEvents();
      channels->setCurrentIndex(channels->findData(1U));
      vector_backend->trigger();
      application.processEvents();
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
      check(channels->currentData().toUInt() == 4U,
            "Switching back to Vector did not restore this device's CH4");
      channels->setCurrentIndex(channels->findData(2U));
      zlg_backend->trigger();
      application.processEvents();
      check(channels->currentData().toUInt() == 1U,
            "A new device/backend channel did not start from CH1");
      channels->setCurrentIndex(channels->findData(4U));
      application.processEvents();
      check(channels->currentData().toUInt() == 4U,
            "ZLG device-scoped channel could not be selected");
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
    const auto scoped_value = [&settings](const QString& target,
                                          const QString& name) {
      return settings.value(
          QStringLiteral("selectors/profile_state/chuneng_331_left_rear/%1/%2")
              .arg(target, name));
    };
    const auto target_has_scoped_hardware = [&](const QString& target) {
      return scoped_value(target, QStringLiteral("channel/zlg")).toUInt() ==
                 4U &&
             scoped_value(target, QStringLiteral("channel/tosun")).toUInt() ==
                 3U &&
             scoped_value(target, QStringLiteral("channel/vector")).toUInt() ==
                 2U;
    };
    check(target_has_scoped_hardware(QStringLiteral("right_rear")) ||
              target_has_scoped_hardware(QStringLiteral("left_rear")),
          "Qt channels were not saved by Profile/target and backend");
    const auto chuneng_right_mode =
        settings
            .value(QStringLiteral(
                "selectors/profile_state/chuneng_331_left_rear/right_rear/"
                "entry_mode"))
            .toString();
    const auto chuneng_left_mode =
        settings
            .value(QStringLiteral(
                "selectors/profile_state/chuneng_331_left_rear/left_rear/"
                "entry_mode"))
            .toString();
    check(chuneng_right_mode == QStringLiteral("ft") ||
              chuneng_left_mode == QStringLiteral("ft"),
          "Qt entry selection was not saved by Profile/target");
    check(scoped_value(QStringLiteral("right_rear"),
                       QStringLiteral("repeat_count")).toInt() == 3 ||
              scoped_value(QStringLiteral("left_rear"),
                           QStringLiteral("repeat_count")).toInt() == 3,
          "Qt flash repeat count was not saved by Profile/target");
    check(!settings.contains(QStringLiteral("selectors/repeat_count")) &&
              !settings.contains(QStringLiteral("hardware/channel/vector")) &&
              !settings.contains(QStringLiteral("hardware/channel/tosun")) &&
              !settings.contains(QStringLiteral("hardware/channel/zlg")),
          "Device state leaked back into legacy global settings");
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
