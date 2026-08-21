#include "ui/qt/main_window.hpp"

#include "drivers/can/can_bus_provider.hpp"
#include "core/uds_nrc.hpp"
#include "ui/qt/bus_monitor_page.hpp"
#include "ui/qt/controller_bridge.hpp"
#include "ui/qt/version_confirmation_page.hpp"
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
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace uds::ui::qt {
namespace {

constexpr auto kFullPathProperty = "fullPath";
constexpr auto kConfiguredPlaceholderProperty = "configuredPathPlaceholder";

std::optional<std::uint8_t> nrcFromLogLine(const QString& message) {
  static const QRegularExpression explicit_nrc(
      QStringLiteral(R"(NRC\s*(?:=|:)?\s*0x([0-9A-Fa-f]{2}))"),
      QRegularExpression::CaseInsensitiveOption);
  auto match = explicit_nrc.match(message);
  if (match.hasMatch()) {
    bool ok{};
    const auto value = match.captured(1).toUInt(&ok, 16);
    if (ok) return static_cast<std::uint8_t>(value);
  }

  // Raw UDS lines emitted by older flows may not yet carry an NRC label.  A
  // negative response is valid only when 7F is the first UDS payload byte;
  // never scan inside a positive response's Seed, DID value or signature.
  if (!message.trimmed().startsWith(QStringLiteral("RX ["),
                                    Qt::CaseInsensitive)) {
    return std::nullopt;
  }
  static const QRegularExpression raw_negative(
      QStringLiteral(
          R"(^\s*RX\s*\[[^\]]+\]\s+7F\s+[0-9A-Fa-f]{2}\s+([0-9A-Fa-f]{2})(?=\s|$|\|))"),
      QRegularExpression::CaseInsensitiveOption);
  match = raw_negative.match(message);
  if (!match.hasMatch()) return std::nullopt;
  bool ok{};
  const auto value = match.captured(1).toUInt(&ok, 16);
  return ok ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(value))
            : std::nullopt;
}

std::optional<UdsRoutineResult> failedRoutineFromLogLine(
    const QString& message) {
  const auto response_line =
      message.trimmed().startsWith(QStringLiteral("RX ["),
                                   Qt::CaseInsensitive) ||
      message.contains(QStringLiteral("响应")) ||
      message.contains(QStringLiteral("response"), Qt::CaseInsensitive);
  if (!response_line) return std::nullopt;
  static const QRegularExpression raw_result(
      QStringLiteral(
          R"((?:^|\s|\[)71\s+[0-9A-Fa-f]{2}\s+([0-9A-Fa-f]{2})\s+([0-9A-Fa-f]{2})\s+(05)(?=\s|$|\]|\|))"),
      QRegularExpression::CaseInsensitiveOption);
  const auto match = raw_result.match(message);
  if (!match.hasMatch()) return std::nullopt;
  bool high_ok{}, low_ok{}, status_ok{};
  const auto high = match.captured(1).toUInt(&high_ok, 16);
  const auto low = match.captured(2).toUInt(&low_ok, 16);
  const auto status = match.captured(3).toUInt(&status_ok, 16);
  if (!high_ok || !low_ok || !status_ok) return std::nullopt;
  const UdsRoutineResult result{
      static_cast<std::uint16_t>((high << 8U) | low),
      static_cast<std::uint8_t>(status),
      status == 0x05U && ((high << 8U) | low) != 0x0203U};
  return result.failure ? std::optional<UdsRoutineResult>(result)
                        : std::nullopt;
}

QString fullPath(const QLineEdit* path_edit) {
  const auto stored = path_edit->property(kFullPathProperty).toString();
  return stored.isEmpty() ? path_edit->text().trimmed() : stored;
}

void showPath(QLineEdit* path_edit, const QString& path) {
  const auto normalized = path.isEmpty() ? QString{} : QDir::toNativeSeparators(path);
  const auto configured_placeholder =
      path_edit->property(kConfiguredPlaceholderProperty);
  if (!configured_placeholder.isValid()) {
    path_edit->setProperty(kConfiguredPlaceholderProperty,
                           path_edit->placeholderText());
  }
  path_edit->setPlaceholderText(
      normalized.isEmpty()
          ? QString{}
          : path_edit->property(kConfiguredPlaceholderProperty).toString());
  path_edit->setProperty(kFullPathProperty, normalized);
  path_edit->setToolTip(normalized);
  path_edit->setText(normalized.isEmpty()
                         ? QString{}
                         : QFileInfo(normalized).fileName());
  path_edit->setCursorPosition(0);
}

QString selectFile(QWidget* parent, const QLineEdit* pathEdit,
                     const QString& caption, const QString& filter) {
  QString initial_directory = QDir::homePath();
  const auto current_path = fullPath(pathEdit);
  const QFileInfo current(current_path);
  if (!current_path.isEmpty()) {
    initial_directory = current.isDir() ? current.absoluteFilePath()
                                        : current.absolutePath();
  }
  // Open at the containing directory without pre-filling a horizontally
  // scrolled filename.  The Windows native dialog otherwise makes a correct
  // long name look like an unrelated suffix.
  return QFileDialog::getOpenFileName(parent, caption, initial_directory,
                                      filter);
}

QString newestReportPath() {
  QDir logs(QDir(QCoreApplication::applicationDirPath()).filePath(
      QStringLiteral("logs")));
  const auto reports = logs.entryInfoList(
      {QStringLiteral("*.html"), QStringLiteral("*.htm")},
      QDir::Files | QDir::Readable, QDir::Time);
  return reports.isEmpty()
             ? QString{}
             : QDir::toNativeSeparators(reports.front().absoluteFilePath());
}

QString canVendorKey(CanVendor vendor) {
  switch (vendor) {
  case CanVendor::Vector:
    return QStringLiteral("vector");
  case CanVendor::Tosun:
    return QStringLiteral("tosun");
  case CanVendor::Zlg:
    return QStringLiteral("zlg");
  case CanVendor::Kvaser:
    return QStringLiteral("kvaser");
  case CanVendor::Other:
    return QStringLiteral("other");
  }
  return QStringLiteral("vector");
}

CanVendor canVendorFromKey(const QString& key) {
  if (key.compare(QStringLiteral("tosun"), Qt::CaseInsensitive) == 0) {
    return CanVendor::Tosun;
  }
  if (key.compare(QStringLiteral("zlg"), Qt::CaseInsensitive) == 0) {
    return CanVendor::Zlg;
  }
  if (key.compare(QStringLiteral("kvaser"), Qt::CaseInsensitive) == 0) {
    return CanVendor::Kvaser;
  }
  return CanVendor::Vector;
}

QString canChannelSettingsKey(CanVendor vendor) {
  return QStringLiteral("hardware/channel/%1").arg(canVendorKey(vendor));
}

QString canVendorDisplayName(CanVendor vendor) {
  return QString::fromUtf8(can_vendor_name(vendor).data(),
                           static_cast<int>(can_vendor_name(vendor).size()));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(std::make_unique<Ui::MainWindow>()) {
  ui_->setupUi(this);
  configureVisualDesign();
  version_page_ = new VersionConfirmationPage(ui_->workspaceTabWidget);
  ui_->workspaceTabWidget->addTab(version_page_,
                                  QStringLiteral("版本读取"));
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
      add_can_backend(QStringLiteral("Vector XL"), QStringLiteral("vectorCanBackendAction"),
                      CanVendor::Vector, UDS_ENABLE_VECTOR != 0);
  auto* zlg_backend = add_can_backend(
      QStringLiteral("ZLG / ZCANPRO（ZCAN API）"),
      QStringLiteral("zlgCanBackendAction"), CanVendor::Zlg,
      UDS_ENABLE_ZLG != 0);
  auto* tosun_backend = add_can_backend(
      QStringLiteral("TOSUN / TSMaster（TSCAN API）"),
      QStringLiteral("tosunCanBackendAction"), CanVendor::Tosun,
      UDS_ENABLE_TOSUN != 0);
  auto* kvaser_backend = add_can_backend(
      QStringLiteral("Kvaser（CANlib API）"),
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
  controller_bridge_ = std::make_unique<ControllerBridge>();
  for (int index = 0; index < ui_->vectorChannelComboBox->count(); ++index) {
    const auto physical_channel = index + 1;
    ui_->vectorChannelComboBox->setItemText(
        index, QStringLiteral("Channel %1").arg(physical_channel));
    ui_->vectorChannelComboBox->setItemData(index, physical_channel);
  }
  connectControllerActions();
  connectActions();
  {
    QSettings settings;
    ui_->repeatCountSpinBox->setValue(
        std::clamp(
            settings
                .value(QStringLiteral("selectors/repeat_count"),
                       static_cast<int>(uds::app::kMinFlashRepeatCount))
                .toInt(),
            static_cast<int>(uds::app::kMinFlashRepeatCount),
            static_cast<int>(uds::app::kMaxFlashRepeatCount)));
  }
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
QWidget#logColumnHeader {
  background: #f6f8fb;
  border: 1px solid #d8dee8;
  border-bottom: none;
}
QWidget#logColumnHeader QLabel {
  color: #394456;
  font-weight: 600;
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

  ui_->entryModeLabel->setText(QStringLiteral("刷写模式"));
  ui_->driverPathLabel->setText(QStringLiteral("Driver 文件"));
  ui_->driverVerifyPathLabel->setText(QStringLiteral("Driver 校验文件"));
  ui_->appPathLabel->setText(QStringLiteral("APP 文件"));
  ui_->appVerifyPathLabel->setText(QStringLiteral("APP 校验文件"));
  ui_->calPathLabel->setText(QStringLiteral("CAL 文件"));
  ui_->calVerifyPathLabel->setText(QStringLiteral("CAL 校验文件"));
  ui_->seedKeyDllPathLabel->setText(QStringLiteral("SeedKey 算法库"));

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

  auto* log_header = new QWidget(ui_->logGroupBox);
  log_header->setObjectName(QStringLiteral("logColumnHeader"));
  log_header->setFixedHeight(35);
  auto* log_header_layout = new QHBoxLayout(log_header);
  log_header_layout->setContentsMargins(12, 0, 12, 0);
  log_header_layout->setSpacing(12);
  auto* time_header = new QLabel(QStringLiteral("时间"), log_header);
  time_header->setFixedWidth(106);
  auto* direction_header = new QLabel(QStringLiteral("方向"), log_header);
  direction_header->setFixedWidth(58);
  log_header_layout->addWidget(time_header);
  log_header_layout->addWidget(direction_header);
  log_header_layout->addWidget(new QLabel(QStringLiteral("内容"), log_header),
                               1);
  ui_->logLayout->setContentsMargins(10, 10, 10, 10);
  ui_->logLayout->setSpacing(0);
  ui_->logLayout->insertWidget(0, log_header);
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

void MainWindow::connectActions() {
  const auto connectFileButton =
      [this](QPushButton* button, QLineEdit* pathEdit,
              const QString& caption, const QString& filter,
              const QString& logName) {
        button->setProperty("fileDialogFilter", filter);
        connect(button, &QPushButton::clicked, this,
                [this, pathEdit, caption, filter, logName] {
                  const auto selected =
                      selectFile(this, pathEdit, caption, filter);
                  if (selected.isEmpty()) return;
                  showPath(pathEdit, selected);
                  appendUiLog(QStringLiteral("%1路径：%2")
                                  .arg(logName, fullPath(pathEdit)));
                });
      };

  const auto srecordFilter =
      QStringLiteral(
          "刷写文件 (*.s19 *.srec *.s28 *.s37 *.mot *.hex *.bin *.vbf *.cbf);;所有文件 (*.*)");
  const auto verificationFilter =
      QStringLiteral(
          "校验数据 (*.asc *.txt *.rsa *.s19 *.srec *.s28 *.s37 *.mot *.hex *.bin);;所有文件 (*.*)");
  connectFileButton(ui_->driverBrowseButton, ui_->driverPathLineEdit,
                    QStringLiteral("选择 Driver 文件"), srecordFilter,
                    QStringLiteral("Driver"));
  connectFileButton(ui_->driverVerifyBrowseButton,
                    ui_->driverVerifyPathLineEdit,
                    QStringLiteral("选择 DriverData 文件"),
                    verificationFilter, QStringLiteral("DriverData"));
  connectFileButton(ui_->appBrowseButton, ui_->appPathLineEdit,
                    QStringLiteral("选择 APP 文件"), srecordFilter,
                    QStringLiteral("APP"));
  connectFileButton(ui_->appVerifyBrowseButton,
                    ui_->appVerifyPathLineEdit,
                    QStringLiteral("选择 APP 校验文件"),
                    verificationFilter, QStringLiteral("APP 校验文件"));
  connectFileButton(ui_->calBrowseButton, ui_->calPathLineEdit,
                    QStringLiteral("选择 CAL 文件"), srecordFilter,
                    QStringLiteral("CAL"));
  connectFileButton(ui_->calVerifyBrowseButton,
                    ui_->calVerifyPathLineEdit,
                    QStringLiteral("选择 CALData 文件"),
                    verificationFilter, QStringLiteral("CALData"));
  connectFileButton(ui_->seedKeyDllBrowseButton,
                    ui_->seedKeyDllPathLineEdit,
                    QStringLiteral("选择 SeedKey DLL"),
                    QStringLiteral("动态链接库 (*.dll);;所有文件 (*.*)"),
                    QStringLiteral("SeedKey DLL"));

  connect(ui_->probeButton, &QPushButton::clicked, this,
          &MainWindow::startProbeFromUi);
  connect(ui_->startFlashButton, &QPushButton::clicked, this,
          &MainWindow::startFlashFromUi);
  connect(ui_->stopButton, &QPushButton::clicked, this, [this] {
    if (flash_running_) {
      flash_stop_requested_ = controller_bridge_->requestFlashStop();
      if (flash_stop_requested_) {
        ui_->progressStatusLabel->setText(
            QStringLiteral("正在请求中止刷写（等待可中断点）……"));
        appendUiLog(QStringLiteral(
            "已接受中止请求；按钮已锁定，请保持供电并等待当前UDS请求结束和报告生成。"));
      } else {
        appendUiLog(QStringLiteral("刷写任务已经结束，无需再次停止。"));
      }
      updateEnabledState();
    }
    if (probe_running_) {
      controller_bridge_->requestProbeStop();
      ui_->progressStatusLabel->setText(QStringLiteral("正在停止在线探测……"));
      appendUiLog(QStringLiteral("已请求停止在线探测。"));
    }
  });
  connect(ui_->powerOnButton, &QPushButton::clicked, this,
          [this] { requestPowerFromUi(true); });
  connect(ui_->powerOffButton, &QPushButton::clicked, this,
          [this] { requestPowerFromUi(false); });
  connect(ui_->openReportButton, &QPushButton::clicked, this, [this] {
    refreshLatestReportPath();
    if (latest_report_path_.isEmpty()) {
      QMessageBox::information(this, QStringLiteral("没有测试报告"),
                               QStringLiteral("logs目录中尚未找到测试报告。"));
      return;
    }
    appendUiLog(QStringLiteral("打开最新测试报告：%1")
                    .arg(latest_report_path_));
    if (!QDesktopServices::openUrl(
            QUrl::fromLocalFile(latest_report_path_))) {
      QMessageBox::warning(this, QStringLiteral("打开报告失败"),
                           latest_report_path_);
    }
  });

  auto* clear_log_action = new QAction(
      QStringLiteral("清空当前显示（保留历史日志）"), this);
  clear_log_action->setObjectName(QStringLiteral("clearLogAction"));
  connect(clear_log_action, &QAction::triggered, this,
          &MainWindow::clearActiveUiLog);
  ui_->logPlainTextEdit->setContextMenuPolicy(Qt::CustomContextMenu);
  ui_->logPlainTextEdit->installEventFilter(this);
  ui_->logPlainTextEdit->setToolTip(QStringLiteral(
      "Home：日志开头；End：日志末尾；PageUp/PageDown及方向键保持系统默认行为"));
  connect(ui_->logPlainTextEdit, &QPlainTextEdit::customContextMenuRequested,
          this, [this, clear_log_action](const QPoint& position) {
            std::unique_ptr<QMenu> menu(
                ui_->logPlainTextEdit->createStandardContextMenu());
            menu->addSeparator();
            clear_log_action->setEnabled(
                !ui_->logPlainTextEdit->document()->isEmpty());
            menu->addAction(clear_log_action);
            menu->exec(ui_->logPlainTextEdit->mapToGlobal(position));
          });

  connect(ui_->projectComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            populateDeviceOptions(index);
            saveComboSelections();
          });
  connect(ui_->deviceComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            populateTargetOptions(index);
            saveComboSelections();
          });
  connect(ui_->radarComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            applySelectedProfile(index);
            saveComboSelections();
          });
  connect(ui_->vectorChannelComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] {
            saveComboSelections();
            syncVersionContext();
            followSelectedBusMonitorContext();
          });
  connect(ui_->txIdLineEdit, &QLineEdit::editingFinished, this,
          [this] {
            syncVersionContext();
            syncBusMonitorContext();
          });
  connect(ui_->rxIdLineEdit, &QLineEdit::editingFinished, this,
          [this] {
            syncVersionContext();
            syncBusMonitorContext();
          });
  connect(ui_->entryModeComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { saveComboSelections(); });
  connect(ui_->repeatCountSpinBox,
          QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this] { saveComboSelections(); });
}

