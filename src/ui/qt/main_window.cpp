#include "ui/qt/main_window.hpp"
#include "ui/qt/main_window_support.hpp"
#include "ui/qt/resource_file_store.hpp"
#include "ui/qt/ui_log_message_parser.hpp"

#include "core/flash_data.hpp"
#include "drivers/can/can_bus_provider.hpp"
#include "core/uds_nrc.hpp"
#include "ui/qt/bus_monitor_page.hpp"
#include "ui/qt/controller_bridge.hpp"
#include "ui/qt/version_confirmation_page.hpp"
#include "ui/qt/diagnostic_request_page.hpp"
#include "ui_main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace uds::ui::qt {
using namespace main_window_support;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(std::make_unique<Ui::MainWindow>()) {
  ui_->setupUi(this);
  // Keep the prompt visible in the closed field without inserting it into the
  // popup model. Qt's non-editable QComboBox placeholder is not painted by all
  // Windows styles. Overlay a mouse-transparent label instead, preserving the
  // native non-editable combo's whole-field click and keyboard behavior.
  entry_mode_placeholder_ =
      new QLabel(QStringLiteral("请选择"), ui_->entryModeComboBox);
  entry_mode_placeholder_->setObjectName(
      QStringLiteral("entryModePlaceholderLabel"));
  entry_mode_placeholder_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  entry_mode_placeholder_->setAttribute(Qt::WA_TransparentForMouseEvents);
  entry_mode_placeholder_->setStyleSheet(
      QStringLiteral("background: transparent; color: #7b8490;"));
  entry_mode_placeholder_->setGeometry(
      ui_->entryModeComboBox->rect().adjusted(8, 1, -28, -1));
  entry_mode_placeholder_->raise();
  configureVisualDesign();
  version_page_ = new VersionConfirmationPage(ui_->workspaceTabWidget);
  ui_->workspaceTabWidget->addTab(version_page_,
                                  QStringLiteral("版本读取"));
  diagnostic_request_page_ =
      new DiagnosticRequestPage(ui_->workspaceTabWidget);
  ui_->workspaceTabWidget->addTab(diagnostic_request_page_,
                                  QStringLiteral("诊断报文"));
  bus_monitor_page_ = new BusMonitorPage(ui_->workspaceTabWidget);
  ui_->workspaceTabWidget->addTab(bus_monitor_page_, QStringLiteral("总线监听"));
  ui_->workspaceTabWidget->setCurrentWidget(ui_->flashWorkspacePage);
  installWheelMutationGuards();
  auto* file_menu = menuBar()->addMenu(QStringLiteral("文件"));
  file_menu->addAction(QStringLiteral("打开日志目录"), this, [] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("logs"))));
  });
  file_menu->addSeparator();
  file_menu->addAction(QStringLiteral("退出"), this, &QWidget::close);
  auto* device_menu = menuBar()->addMenu(QStringLiteral("设备"));
  auto* hardware_menu =
      device_menu->addMenu(QStringLiteral("CAN硬件后端"));
  hardware_menu->setObjectName(QStringLiteral("canHardwareBackendMenu"));
  can_backend_group_ = new QActionGroup(this);
  can_backend_group_->setObjectName(QStringLiteral("canBackendActionGroup"));
  can_backend_group_->setExclusive(true);
  const auto add_can_backend =
      [this, hardware_menu](const QString& text, const QString& object_name,
                            CanVendor vendor, bool enabled) {
        auto* action = hardware_menu->addAction(text);
        action->setObjectName(object_name);
        action->setCheckable(true);
        action->setData(static_cast<int>(vendor));
        action->setEnabled(enabled);
        can_backend_group_->addAction(action);
        return action;
      };
  auto* vector_backend =
      add_can_backend(QStringLiteral("Vector"), QStringLiteral("vectorCanBackendAction"),
                      CanVendor::Vector, UDS_ENABLE_VECTOR != 0);
  auto* zlg_backend = add_can_backend(
      QStringLiteral("ZLG"),
      QStringLiteral("zlgCanBackendAction"), CanVendor::Zlg,
      UDS_ENABLE_ZLG != 0);
  auto* tosun_backend = add_can_backend(
      QStringLiteral("TOSUN"),
      QStringLiteral("tosunCanBackendAction"), CanVendor::Tosun,
      UDS_ENABLE_TOSUN != 0);
  auto* kvaser_backend = add_can_backend(
      QStringLiteral("Kvaser"),
      QStringLiteral("kvaserCanBackendAction"), CanVendor::Kvaser,
      UDS_ENABLE_KVASER != 0);
  QSettings hardware_settings;
  auto initial_vendor = canVendorFromKey(
      hardware_settings
          .value(QStringLiteral("hardware/can_vendor"),
                 QStringLiteral("vector"))
          .toString());
  auto* initial_action = vector_backend;
  switch (initial_vendor) {
  case CanVendor::Zlg:
    initial_action = zlg_backend;
    break;
  case CanVendor::Tosun:
    initial_action = tosun_backend;
    break;
  case CanVendor::Kvaser:
    initial_action = kvaser_backend;
    break;
  case CanVendor::Vector:
  case CanVendor::Other:
    break;
  }
  if (!initial_action->isEnabled()) {
    initial_vendor = CanVendor::Vector;
    initial_action = vector_backend;
  }
  initial_action->setChecked(true);
  set_default_can_vendor(initial_vendor);
  connect(can_backend_group_, &QActionGroup::triggered, this,
          [this](QAction* action) {
            saveCurrentBackendChannel();
            // Passive monitoring owns a live adapter/channel and a trace
            // session even though it never transmits. Close that old backend
            // first, then change vendor/channel and start a fresh trace on the
            // new backend. This makes an in-monitor UI switch deterministic.
            const auto restart_monitor =
                bus_monitor_page_ && bus_monitor_page_->isRunning();
            if (restart_monitor) bus_monitor_page_->stop();
            const auto vendor =
                static_cast<CanVendor>(action->data().toInt());
            set_default_can_vendor(vendor);
            QSettings settings;
            settings.setValue(QStringLiteral("hardware/can_vendor"),
                              canVendorKey(vendor));
            restoreCurrentBackendChannel(currentProfileDefaultChannel());
            saveComboSelections();
            // restoreCurrentBackendChannel() deliberately blocks the combo-box
            // signal while changing its index. Explicitly refresh every page
            // that caches CAN context before restarting a live monitor.
            syncVersionContext();
            syncBusMonitorContext();
            syncDiagnosticRequestContext();
            if (restart_monitor) bus_monitor_page_->start();
            appendUiLog(QStringLiteral("CAN硬件后端已切换为：%1")
                            .arg(canVendorDisplayName(vendor)) +
                        QStringLiteral("；使用该后端保存的CH%1")
                            .arg(ui_->vectorChannelComboBox
                                     ->currentData().toUInt()));
            updateStatusBar();
          });
  device_menu->addSeparator();
  device_menu->addAction(QStringLiteral("在线探测"), this,
                         &MainWindow::startProbeFromUi);
  auto* diagnostic_menu = menuBar()->addMenu(QStringLiteral("诊断"));
  diagnostic_menu->addAction(QStringLiteral("版本读取"), this, [this] {
    ui_->workspaceTabWidget->setCurrentWidget(version_page_);
  });
  diagnostic_menu->addAction(QStringLiteral("诊断报文"), this, [this] {
    ui_->workspaceTabWidget->setCurrentWidget(diagnostic_request_page_);
  });
  diagnostic_menu->addAction(QStringLiteral("总线监听"), this, [this] {
    ui_->workspaceTabWidget->setCurrentWidget(bus_monitor_page_);
  });
  auto* help_menu = menuBar()->addMenu(QStringLiteral("帮助"));
  help_menu->addAction(QStringLiteral("关于"), this, [this] {
    QMessageBox::about(
        this, QStringLiteral("关于"),
        QStringLiteral("楚航科技\n"
                       "UDS 通用刷写工具\n\n"
                       "版本：V2026.08.20\n"
                       "仅供内部诊断与刷写使用\n\n"
                       "© 2026 楚航科技"));
  });
  // Use a normal resizable main window with the native minimize, maximize and
  // close controls.
  setWindowFlags(Qt::Window);
  initializeExecutionLog();
  controller_bridge_ = std::make_unique<ControllerBridge>(this);
  for (int index = 0; index < ui_->vectorChannelComboBox->count(); ++index) {
    const auto physical_channel = index + 1;
    ui_->vectorChannelComboBox->setItemText(
        index, QStringLiteral("Channel %1").arg(physical_channel));
    ui_->vectorChannelComboBox->setItemData(index, physical_channel);
  }
  connectControllerActions();
  connectActions();
  // Device-scoped state is restored after the selected Profile/target is
  // known.  Starting from the safe minimum prevents another target's legacy
  // global value from leaking into a newly selected device.
  ui_->repeatCountSpinBox->setValue(
      static_cast<int>(uds::app::kMinFlashRepeatCount));
  // The repeat count is an operator input.  Keep the spin-box editor writable
  // so a value can be typed directly as well as changed with its arrows.
  ui_->repeatCountSpinBox->setReadOnly(false);
  ui_->repeatCountSpinBox->setKeyboardTracking(false);
  ui_->repeatCountSpinBox->setFocusPolicy(Qt::StrongFocus);
  if (auto* repeat_editor = ui_->repeatCountSpinBox->findChild<QLineEdit*>()) {
    repeat_editor->setReadOnly(false);
  }
  populateProfileOptions();
  refreshLatestReportPath();
  syncVersionContext();
  appendUiLog(QStringLiteral(
                  "界面已就绪；当前CAN硬件后端：%1；在线探测、刷写、停止、报告及受控电源操作均已连接。")
                  .arg(canVendorDisplayName(default_can_vendor())));
  if (execution_log_file_) {
    appendUiLog(QStringLiteral("本次执行日志：%1")
                    .arg(QDir::toNativeSeparators(
                        execution_log_file_->fileName())));
  }
  for (const auto& message : controller_bridge_->startupMessages()) {
    appendUiLog(message);
  }
}