void MainWindow::connectControllerActions() {
  connect(this, &MainWindow::probeRequested, controller_bridge_.get(),
          &ControllerBridge::startProbe);
  connect(this, &MainWindow::flashRequested, controller_bridge_.get(),
          &ControllerBridge::startFlash);
  connect(this, &MainWindow::powerRequested, controller_bridge_.get(),
           &ControllerBridge::setPower);
  connect(version_page_, &VersionConfirmationPage::checkRequested, this,
          [this](int profile_index, const QString& target_id, unsigned channel,
                 quint32 tx_id, quint32 rx_id) {
            followSelectedBusMonitorContext();
            if (!monitorMatchesSelectedHardware(profile_index)) {
              QMessageBox::warning(
                  this, QStringLiteral("监听通道配置不一致"),
                  QStringLiteral("自动监听未能切换到当前 CAN 后端、通道或速率配置，"
                                 "本次版本读取已阻止。"));
              return;
            }
            controller_bridge_->startVersionCheck(profile_index, target_id,
                                                   channel, tx_id, rx_id);
          });
  connect(version_page_, &VersionConfirmationPage::stopRequested,
          controller_bridge_.get(),
          &ControllerBridge::requestVersionCheckStop);
  connect(version_page_, &VersionConfirmationPage::openReportRequested, this,
          [this] {
            refreshLatestReportPath();
            if (!latest_report_path_.isEmpty()) {
              QDesktopServices::openUrl(
                  QUrl::fromLocalFile(latest_report_path_));
            }
          });
  connect(bus_monitor_page_, &BusMonitorPage::runningChanged, this,
          [this](bool running) {
            bus_monitor_running_ = running;
            updateEnabledState();
          });
  connect(bus_monitor_page_, &BusMonitorPage::monitorMessage, this,
          [this](const QString& message) { appendUiLog(message); });
  connect(controller_bridge_.get(), &ControllerBridge::logMessage, this,
          [this](const QString& message) { appendUiLog(message); });
  connect(controller_bridge_.get(), &ControllerBridge::progressChanged, this,
           [this](int percent, const QString& message) {
             if (probe_running_) {
               // Online detection is a binary verdict: 0 until a validated
               // physical diagnostic response is received, then 100.
               ui_->progressBar->setValue(percent >= 100 ? 100 : 0);
             } else if (flash_running_) {
               flash_progress_ =
                   std::max(flash_progress_, std::clamp(percent, 0, 100));
               ui_->progressBar->setValue(flash_progress_);
             }
              ui_->progressStatusLabel->setText(message);
              if (version_check_running_) statusBar()->showMessage(message);
           });
  connect(controller_bridge_.get(), &ControllerBridge::probeRunningChanged,
          this, [this](bool running) {
            probe_running_ = running;
            updateEnabledState();
             if (running) {
               ui_->progressBar->setValue(0);
               ui_->progressStatusLabel->setText(
                   QStringLiteral("在线探测运行中……"));
            }
          });
  connect(controller_bridge_.get(), &ControllerBridge::probeFinished, this,
          [this](bool success, bool cancelled, const QString& message) {
            handleProbeFinished(success, cancelled, message);
          });
  connect(controller_bridge_.get(), &ControllerBridge::flashRunningChanged,
           this, [this](bool running) {
             flash_running_ = running;
             flash_stop_requested_ = false;
             updateEnabledState();
             if (running) {
               flash_progress_ = 0;
               ui_->progressBar->setValue(0);
               ui_->progressStatusLabel->setText(
                   QStringLiteral("完整刷写运行中……"));
            }
          });
  connect(controller_bridge_.get(), &ControllerBridge::flashFinished, this,
          &MainWindow::handleFlashFinished);
  connect(controller_bridge_.get(),
          &ControllerBridge::versionCheckRunningChanged, this,
          [this](bool running) {
            version_check_running_ = running;
            version_page_->setRunning(running);
            updateEnabledState();
          });
  connect(controller_bridge_.get(), &ControllerBridge::versionCheckRow,
          version_page_, &VersionConfirmationPage::appendResult);
  connect(controller_bridge_.get(), &ControllerBridge::versionCheckFinished,
          this,
          [this](bool success, bool cancelled, const QString& message) {
            version_check_running_ = false;
            version_page_->finish(success, cancelled, message);
            appendUiLog(message);
            updateEnabledState();
            updateStatusBar();
          });
  connect(controller_bridge_.get(), &ControllerBridge::powerRunningChanged,
          this, [this](bool running) {
            power_running_ = running;
            updateEnabledState();
          });
  connect(controller_bridge_.get(), &ControllerBridge::powerFinished, this,
          [this](bool success, const QString& message) {
            power_running_ = false;
            updateEnabledState();
            ui_->progressStatusLabel->setText(message);
            appendUiLog(message);
            if (!success) {
              QMessageBox::warning(this, QStringLiteral("电源操作"), message);
            }
          });
}

void MainWindow::populateProfileOptions() {
  restoring_combo_selections_ = true;
  QSignalBlocker project_blocker(ui_->projectComboBox);
  QSignalBlocker device_blocker(ui_->deviceComboBox);
  QSignalBlocker target_blocker(ui_->radarComboBox);
  ui_->projectComboBox->clear();
  ui_->deviceComboBox->clear();
  ui_->radarComboBox->clear();

  QMap<QString, QVariantList> vendors;
  const auto& profiles = controller_bridge_->profileOptions();
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    vendors[profiles[index].vendor_name].push_back(
        static_cast<int>(index));
  }
  for (auto vendor = vendors.cbegin(); vendor != vendors.cend(); ++vendor) {
    ui_->projectComboBox->addItem(vendor.key(), vendor.value());
  }

  if (ui_->projectComboBox->count() == 0) {
    ui_->projectComboBox->addItem(QStringLiteral("未发现厂商"));
    ui_->deviceComboBox->addItem(QStringLiteral("未发现项目"));
    ui_->radarComboBox->addItem(QStringLiteral("未发现设备"));
    updateEnabledState();
    restoring_combo_selections_ = false;
    return;
  }

  QSettings settings;
  const auto legacy_vendor =
      settings.value(QStringLiteral("selectors/project")).toString();
  auto saved_vendor =
      settings.value(QStringLiteral("selectors/vendor"),
                     legacy_vendor)
          .toString();
  if (legacy_vendor == QStringLiteral("长安C857") ||
      legacy_vendor == QStringLiteral("铃耀_B216") ||
      legacy_vendor == QStringLiteral("铃耀") ||
      saved_vendor == QStringLiteral("长安C857") ||
      saved_vendor == QStringLiteral("铃耀_B216") ||
      saved_vendor == QStringLiteral("铃耀")) {
    saved_vendor = QStringLiteral("长安");
  }
  auto vendor_index = ui_->projectComboBox->findText(saved_vendor);
  if (vendor_index < 0) vendor_index = 0;
  ui_->projectComboBox->setCurrentIndex(vendor_index);
  project_blocker.unblock();
  device_blocker.unblock();
  target_blocker.unblock();
  populateDeviceOptions(vendor_index);
  restoring_combo_selections_ = false;
}