MainWindow::~MainWindow() = default;

void MainWindow::startDefaultBusMonitoring() {
  followSelectedBusMonitorContext();
}

void MainWindow::configureVisualDesign() {
  ui_->centralWidget->setStyleSheet(QStringLiteral(R"(
QMainWindow,
QWidget#centralWidget,
QWidget#flashWorkspacePage {
  background: #f5f7fa;
  color: #202938;
  font-family: "Microsoft YaHei UI";
  font-size: 13px;
}

QMenuBar {
  background: #ffffff;
  border-bottom: 1px solid #e1e6ed;
  padding: 2px 6px;
}
QMenuBar::item {
  padding: 6px 10px;
  background: transparent;
}
QMenuBar::item:selected {
  background: #eef4fb;
  border-radius: 3px;
}
QTabWidget::pane {
  background: #ffffff;
  border: 1px solid #d8dee8;
  top: -1px;
}
QTabBar::tab {
  background: #eef1f5;
  border: 1px solid #d8dee8;
  padding: 7px 18px;
  margin-right: -1px;
}
QTabBar::tab:selected {
  background: #ffffff;
  color: #175fa8;
  font-weight: 600;
  border-bottom-color: #ffffff;
}
QWidget#leftWorkPanel,
QGroupBox#logGroupBox {
  background: #ffffff;
}
QScrollArea,
QWidget#configurationScrollContents,
QWidget#configurationPanel {
  background: #ffffff;
  border: none;
}
QGroupBox {
  background: transparent;
  border: none;
  border-top: 1px solid #dce2ea;
  margin-top: 15px;
  padding-top: 10px;
  font-weight: 600;
}
QGroupBox::title {
  subcontrol-origin: margin;
  subcontrol-position: top left;
  left: 8px;
  padding: 0 7px;
  background: #ffffff;
  color: #172033;
}
QGroupBox#logGroupBox::title {
  background: transparent;
}
QLabel {
  background: transparent;
  font-weight: 400;
}
QComboBox,
QLineEdit,
QSpinBox {
  min-height: 29px;
  background: #ffffff;
  border: 1px solid #cbd3df;
  border-radius: 2px;
  padding: 0 7px;
  selection-background-color: #2878c7;
}
QComboBox:hover,
QLineEdit:hover,
QSpinBox:hover {
  border-color: #7da8d5;
}
QComboBox:focus,
QLineEdit:focus,
QSpinBox:focus {
  border: 1px solid #2878c7;
}
QComboBox#entryModeComboBox[modeUnselected="true"] {
  background: #eef1f4;
  color: #7b8490;
}
QComboBox::drop-down {
  width: 26px;
  border: none;
}
QPushButton {
  min-height: 30px;
  background: #f8f9fb;
  border: 1px solid #bcc5d1;
  border-radius: 3px;
  padding: 0 12px;
}
QPushButton:hover {
  background: #eef4fb;
  border-color: #76a6d7;
}
QPushButton:pressed {
  background: #e0ebf7;
}
QPushButton:disabled {
  color: #9aa4b2;
  background: #eceff3;
  border-color: #d8dde5;
}
QPushButton#startFlashButton,
QPushButton#versionCheckButton {
  background: #216fba;
  border-color: #216fba;
  color: #ffffff;
  font-weight: 600;
}
QPushButton#startFlashButton:hover,
QPushButton#versionCheckButton:hover {
  background: #185e9f;
  border-color: #185e9f;
}
QPushButton#stopButton {
  color: #c54040;
  font-weight: 600;
}
QProgressBar {
  min-height: 20px;
  background: #eef1f5;
  border: 1px solid #d1d8e2;
  border-radius: 2px;
  text-align: center;
}
QProgressBar::chunk {
  background: #2c7bc4;
}
QPlainTextEdit#logPlainTextEdit {
  background: #ffffff;
  border: 1px solid #d8dee8;
  color: #202938;
  padding: 9px;
  font-family: "Consolas";
  font-size: 12px;
  selection-background-color: #cfe3f7;
}
QTableWidget {
  background: #ffffff;
  alternate-background-color: #f8fafc;
  border: 1px solid #d8dee8;
  gridline-color: #e4e8ee;
}
QHeaderView::section {
  background: #f3f6f9;
  border: none;
  border-right: 1px solid #dfe4eb;
  border-bottom: 1px solid #d8dee8;
  padding: 7px;
  font-weight: 600;
}
QScrollBar:vertical {
  width: 10px;
  background: #f3f5f8;
  margin: 0;
}
QScrollBar::handle:vertical {
  min-height: 28px;
  background: #c2cad5;
  border-radius: 4px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
  height: 0;
}
QStatusBar {
  background: #ffffff;
  border-top: 1px solid #e1e6ed;
}
)"));

  for (auto* combo : findChildren<QComboBox*>()) combo->setMinimumHeight(30);
  for (auto* edit : findChildren<QLineEdit*>()) edit->setMinimumHeight(30);
  for (auto* spin : findChildren<QSpinBox*>()) spin->setMinimumHeight(30);
  for (auto* button : findChildren<QPushButton*>()) {
    button->setMinimumHeight(31);
  }

  ui_->mainLayout->setContentsMargins(14, 12, 14, 14);
  ui_->mainLayout->setSpacing(14);
  ui_->configurationLayout->setSpacing(8);
  ui_->targetGridLayout->setHorizontalSpacing(10);
  ui_->targetGridLayout->setVerticalSpacing(6);
  ui_->targetGridLayout->setColumnMinimumWidth(0, 104);
  ui_->targetGridLayout->setColumnStretch(1, 1);
  ui_->communicationGridLayout->setHorizontalSpacing(9);
  ui_->communicationGridLayout->setVerticalSpacing(6);
  ui_->filesGridLayout->setHorizontalSpacing(8);
  ui_->filesGridLayout->setVerticalSpacing(5);
  ui_->filesGridLayout->setColumnMinimumWidth(0, 124);
  ui_->filesGridLayout->setColumnStretch(1, 1);
  ui_->filesGridLayout->setColumnMinimumWidth(2, 72);
  ui_->filesGroupBox->setMinimumHeight(270);
  ui_->configurationScrollArea->setMinimumWidth(500);
  ui_->logGroupBox->setMinimumWidth(420);

  // Runtime status is shared by probe, flash and power operations for every
  // profile.  Its text can range from a short phase name to a detailed error;
  // keep that content inside the existing left column instead of allowing the
  // QLabel size hint to redistribute the two workspace columns.
  ui_->progressStatusLabel->setWordWrap(true);
  ui_->progressStatusLabel->setSizePolicy(QSizePolicy::Ignored,
                                          QSizePolicy::Preferred);

  ui_->entryModeLabel->setText(QStringLiteral("刷写模式"));
  ui_->txIdLabel->setToolTip(QStringLiteral("双击恢复当前设备的默认 Tx ID"));
  ui_->rxIdLabel->setToolTip(QStringLiteral("双击恢复当前设备的默认 Rx ID"));
  ui_->txIdLabel->installEventFilter(this);
  ui_->rxIdLabel->installEventFilter(this);
  ui_->driverPathLabel->setText(QStringLiteral("Driver 文件"));
  ui_->driverVerifyPathLabel->setText(QStringLiteral("Driver 校验文件"));
  ui_->appPathLabel->setText(QStringLiteral("APP 文件/升级包"));
  ui_->appVerifyPathLabel->setText(QStringLiteral("APP 校验文件"));
  ui_->calPathLabel->setText(QStringLiteral("CAL 文件"));
  ui_->calVerifyPathLabel->setText(QStringLiteral("CAL 校验文件"));
  ui_->seedKeyDllPathLabel->setText(QStringLiteral("SeedKey 算法库"));
  const std::array<std::pair<QLabel*, FlashFileField>, 7> file_labels{{
      {ui_->driverPathLabel, FlashFileField::Driver},
      {ui_->driverVerifyPathLabel, FlashFileField::DriverVerify},
      {ui_->appPathLabel, FlashFileField::App},
      {ui_->appVerifyPathLabel, FlashFileField::AppVerify},
      {ui_->calPathLabel, FlashFileField::Cal},
      {ui_->calVerifyPathLabel, FlashFileField::CalVerify},
      {ui_->seedKeyDllPathLabel, FlashFileField::SeedKeyDll},
  }};
  for (const auto& [label, field] : file_labels) {
    label->setProperty(kFlashFileFieldProperty, static_cast<int>(field));
    label->setToolTip(QStringLiteral(
        "双击恢复当前项目/设备在 resources 中配置的默认文件"));
    label->setCursor(Qt::PointingHandCursor);
    label->installEventFilter(this);
  }

  // Keep frequently used actions and progress visible while the configuration
  // area scrolls. Widgets and their signal connections remain unchanged.
  ui_->mainLayout->removeWidget(ui_->configurationScrollArea);
  ui_->configurationLayout->removeWidget(ui_->operationsGroupBox);
  ui_->configurationLayout->removeWidget(ui_->progressGroupBox);

  auto* left_panel = new QWidget(ui_->flashWorkspacePage);
  left_panel->setObjectName(QStringLiteral("leftWorkPanel"));
  auto* left_layout = new QVBoxLayout(left_panel);
  left_layout->setContentsMargins(10, 0, 10, 10);
  left_layout->setSpacing(7);
  left_layout->addWidget(ui_->configurationScrollArea, 1);
  left_layout->addWidget(ui_->operationsGroupBox, 0);
  left_layout->addWidget(ui_->progressGroupBox, 0);
  ui_->mainLayout->insertWidget(0, left_panel, 48);
  ui_->mainLayout->setStretch(1, 52);

  ui_->logLayout->setContentsMargins(10, 10, 10, 10);
  ui_->logLayout->setSpacing(0);
}