void MainWindow::populateDeviceOptions(int vendor_index) {
  if (vendor_index < 0) return;
  const auto profile_indexes =
      ui_->projectComboBox->itemData(vendor_index).toList();
  QSignalBlocker blocker(ui_->deviceComboBox);
  ui_->deviceComboBox->clear();
  const auto& profiles = controller_bridge_->profileOptions();
  QMap<QString, QVariantList> projects;
  for (const auto& value : profile_indexes) {
    bool valid{};
    const auto profile_index = value.toInt(&valid);
    if (!valid || profile_index < 0 ||
        static_cast<std::size_t>(profile_index) >= profiles.size()) {
      continue;
    }
    projects[profiles[profile_index].project_name].push_back(profile_index);
  }
  for (auto project = projects.cbegin(); project != projects.cend(); ++project) {
    ui_->deviceComboBox->addItem(project.key(), project.value());
  }
  blocker.unblock();
  if (ui_->deviceComboBox->count() > 0) {
    auto project_index = 0;
    if (restoring_combo_selections_) {
      QSettings settings;
      const auto saved_project_name =
          settings.value(QStringLiteral("selectors/project_name")).toString();
      const auto saved_profile_id =
          settings.value(QStringLiteral("selectors/profile_id")).toString();
      for (int index = 0; index < ui_->deviceComboBox->count(); ++index) {
        const auto candidates = ui_->deviceComboBox->itemData(index).toList();
        const auto contains_saved_profile = std::any_of(
            candidates.cbegin(), candidates.cend(),
            [&profiles, &saved_profile_id](const QVariant& value) {
              bool valid{};
              const auto profile_index = value.toInt(&valid);
              return valid && profile_index >= 0 &&
                     static_cast<std::size_t>(profile_index) < profiles.size() &&
                     profiles[profile_index].profile_id == saved_profile_id;
            });
        if ((!saved_project_name.isEmpty() &&
             ui_->deviceComboBox->itemText(index) == saved_project_name) ||
            contains_saved_profile) {
          project_index = index;
          break;
        }
      }
    }
    ui_->deviceComboBox->setCurrentIndex(project_index);
    populateTargetOptions(project_index);
  }
}

void MainWindow::populateTargetOptions(int project_index) {
  if (project_index < 0) return;
  const auto profile_indexes =
      ui_->deviceComboBox->itemData(project_index).toList();
  QSignalBlocker blocker(ui_->radarComboBox);
  ui_->radarComboBox->clear();
  const auto& profiles = controller_bridge_->profileOptions();
  for (const auto& value : profile_indexes) {
    bool valid{};
    const auto profile_index = value.toInt(&valid);
    if (!valid || profile_index < 0 ||
        static_cast<std::size_t>(profile_index) >= profiles.size()) {
      continue;
    }
    const auto& profile = profiles[profile_index];
    if (profile.target_options.empty()) {
      QVariantMap selection;
      selection.insert(QStringLiteral("profile_index"), profile_index);
      selection.insert(QStringLiteral("target_id"), QString{});
      ui_->radarComboBox->addItem(profile.device_name, selection);
      continue;
    }
    for (const auto& target : profile.target_options) {
      QVariantMap selection;
      selection.insert(QStringLiteral("profile_index"), profile_index);
      selection.insert(QStringLiteral("target_id"), target.target_id);
      ui_->radarComboBox->addItem(target.display_name, selection);
    }
  }

  auto target_index = 0;
  if (restoring_combo_selections_) {
    QSettings settings;
    const auto saved_profile_id =
        settings.value(QStringLiteral("selectors/profile_id")).toString();
    const auto saved_target_id =
        settings.value(QStringLiteral("selectors/target/%1")
                           .arg(saved_profile_id))
            .toString();
    for (int index = 0; index < ui_->radarComboBox->count(); ++index) {
      const auto selection = ui_->radarComboBox->itemData(index).toMap();
      bool valid{};
      const auto profile_index =
          selection.value(QStringLiteral("profile_index")).toInt(&valid);
      if (!valid || profile_index < 0 ||
          static_cast<std::size_t>(profile_index) >= profiles.size() ||
          profiles[profile_index].profile_id != saved_profile_id) {
        continue;
      }
      if (saved_target_id.isEmpty() ||
          selection.value(QStringLiteral("target_id")).toString() ==
              saved_target_id) {
        target_index = index;
        break;
      }
    }
  }
  if (ui_->radarComboBox->count() > 0) {
    ui_->radarComboBox->setCurrentIndex(target_index);
  }
  blocker.unblock();
  applySelectedProfile(target_index);
}

void MainWindow::applySelectedProfile(int device_index) {
  if (device_index < 0) return;
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    updateEnabledState();
    return;
  }

  const auto& profile = profiles[profile_index];
  const auto geely_p416 =
      profile.flow_id == QStringLiteral("geely_p416");
  ui_->driverPathLabel->setText(QStringLiteral("Driver 文件"));
  ui_->appPathLabel->setText(
      geely_p416 ? QStringLiteral("APP VBF 文件")
                 : QStringLiteral("APP 文件"));
  ui_->calPathLabel->setText(
      geely_p416 ? QStringLiteral("ESS VBF 文件")
                 : QStringLiteral("CAL 文件"));
  ui_->seedKeyDllPathLabel->setText(
      geely_p416 ? QStringLiteral("SeedKey（内置）")
                 : QStringLiteral("SeedKey 算法库"));
  QSignalBlocker entry_mode_blocker(ui_->entryModeComboBox);
  restoreCurrentBackendChannel(profile.channel);
  ui_->txIdLineEdit->setText(
      QStringLiteral("0x%1").arg(QString::number(profile.tx_id, 16).toUpper()));
  ui_->rxIdLineEdit->setText(
      QStringLiteral("0x%1").arg(QString::number(profile.rx_id, 16).toUpper()));
  ui_->entryModeComboBox->clear();
  if (profile.supports_ft_entry &&
      profile.default_entry_mode == QStringLiteral("auto")) {
    ui_->entryModeComboBox->addItem(QStringLiteral("自动检测"),
                                    QStringLiteral("auto"));
  }
  ui_->entryModeComboBox->addItem(
      profile.app_entry_label.isEmpty()
          ? QStringLiteral("APP")
          : profile.app_entry_label,
      QStringLiteral("app"));
  if (profile.flow_id == QStringLiteral("chuneng_arc331")) {
    ui_->entryModeComboBox->addItem(
        QStringLiteral("BOOT→APP（仅Boot）"), QStringLiteral("boot"));
  }
  if (profile.supports_ft_entry) {
    ui_->entryModeComboBox->addItem(
        profile.ft_entry_label.isEmpty()
            ? QStringLiteral("FT")
            : profile.ft_entry_label,
        QStringLiteral("ft"));
  }
  if (profile.supports_cal_download) {
    const auto concise_chery_labels =
        profile.profile_id == QStringLiteral("chery_ars1_33") ||
        profile.profile_id == QStringLiteral("chery_kp31");
    ui_->entryModeComboBox->addItem(concise_chery_labels
                                        ? QStringLiteral("CAL")
                                        : QStringLiteral("CAL标定刷写"),
                                    QStringLiteral("cal"));
    ui_->entryModeComboBox->addItem(concise_chery_labels
                                        ? QStringLiteral("APP+CAL")
                                        : QStringLiteral("APP+CAL完整刷写"),
                                    QStringLiteral("app_cal"));
  }
  const auto entry_index =
      ui_->entryModeComboBox->findData(profile.default_entry_mode);
  ui_->entryModeComboBox->setCurrentIndex(entry_index < 0 ? 0 : entry_index);
  if (restoring_combo_selections_) {
    QSettings settings;
    const auto saved_profile_id =
        settings.value(QStringLiteral("selectors/profile_id")).toString();
    if (saved_profile_id == profile.profile_id) {
      const auto saved_entry_mode =
          settings.value(QStringLiteral("selectors/entry_mode")).toString();
      const auto saved_entry_index =
          ui_->entryModeComboBox->findData(saved_entry_mode);
      if (saved_entry_index >= 0) {
        ui_->entryModeComboBox->setCurrentIndex(saved_entry_index);
      }
    }
  }
  showPath(ui_->driverPathLineEdit, profile.driver_path);
  showPath(ui_->appPathLineEdit, profile.app_path);
  showPath(ui_->calPathLineEdit, profile.cal_path);
  showPath(ui_->driverVerifyPathLineEdit, profile.driver_verify_path);
  showPath(ui_->appVerifyPathLineEdit, profile.app_verify_path);
  ui_->appVerifyPathLabel->setText(QStringLiteral("APP 校验文件"));
  showPath(ui_->calVerifyPathLineEdit, profile.cal_verify_path);
  showPath(ui_->seedKeyDllPathLineEdit, profile.seed_key_dll_path);
  ui_->txIdLineEdit->setReadOnly(profile.lock_diagnostic_ids);
  ui_->rxIdLineEdit->setReadOnly(profile.lock_diagnostic_ids);
  applySelectedRadar(false);
  activateSelectedLogTarget();
  // Keep every generic file row stable while switching projects.
  updateEnabledState();
  syncVersionContext();
  syncBusMonitorContext();

  const auto target_name = ui_->radarComboBox->currentText();
  appendUiLog(QStringLiteral(
                  "已选择：%1 / %2 / %3；CH%4；TX=%5；RX=%6；FUNC=0x%7")
                  .arg(profile.vendor_name, profile.project_name, target_name)
                  .arg(profile.channel)
                  .arg(ui_->txIdLineEdit->text(),
                       ui_->rxIdLineEdit->text())
                  .arg(QString::number(profile.functional_id, 16).toUpper()));
}

int MainWindow::selectedProfileIndex(bool* valid) const {
  bool local_valid{};
  const auto selection = ui_->radarComboBox->currentData().toMap();
  const auto profile_index =
      selection.value(QStringLiteral("profile_index")).toInt(&local_valid);
  if (valid) *valid = local_valid;
  return local_valid ? profile_index : -1;
}

bool MainWindow::hasRadarSelector() const {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  return valid && profile_index >= 0 &&
         static_cast<std::size_t>(profile_index) < profiles.size() &&
         !profiles[profile_index].target_options.empty();
}

QString MainWindow::selectedTargetId() const {
  return ui_->radarComboBox->currentData()
      .toMap()
      .value(QStringLiteral("target_id"))
      .toString();
}

void MainWindow::applySelectedRadar(bool log_change) {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return;
  }
  if (!hasRadarSelector()) {
    activateSelectedLogTarget();
    syncVersionContext();
    syncBusMonitorContext();
    return;
  }
  const auto target_id = selectedTargetId();
  const auto& targets = profiles[profile_index].target_options;
  const auto selected =
      std::find_if(targets.cbegin(), targets.cend(),
                   [&target_id](const ControllerTargetOption& target) {
                     return target.target_id == target_id;
                   });
  if (selected == targets.cend()) return;
  ui_->txIdLineEdit->setText(
      QStringLiteral("0x%1").arg(QString::number(selected->tx_id, 16).toUpper()));
  ui_->rxIdLineEdit->setText(
      QStringLiteral("0x%1").arg(QString::number(selected->rx_id, 16).toUpper()));
  showPath(ui_->driverPathLineEdit, selected->driver_path);
  showPath(ui_->appPathLineEdit, selected->app_path);
  showPath(ui_->calPathLineEdit, selected->cal_path);
  showPath(ui_->driverVerifyPathLineEdit, selected->driver_verify_path);
  showPath(ui_->appVerifyPathLineEdit, selected->app_verify_path);
  showPath(ui_->calVerifyPathLineEdit, selected->cal_verify_path);
  showPath(ui_->seedKeyDllPathLineEdit, selected->seed_key_dll_path);
  activateSelectedLogTarget();
  if (log_change) {
    appendUiLog(
        QStringLiteral("设备选择：%1；TX=0x%2；RX=0x%3；FUNC=0x%4%5")
            .arg(selected->display_name,
                 QString::number(selected->tx_id, 16).toUpper(),
                 QString::number(selected->rx_id, 16).toUpper(),
                 QString::number(profiles[profile_index].functional_id, 16)
                     .toUpper(),
                 selected->pending_validation
                     ? QStringLiteral("；该端点待台架/实车验证")
                     : QString{}));
  }
  syncVersionContext();
  syncBusMonitorContext();
}

void MainWindow::syncVersionContext(bool recent_flash) {
  if (!version_page_ || !controller_bridge_) return;
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return;
  }
  const auto& profile = profiles[profile_index];
  QString target_name = ui_->radarComboBox->currentText();
  const auto* version_items = &profile.version_items;
  if (hasRadarSelector()) {
    const auto target_id = selectedTargetId();
    const auto selected = std::find_if(
        profile.target_options.cbegin(), profile.target_options.cend(),
        [&target_id](const ControllerTargetOption& target) {
          return target.target_id == target_id;
        });
    if (selected != profile.target_options.cend()) {
      target_name = selected->display_name;
      version_items = &selected->version_items;
    }
  }
  bool tx_ok{};
  bool rx_ok{};
  const auto tx_id = ui_->txIdLineEdit->text().toUInt(&tx_ok, 0);
  const auto rx_id = ui_->rxIdLineEdit->text().toUInt(&rx_ok, 0);
  version_page_->setContext(
      profile_index,
      recent_flash ? QStringLiteral("最近一次成功刷写")
                   : QStringLiteral("当前刷写页选择"),
      canVendorDisplayName(default_can_vendor()),
      profile.vendor_name, profile.project_name, selectedTargetId(), target_name,
      ui_->vectorChannelComboBox->currentData().toUInt(),
      tx_ok ? tx_id : profile.tx_id, rx_ok ? rx_id : profile.rx_id,
      *version_items);
  updateStatusBar();
}

void MainWindow::syncBusMonitorContext() {
  if (!bus_monitor_page_ || !controller_bridge_) return;
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) return;
  const auto& profile = profiles[profile_index];
  bool tx_ok{};
  bool rx_ok{};
  const auto displayed_tx_id = ui_->txIdLineEdit->text().toUInt(&tx_ok, 0);
  const auto displayed_rx_id = ui_->rxIdLineEdit->text().toUInt(&rx_ok, 0);
  std::vector<std::uint32_t> physical_ids{
      tx_ok ? displayed_tx_id : profile.tx_id,
      rx_ok ? displayed_rx_id : profile.rx_id};
  std::vector<std::uint32_t> functional_ids{profile.functional_id};

  auto ft_tx_id = profile.ft_tx_id;
  auto ft_rx_id = profile.ft_rx_id;
  if (hasRadarSelector()) {
    const auto target_id = selectedTargetId();
    const auto selected = std::find_if(
        profile.target_options.cbegin(), profile.target_options.cend(),
        [&target_id](const ControllerTargetOption& target) {
          return target.target_id == target_id;
        });
    if (selected != profile.target_options.cend() &&
        selected->ft_tx_id != 0 && selected->ft_rx_id != 0) {
      ft_tx_id = selected->ft_tx_id;
      ft_rx_id = selected->ft_rx_id;
    }
  }
  if (profile.supports_ft_entry && ft_tx_id != 0 && ft_rx_id != 0) {
    physical_ids.push_back(ft_tx_id);
    physical_ids.push_back(ft_rx_id);
  }
  bus_monitor_page_->setDiagnosticAddressing(std::move(physical_ids),
                                             std::move(functional_ids));
  bus_monitor_page_->setContext(
      default_can_vendor(),
      ui_->vectorChannelComboBox->currentData().toUInt(),
      profile.nominal_bitrate, profile.data_bitrate, profile.can_fd);
}

void MainWindow::followSelectedBusMonitorContext() {
  syncBusMonitorContext();
  if (bus_monitor_page_ && !bus_monitor_page_->isRunning()) {
    bus_monitor_page_->start();
  }
}

bool MainWindow::monitorMatchesSelectedHardware(int profile_index) const {
  if (!bus_monitor_running_ || !bus_monitor_page_ || !controller_bridge_) return true;
  const auto& profiles = controller_bridge_->profileOptions();
  if (profile_index < 0 || static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return false;
  }
  const auto& profile = profiles[static_cast<std::size_t>(profile_index)];
  return bus_monitor_page_->matchesContext(
      default_can_vendor(),
      ui_->vectorChannelComboBox->currentData().toUInt(),
      profile.nominal_bitrate, profile.data_bitrate, profile.can_fd);
}

void MainWindow::updateStatusBar() {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    statusBar()->showMessage(QStringLiteral("未选择项目"));
    return;
  }
  const auto& profile = profiles[profile_index];
  statusBar()->showMessage(
      QStringLiteral("%1 | %2 | %3 | %4 | CH%5 | TX %6 | RX %7 | %8k/%9M | %10")
          .arg(profile.vendor_name, profile.project_name,
               ui_->radarComboBox->currentText())
          .arg(canVendorDisplayName(default_can_vendor()))
          .arg(ui_->vectorChannelComboBox->currentData().toUInt())
          .arg(ui_->txIdLineEdit->text(), ui_->rxIdLineEdit->text())
          .arg(profile.nominal_bitrate / 1000)
          .arg(profile.data_bitrate / 1000000)
          .arg(bus_monitor_running_ ? QStringLiteral("总线监听中")
                                     : version_check_running_ ? QStringLiteral("版本读取中")
                                     : flash_running_
                                           ? QStringLiteral("刷写中")
                                           : QStringLiteral("就绪")));
}

unsigned MainWindow::currentProfileDefaultChannel() const {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  if (!valid || !controller_bridge_) return 1;
  const auto& profiles = controller_bridge_->profileOptions();
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return 1;
  }
  return std::max(1U, profiles[profile_index].channel);
}