void MainWindow::installWheelMutationGuards() {
  // A wheel gesture over a selector must scroll its containing page, not
  // silently change a project, channel, mode or flash count. Mouse clicks,
  // popup selection and keyboard editing remain unchanged.
  for (auto* combo : findChildren<QComboBox*>()) {
    combo->installEventFilter(this);
  }
  for (auto* spin_box : findChildren<QAbstractSpinBox*>()) {
    spin_box->installEventFilter(this);
  }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (ui_ && entry_mode_placeholder_ &&
      watched == ui_->entryModeComboBox && event->type() == QEvent::Resize) {
    entry_mode_placeholder_->setGeometry(
        ui_->entryModeComboBox->rect().adjusted(8, 1, -28, -1));
    entry_mode_placeholder_->raise();
  }
  if (ui_ && watched == ui_->logPlainTextEdit->viewport()) {
    const auto link_at_event = [&]() {
      const auto* mouse_event = static_cast<QMouseEvent*>(event);
      return localFileLinkAt(ui_->logPlainTextEdit, mouse_event->pos());
    };
    if (event->type() == QEvent::MouseMove) {
      ui_->logPlainTextEdit->viewport()->setCursor(
          link_at_event() ? Qt::PointingHandCursor : Qt::IBeamCursor);
    } else if (event->type() == QEvent::MouseButtonRelease) {
      const auto* mouse_event = static_cast<QMouseEvent*>(event);
      if (mouse_event->button() == Qt::LeftButton) {
        if (const auto path = link_at_event()) {
          if (!QFileInfo(*path).isFile()) {
            QMessageBox::warning(this, QStringLiteral("报告文件不存在"),
                                 *path);
          } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(*path))) {
            QMessageBox::warning(this, QStringLiteral("打开报告失败"), *path);
          }
          return true;
        }
      }
    }
  }
  if (event->type() == QEvent::MouseButtonDblClick) {
    if (watched == ui_->txIdLabel) {
      restoreDefaultDiagnosticId(true);
      return true;
    }
    if (watched == ui_->rxIdLabel) {
      restoreDefaultDiagnosticId(false);
      return true;
    }
    const auto file_field = watched->property(kFlashFileFieldProperty);
    if (file_field.isValid()) {
      restoreDefaultFlashFile(
          static_cast<FlashFileField>(file_field.toInt()));
      return true;
    }
  }
  if (event->type() == QEvent::Wheel &&
      (qobject_cast<QComboBox*>(watched) ||
       qobject_cast<QAbstractSpinBox*>(watched))) {
    auto* watched_widget = qobject_cast<QWidget*>(watched);
    auto* wheel_event = static_cast<QWheelEvent*>(event);
    for (auto* ancestor = watched_widget ? watched_widget->parentWidget()
                                         : nullptr;
         ancestor; ancestor = ancestor->parentWidget()) {
      auto* scroll_area = qobject_cast<QAbstractScrollArea*>(ancestor);
      if (!scroll_area) continue;

      const auto global_position = wheel_event->globalPosition();
      const QPointF viewport_position = scroll_area->viewport()->mapFromGlobal(
          global_position.toPoint());
      QWheelEvent forwarded_event(
          viewport_position, global_position, wheel_event->pixelDelta(),
          wheel_event->angleDelta(), wheel_event->buttons(),
          wheel_event->modifiers(), wheel_event->phase(),
          wheel_event->inverted(), wheel_event->source());
      QCoreApplication::sendEvent(scroll_area->viewport(), &forwarded_event);
      break;
    }
    return true;
  }
  if (watched == ui_->logPlainTextEdit && event->type() == QEvent::KeyPress) {
    const auto* key_event = static_cast<QKeyEvent*>(event);
    auto modifiers = key_event->modifiers();
    // Windows reports End from the numeric keypad (and some laptop Fn key
    // layouts) with KeypadModifier. It identifies the physical key source,
    // not an alternate navigation command, so keep handling it as plain End.
    modifiers.setFlag(Qt::KeypadModifier, false);
    if (modifiers == Qt::NoModifier || modifiers == Qt::ControlModifier) {
      auto cursor = ui_->logPlainTextEdit->textCursor();
      if (key_event->key() == Qt::Key_Home) {
        execution_log_follow_tail_ = false;
        cursor.movePosition(QTextCursor::Start);
        ui_->logPlainTextEdit->setTextCursor(cursor);
        ui_->logPlainTextEdit->ensureCursorVisible();
        return true;
      }
      if (key_event->key() == Qt::Key_End) {
        execution_log_follow_tail_ = true;
        cursor.movePosition(QTextCursor::End);
        ui_->logPlainTextEdit->setTextCursor(cursor);
        auto* scrollbar = ui_->logPlainTextEdit->verticalScrollBar();
        scrollbar->setValue(scrollbar->maximum());
        scheduleExecutionLogTailFollow();
        return true;
      }
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  saveActiveProfileState();
  saveRuntimeFileSelection();
  saveComboSelections();
  if (!probe_running_ && !flash_running_ && !bus_monitor_running_) {
    event->accept();
    return;
  }
  if (bus_monitor_running_ && !probe_running_ && !flash_running_) {
    bus_monitor_page_->stop();
    event->accept();
    return;
  }
  if (QMessageBox::question(
          this, QStringLiteral("操作仍在运行"),
          flash_running_
              ? QStringLiteral(
                    "刷写仍在运行。关闭只能请求中止，不能保证ECU恢复。\n"
                    "确认请求中止吗？窗口将保持打开，直到任务结束并生成报告。")
              : QStringLiteral(
                    "在线探测仍在运行。确认请求停止吗？窗口将保持打开，"
                    "直到任务结束。"),
          QMessageBox::Yes | QMessageBox::No,
          QMessageBox::No) != QMessageBox::Yes) {
    event->ignore();
    return;
  }
  controller_bridge_->requestFlashStop();
  controller_bridge_->requestProbeStop();
  if (flash_running_) {
    ui_->progressStatusLabel->setText(
        QStringLiteral("正在请求中止刷写（等待报告）……"));
    appendUiLog(QStringLiteral(
        "关闭请求已转换为刷写中止请求；ECU恢复状态未确认，窗口保持打开。"));
  }
  event->ignore();
}

} // namespace uds::ui::qt