void MainWindow::saveCurrentBackendChannel() const {
  bool valid{};
  const auto channel =
      ui_->vectorChannelComboBox->currentData().toUInt(&valid);
  if (!valid || channel == 0) return;
  QSettings settings;
  settings.setValue(canChannelSettingsKey(default_can_vendor()), channel);
}

void MainWindow::restoreCurrentBackendChannel(
    unsigned profile_default_channel) {
  QSettings settings;
  const auto vendor = default_can_vendor();
  const auto key = canChannelSettingsKey(vendor);
  auto fallback =
      vendor == CanVendor::Vector ? std::max(1U, profile_default_channel) : 1U;
  // One-time migration of releases that stored a single global channel.
  if (vendor == CanVendor::Vector && !settings.contains(key) &&
      settings.contains(QStringLiteral("selectors/channel"))) {
    const auto legacy =
        settings.value(QStringLiteral("selectors/channel")).toUInt();
    if (legacy > 0) fallback = legacy;
  }
  const auto channel = std::max(
      1U, settings.value(key, fallback).toUInt());

  QSignalBlocker blocker(ui_->vectorChannelComboBox);
  auto index = ui_->vectorChannelComboBox->findData(channel);
  if (index < 0) {
    ui_->vectorChannelComboBox->addItem(
        QStringLiteral("Channel %1").arg(channel), channel);
    index = ui_->vectorChannelComboBox->count() - 1;
  }
  ui_->vectorChannelComboBox->setCurrentIndex(index);
}

void MainWindow::saveComboSelections() const {
  if (restoring_combo_selections_) return;
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return;
  }

  QSettings settings;
  settings.setValue(QStringLiteral("selectors/vendor"),
                    ui_->projectComboBox->currentText());
  settings.setValue(QStringLiteral("selectors/project_name"),
                    ui_->deviceComboBox->currentText());
  // Keep the old key as a vendor alias so existing installations retain their
  // last selection across this UI-only hierarchy migration.
  settings.setValue(QStringLiteral("selectors/project"),
                    ui_->projectComboBox->currentText());
  settings.setValue(QStringLiteral("selectors/profile_id"),
                    profiles[profile_index].profile_id);
  settings.setValue(
      canChannelSettingsKey(default_can_vendor()),
      ui_->vectorChannelComboBox->currentData());
  settings.setValue(QStringLiteral("selectors/entry_mode"),
                    ui_->entryModeComboBox->currentData());
  settings.setValue(QStringLiteral("selectors/repeat_count"),
                    ui_->repeatCountSpinBox->value());
  if (hasRadarSelector()) {
    settings.setValue(
        QStringLiteral("selectors/target/%1")
            .arg(profiles[profile_index].profile_id),
        selectedTargetId());
  }
}

void MainWindow::startProbeFromUi() {
  bool profile_valid{};
  const auto profile_index = selectedProfileIndex(&profile_valid);
  bool channel_valid{};
  const auto channel =
      ui_->vectorChannelComboBox->currentData().toUInt(&channel_valid);
  bool tx_valid{};
  const auto tx_id = ui_->txIdLineEdit->text().trimmed().toUInt(&tx_valid, 0);
  bool rx_valid{};
  const auto rx_id = ui_->rxIdLineEdit->text().trimmed().toUInt(&rx_valid, 0);
  if (!profile_valid || !channel_valid || !tx_valid || !rx_valid) {
    const auto message = QStringLiteral("在线探测配置错误：设备、CAN物理通道或诊断ID无效。");
    ui_->progressStatusLabel->setText(message);
    appendUiLog(message);
    return;
  }
  followSelectedBusMonitorContext();
  if (!monitorMatchesSelectedHardware(profile_index)) {
    QMessageBox::warning(this, QStringLiteral("监听通道配置不一致"),
                         QStringLiteral("自动监听未能切换到当前 CAN 后端、通道或速率配置，"
                                        "本次在线探测已阻止。"));
    return;
  }

  ui_->progressBar->setValue(0);
  ui_->progressStatusLabel->setText(QStringLiteral("正在启动在线探测……"));
  const auto entry_mode = ui_->entryModeComboBox->currentData().toString();
  appendUiLog(QStringLiteral("请求在线探测：%1CH%2，入口=%3，界面APP端点 TX=0x%4，RX=0x%5；实际寻址按项目探测策略执行")
                  .arg(hasRadarSelector()
                           ? ui_->radarComboBox->currentText() +
                                 QStringLiteral("；")
                           : QString{})
                  .arg(channel)
                  .arg(entry_mode.toUpper())
                  .arg(QString::number(tx_id, 16).toUpper())
                  .arg(QString::number(rx_id, 16).toUpper()));
  emit probeRequested(profile_index, selectedTargetId(), entry_mode, channel,
                      tx_id, rx_id);
}

void MainWindow::startFlashFromUi() {
  bool profile_valid{};
  const auto profile_index = selectedProfileIndex(&profile_valid);
  bool channel_valid{};
  const auto channel =
      ui_->vectorChannelComboBox->currentData().toUInt(&channel_valid);
  bool tx_valid{};
  const auto tx_id = ui_->txIdLineEdit->text().trimmed().toUInt(&tx_valid, 0);
  bool rx_valid{};
  const auto rx_id = ui_->rxIdLineEdit->text().trimmed().toUInt(&rx_valid, 0);
  if (!profile_valid || !channel_valid || !tx_valid || !rx_valid) {
    QMessageBox::warning(this, QStringLiteral("配置错误"),
                         QStringLiteral("设备、CAN通道或诊断ID无效。"));
    return;
  }

  const auto& options = controller_bridge_->profileOptions();
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= options.size()) {
    QMessageBox::warning(this, QStringLiteral("配置错误"),
                         QStringLiteral("设备Profile索引无效。"));
    return;
  }
  const auto& profile = options[static_cast<std::size_t>(profile_index)];
  followSelectedBusMonitorContext();
  if (!monitorMatchesSelectedHardware(profile_index)) {
    QMessageBox::warning(this, QStringLiteral("监听通道配置不一致"),
                         QStringLiteral("自动监听未能切换到当前 CAN 后端、通道或速率配置，"
                                        "本次刷写已阻止。"));
    return;
  }
  const auto entry_mode = ui_->entryModeComboBox->currentData().toString();
  const auto needs_app =
      entry_mode != QStringLiteral("cal");
  const auto needs_cal =
      entry_mode == QStringLiteral("cal") ||
      entry_mode == QStringLiteral("app_cal") ||
      profile.flow_id == QStringLiteral("geely_p416");
  const auto configured_files = std::array{
      std::tuple{QStringLiteral("Boot Driver"), true, profile.driver_path,
                 fullPath(ui_->driverPathLineEdit)},
      std::tuple{QStringLiteral("Driver Data"),
                 !profile.driver_verify_path.isEmpty(),
                 profile.driver_verify_path,
                 fullPath(ui_->driverVerifyPathLineEdit)},
      std::tuple{QStringLiteral("APP"), needs_app, profile.app_path,
                 fullPath(ui_->appPathLineEdit)},
      std::tuple{profile.app_verify_label.isEmpty()
                     ? QStringLiteral("APP Data")
                     : profile.app_verify_label,
                 needs_app,
                 profile.app_verify_path,
                 fullPath(ui_->appVerifyPathLineEdit)},
      std::tuple{QStringLiteral("CAL"), needs_cal, profile.cal_path,
                 fullPath(ui_->calPathLineEdit)},
      std::tuple{QStringLiteral("CAL Data"), needs_cal,
                 profile.cal_verify_path,
                 fullPath(ui_->calVerifyPathLineEdit)},
      std::tuple{QStringLiteral("SeedKey"), true,
                 profile.seed_key_dll_path,
                 fullPath(ui_->seedKeyDllPathLineEdit)}};
  for (const auto& [label, required, configured_path, selected_path] :
       configured_files) {
    if (required && !configured_path.isEmpty() &&
        (selected_path.isEmpty() || !QFileInfo::exists(selected_path))) {
      QMessageBox::warning(
          this, QStringLiteral("刷写文件无效"),
          QStringLiteral("%1 文件不存在：\n%2").arg(label, selected_path));
      return;
    }
  }

  const auto repeat_count =
      static_cast<unsigned>(ui_->repeatCountSpinBox->value());
  if (profile.supports_ft_entry && entry_mode == QStringLiteral("ft")) {
    appendUiLog(QStringLiteral(
        "提示：已选择FT恢复入口，将使用当前目标Profile配置的FT端点切换，"
        "随后继续执行Driver与APP下载。"));
  }
  if (profile.flow_id == QStringLiteral("chuneng_arc331") &&
      entry_mode == QStringLiteral("boot")) {
    appendUiLog(QStringLiteral(
        "提示：已选择BOOT→APP入口；使用当前设备物理诊断ID，跳过APP态0203/85/28，"
        "正式刷写全程保持0x520/500ms唤醒。"));
  }
  if (entry_mode == QStringLiteral("cal")) {
    appendUiLog(QStringLiteral(
        "提示：CAL模式将按CANoe顺序执行 Driver + CAL，APP文件不会下载。"));
  } else if (entry_mode == QStringLiteral("app_cal")) {
    appendUiLog(QStringLiteral(
        "提示：APP+CAL模式将按CANoe顺序执行 Driver + APP + CAL。"));
  }

  flash_progress_ = 0;
  ui_->progressBar->setValue(0);
  ui_->progressStatusLabel->setText(QStringLiteral("正在启动完整刷写……"));
  const auto flash_target = QStringLiteral("%1 / %2 / %3")
                                .arg(profile.vendor_name,
                                     profile.project_name,
                                     ui_->radarComboBox->currentText());
  appendUiLog(QStringLiteral(
                  "直接开始刷写：%1，次数 %2，CH%3，TX 0x%4 -> RX 0x%5，FUNC 0x%6，模式 %7")
                    .arg(flash_target)
                    .arg(repeat_count)
                    .arg(channel)
                   .arg(QString::number(tx_id, 16).toUpper())
                   .arg(QString::number(rx_id, 16).toUpper())
                   .arg(QString::number(profile.functional_id, 16).toUpper())
                    .arg(entry_mode.toUpper()));
  emit flashRequested(
       profile_index, selectedTargetId(), entry_mode, repeat_count, channel,
       tx_id, rx_id, fullPath(ui_->driverPathLineEdit),
       fullPath(ui_->appPathLineEdit), fullPath(ui_->calPathLineEdit),
       fullPath(ui_->driverVerifyPathLineEdit),
       fullPath(ui_->appVerifyPathLineEdit),
       fullPath(ui_->calVerifyPathLineEdit),
       fullPath(ui_->seedKeyDllPathLineEdit));
}

void MainWindow::requestPowerFromUi(bool enabled) {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  if (!valid) return;
  if (QMessageBox::question(
          this, enabled ? QStringLiteral("上电确认")
                        : QStringLiteral("下电确认"),
          enabled ? QStringLiteral("确认通过CANoe DOUT给当前台架上电？")
                  : QStringLiteral("确认通过CANoe DOUT给当前台架下电？"),
          QMessageBox::Yes | QMessageBox::No,
          QMessageBox::No) != QMessageBox::Yes) {
    return;
  }
  emit powerRequested(profile_index, enabled);
}

void MainWindow::updateEnabledState() {
  const auto busy = probe_running_ || flash_running_ || power_running_ ||
                    version_check_running_;
  const auto& profiles = controller_bridge_->profileOptions();
  bool profile_valid{};
  const auto profile_index = selectedProfileIndex(&profile_valid);
  const auto usable = profile_valid && profile_index >= 0 &&
                      static_cast<std::size_t>(profile_index) < profiles.size() &&
                      !profiles[profile_index].placeholder;
  const auto has_target_choices =
      profile_valid && profile_index >= 0 &&
      static_cast<std::size_t>(profile_index) < profiles.size() &&
      ui_->radarComboBox->count() > 1;
  const auto controls_power = usable && profiles[profile_index].power_control;
  const auto diagnostic_ids_locked =
      usable && profiles[profile_index].lock_diagnostic_ids;
  const auto has_profiles = !profiles.empty();

  // A passive monitor may switch backends safely: the backend action stops the
  // old receive thread/trace and restarts on the new vendor. Keep switching
  // locked only while probe/flash/power/version operations are active.
  if (can_backend_group_) can_backend_group_->setEnabled(!busy);
  ui_->projectComboBox->setEnabled(!busy && has_profiles);
  ui_->deviceComboBox->setEnabled(!busy && has_profiles);
  ui_->radarComboBox->setEnabled(
      !busy && has_target_choices);
  // The passive monitor follows selector changes in syncBusMonitorContext():
  // BusMonitorPage::setContext() stops the old channel and restarts on the new
  // one.  Keeping this selector disabled while monitoring made the automatic
  // startup monitor effectively lock every project to its initial channel.
  ui_->vectorChannelComboBox->setEnabled(!busy && usable);
  ui_->txIdLineEdit->setEnabled(!busy && usable);
  ui_->rxIdLineEdit->setEnabled(!busy && usable);
  ui_->txIdLineEdit->setReadOnly(diagnostic_ids_locked);
  ui_->rxIdLineEdit->setReadOnly(diagnostic_ids_locked);
  ui_->entryModeComboBox->setEnabled(!busy && profile_valid);
  ui_->repeatCountSpinBox->setEnabled(!busy && usable);
  // Placeholder profiles remain fail-closed for CAN operations, but file
  // selection is an offline preparation action and must stay available.
  ui_->filesGroupBox->setEnabled(!busy && profile_valid);
  ui_->probeButton->setEnabled(!busy && usable);
  ui_->startFlashButton->setEnabled(!busy && usable);
  ui_->startFlashButton->setText(flash_running_
                                     ? QStringLiteral("刷写中…")
                                     : QStringLiteral("开始刷写"));
  ui_->powerOnButton->setEnabled(!busy && controls_power);
  ui_->powerOffButton->setEnabled(!busy && controls_power);
  ui_->stopButton->setText(flash_stop_requested_
                               ? QStringLiteral("正在停止…")
                               : QStringLiteral("停止"));
  ui_->stopButton->setEnabled((probe_running_ || flash_running_) &&
                              !flash_stop_requested_);
  ui_->openReportButton->setEnabled(!busy && !latest_report_path_.isEmpty());
  if (version_page_) {
    version_page_->setReportAvailable(!latest_report_path_.isEmpty());
    version_page_->setOperationBusy(busy && !version_check_running_);
  }
  if (bus_monitor_page_) {
    bus_monitor_page_->setOperationBusy(busy);
  }
  updateStatusBar();
}

void MainWindow::initializeExecutionLog() {
  QDir application_directory(QCoreApplication::applicationDirPath());
  if (!application_directory.mkpath(QStringLiteral("logs"))) return;
  const auto file_name = QStringLiteral("execution_%1.log")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd_HHmmss_zzz")));
  auto file = std::make_unique<QFile>(
      application_directory.filePath(QStringLiteral("logs/%1").arg(file_name)));
  if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
    return;
  }
  setProperty("executionLogPath", file->fileName());
  execution_log_file_ = std::move(file);
}

void MainWindow::refreshLatestReportPath() {
  latest_report_path_ = newestReportPath();
  setProperty("latestReportPath", latest_report_path_);
  if (ui_) updateEnabledState();
}

void MainWindow::appendUiLog(const QString& message, UiLogTone tone) {
  const auto now = QDateTime::currentDateTime();
  auto displayed_message = message;
  const auto nrc = nrcFromLogLine(displayed_message);
  if (nrc) {
    // ResponsePending is expected while the ECU processes long-running
    // services (ARC331 returns it for every TransferData block). The raw frame
    // remains available in the bus monitor and ASC trace; suppressing it here
    // keeps the operator log focused on final responses and actual failures.
    if (*nrc == 0x78U) return;
    // Remove the old ambiguous prefix once a concrete ECU NRC is known.
    displayed_message.replace(QStringLiteral("NRC/timeout "), QString{},
                              Qt::CaseInsensitive);
    displayed_message.replace(QStringLiteral("negative response "),
                              QString{}, Qt::CaseInsensitive);
    const auto detail = format_uds_nrc(*nrc);
    const auto nrc_name = uds_nrc_name(*nrc);
    if (!displayed_message.contains(
            QString::fromLatin1(nrc_name.data(),
                                static_cast<int>(nrc_name.size())),
            Qt::CaseInsensitive)) {
      displayed_message += QStringLiteral(" | %1").arg(
          QString::fromUtf8(detail.data(), static_cast<int>(detail.size())));
    }
    if (tone == UiLogTone::Normal) tone = UiLogTone::Failure;
  } else if (const auto routine = failedRoutineFromLogLine(displayed_message)) {
    const auto detail = format_uds_routine_result(*routine);
    const auto detail_text = QString::fromUtf8(
        detail.data(), static_cast<int>(detail.size()));
    if (!displayed_message.contains(detail_text)) {
      displayed_message += QStringLiteral(" | %1").arg(detail_text);
    }
    if (tone == UiLogTone::Normal) tone = UiLogTone::Failure;
  } else if (tone == UiLogTone::Normal &&
             (displayed_message.contains(QStringLiteral("失败")) ||
              displayed_message.contains(QStringLiteral("ERROR"),
                                          Qt::CaseInsensitive) ||
              displayed_message.contains(QStringLiteral("FATAL"),
                                          Qt::CaseInsensitive) ||
              displayed_message.contains(QStringLiteral("NRC/timeout"),
                                          Qt::CaseInsensitive) ||
              displayed_message.contains(QStringLiteral("negative response"),
                                          Qt::CaseInsensitive))) {
    tone = UiLogTone::Failure;
  }
  const auto display_line = QStringLiteral("[%1] %2")
                                .arg(now.toString(QStringLiteral("HH:mm:ss")),
                                     displayed_message);
  if (active_log_target_key_.isEmpty()) {
    active_log_target_key_ = selectedLogTargetKey();
    if (active_log_target_key_.isEmpty()) {
      active_log_target_key_ = QStringLiteral("__application__");
    }
  }
  auto& target_entries = target_log_entries_[active_log_target_key_];
  target_entries.push_back(UiLogEntry{display_line, tone});
  constexpr qsizetype kMaximumUiLogLinesPerTarget = 5000;
  while (target_entries.size() > kMaximumUiLogLinesPerTarget) {
    target_entries.removeFirst();
  }
  auto* scrollbar = ui_->logPlainTextEdit->verticalScrollBar();
  const auto old_scroll_value = scrollbar->value();
  const auto follow_tail = old_scroll_value >= scrollbar->maximum();
  const auto old_cursor = ui_->logPlainTextEdit->textCursor();
  appendUiLogEntryToView(target_entries.back());
  if (follow_tail) {
    scrollbar->setValue(scrollbar->maximum());
  } else {
    ui_->logPlainTextEdit->setTextCursor(old_cursor);
    scrollbar->setValue(old_scroll_value);
  }
  if (execution_log_file_ && execution_log_file_->isOpen()) {
    const auto persisted_line = QStringLiteral("[%1] %2\r\n")
                                    .arg(now.toString(QStringLiteral(
                                         "yyyy-MM-dd HH:mm:ss.zzz")),
                                         displayed_message)
                                    .toUtf8();
    execution_log_file_->write(persisted_line);
    execution_log_file_->flush();
  }
}

void MainWindow::appendUiLogEntryToView(const UiLogEntry& entry) {
  QTextCharFormat format;
  switch (entry.tone) {
  case UiLogTone::Success:
    format.setForeground(QColor(QStringLiteral("#16803C")));
    format.setFontWeight(QFont::Bold);
    break;
  case UiLogTone::Failure:
    format.setForeground(QColor(QStringLiteral("#C62828")));
    format.setFontWeight(QFont::Bold);
    break;
  case UiLogTone::Pending:
    format.setForeground(QColor(QStringLiteral("#A85D00")));
    format.setFontWeight(QFont::Bold);
    break;
  case UiLogTone::Normal:
    break;
  }

  auto cursor = ui_->logPlainTextEdit->textCursor();
  cursor.movePosition(QTextCursor::End);
  if (!ui_->logPlainTextEdit->document()->isEmpty()) cursor.insertBlock();
  if (entry.tone != UiLogTone::Normal) cursor.setBlockCharFormat(format);
  cursor.insertText(entry.text, format);
  ui_->logPlainTextEdit->setTextCursor(cursor);
}

void MainWindow::renderActiveUiLog() {
  ui_->logPlainTextEdit->clear();
  const auto entries = target_log_entries_.value(active_log_target_key_);
  for (const auto& entry : entries) appendUiLogEntryToView(entry);
}

void MainWindow::handleFlashFinished(bool success, bool cancelled,
                                     const QString& message,
                                     const QString& report_path) {
  flash_running_ = false;
  if (!report_path.isEmpty() && QFileInfo::exists(report_path)) {
    latest_report_path_ = QDir::toNativeSeparators(report_path);
  } else {
    refreshLatestReportPath();
  }
  updateEnabledState();
  ui_->progressStatusLabel->setText(message);
  appendUiLog(message, success ? UiLogTone::Success : UiLogTone::Failure);
  if (!report_path.isEmpty()) {
    appendUiLog(QStringLiteral("报告：%1").arg(report_path));
  }

  // Keep the final result visible in the execution log.  This remains a UI
  // concern: workflows only report success/cancelled/message and do not know
  // about colors or presentation.
  if (cancelled) {
    appendUiLog(QStringLiteral(
        "安全提示：刷写已中断，ECU恢复状态未确认。请保持供电，"
        "先查看报告最后成功步骤并按项目恢复流程处理。"));
    appendUiLog(QStringLiteral("========== 刷写已中止 =========="),
                UiLogTone::Failure);
  } else if (success) {
    ui_->progressBar->setValue(100);
    syncVersionContext(true);
    appendUiLog(QStringLiteral("========== 刷写成功 =========="),
                UiLogTone::Success);
  } else {
    appendUiLog(QStringLiteral(
        "安全提示：若失败发生在进入编程会话之后，ECU恢复状态未确认。"
        "请保持供电，先查看报告最后成功步骤，再按项目恢复流程处理。"));
    appendUiLog(QStringLiteral("========== 刷写失败 =========="),
                UiLogTone::Failure);
  }
}

void MainWindow::handleProbeFinished(bool success, bool cancelled,
                                     const QString& message) {
  probe_running_ = false;
  updateEnabledState();
  ui_->progressBar->setValue(success ? 100 : 0);
  ui_->progressStatusLabel->setText(message);
  if (cancelled) {
    appendUiLog(QStringLiteral("在线探测已停止：%1").arg(message));
  } else if (success) {
    appendUiLog(QStringLiteral("● 在线：%1").arg(message),
                UiLogTone::Success);
  } else {
    appendUiLog(QStringLiteral("● 不在线：%1").arg(message),
                UiLogTone::Failure);
  }
}

QString MainWindow::selectedLogTargetKey() const {
  if (!controller_bridge_ || !ui_) return {};
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return {};
  }
  const auto& profile = profiles[profile_index];
  const auto target_id = selectedTargetId();
  return target_id.isEmpty()
             ? profile.profile_id
             : QStringLiteral("%1/%2").arg(profile.profile_id, target_id);
}

void MainWindow::activateSelectedLogTarget() {
  const auto target_key = selectedLogTargetKey();
  if (target_key.isEmpty() || target_key == active_log_target_key_) return;
  active_log_target_key_ = target_key;
  renderActiveUiLog();
  auto cursor = ui_->logPlainTextEdit->textCursor();
  cursor.movePosition(QTextCursor::End);
  ui_->logPlainTextEdit->setTextCursor(cursor);
  ui_->logPlainTextEdit->ensureCursorVisible();
}

void MainWindow::clearActiveUiLog() {
  if (!active_log_target_key_.isEmpty()) {
    target_log_entries_.remove(active_log_target_key_);
  }
  ui_->logPlainTextEdit->clear();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
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
    const auto modifiers = key_event->modifiers();
    if (modifiers == Qt::NoModifier || modifiers == Qt::ControlModifier) {
      auto cursor = ui_->logPlainTextEdit->textCursor();
      if (key_event->key() == Qt::Key_Home) {
        cursor.movePosition(QTextCursor::Start);
        ui_->logPlainTextEdit->setTextCursor(cursor);
        ui_->logPlainTextEdit->ensureCursorVisible();
        return true;
      }
      if (key_event->key() == Qt::Key_End) {
        cursor.movePosition(QTextCursor::End);
        ui_->logPlainTextEdit->setTextCursor(cursor);
        ui_->logPlainTextEdit->ensureCursorVisible();
        return true;
      }
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!probe_running_ && !flash_running_ && !power_running_ &&
      !bus_monitor_running_) {
    event->accept();
    return;
  }
  if (bus_monitor_running_ && !probe_running_ && !flash_running_ &&
      !power_running_) {
    bus_monitor_page_->stop();
    event->accept();
    return;
  }
  if (power_running_ && !probe_running_ && !flash_running_) {
    QMessageBox::information(
        this, QStringLiteral("电源控制仍在运行"),
        QStringLiteral("当前电源控制命令必须先完成，窗口暂不关闭。"));
    event->ignore();
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
