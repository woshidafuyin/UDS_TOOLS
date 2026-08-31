#include "ui/qt/main_window.hpp"
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
namespace {

constexpr auto kFullPathProperty = "fullPath";
constexpr auto kConfiguredPlaceholderProperty = "configuredPathPlaceholder";
constexpr auto kEmbeddedVerificationProperty = "embeddedVerification";
constexpr auto kPackageValidProperty = "appPackageValid";
constexpr auto kFlashFileFieldProperty = "flashFileField";

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
  const auto parsed = parseUiLogMessage(message);
  if (parsed.direction != LogDirection::Rx) {
    return std::nullopt;
  }
  static const QRegularExpression raw_negative(
      QStringLiteral(
          R"(^\s*7F\s+[0-9A-Fa-f]{2}\s+([0-9A-Fa-f]{2})(?=\s|$|\|))"),
      QRegularExpression::CaseInsensitiveOption);
  match = raw_negative.match(parsed.payload);
  if (!match.hasMatch()) return std::nullopt;
  bool ok{};
  const auto value = match.captured(1).toUInt(&ok, 16);
  return ok ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(value))
            : std::nullopt;
}

std::optional<UdsRoutineResult> failedRoutineFromLogLine(
    const QString& message) {
  const auto parsed = parseUiLogMessage(message);
  const auto response_line =
      parsed.direction == LogDirection::Rx ||
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
  // Embedded TMP verification is presentation state, not an external file.
  // Never let the user-facing summary fall back into the execution path.
  if (path_edit->property(kEmbeddedVerificationProperty).toBool()) return {};
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
      QStringLiteral("logs/reports")));
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

QString settingsKeyComponent(const QString& value) {
  return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString vendorProjectSettingsKey(const QString& vendor) {
  return QStringLiteral("selectors/vendor_projects/%1")
      .arg(settingsKeyComponent(vendor));
}

QString projectDeviceSettingsGroup(const QString& vendor,
                                   const QString& project) {
  return QStringLiteral("selectors/project_devices/%1/%2")
      .arg(settingsKeyComponent(vendor), settingsKeyComponent(project));
}

QString profileStateSettingsGroup(const QString& profile_target_key) {
  return QStringLiteral("selectors/profile_state/%1").arg(profile_target_key);
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(std::make_unique<Ui::MainWindow>()) {
  ui_->setupUi(this);
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
              const QString& logName, FlashFileField field) {
        button->setProperty("fileDialogFilter", filter);
        connect(button, &QPushButton::clicked, this,
                [this, pathEdit, caption, filter, logName, field] {
                  const auto selected =
                      selectFile(this, pathEdit, caption, filter);
                  if (selected.isEmpty()) return;
                  storeSelectedFlashFile(field, selected, pathEdit, logName);
                });
      };

  const auto srecordFilter =
      QStringLiteral(
          "刷写文件 (*.s19 *.srec *.s28 *.s37 *.mot *.hex *.bin *.vbf *.cbf);;所有文件 (*.*)");
  const auto verificationFilter =
      QStringLiteral(
          "校验数据 (*.asc *.tmp *.txt *.rsa *.s19 *.srec *.s28 *.s37 *.mot *.hex *.bin);;所有文件 (*.*)");
  connectFileButton(ui_->driverBrowseButton, ui_->driverPathLineEdit,
                    QStringLiteral("选择 Driver 文件"), srecordFilter,
                    QStringLiteral("Driver"), FlashFileField::Driver);
  connectFileButton(ui_->driverVerifyBrowseButton,
                    ui_->driverVerifyPathLineEdit,
                    QStringLiteral("选择 DriverData 文件"),
                    verificationFilter, QStringLiteral("DriverData"),
                    FlashFileField::DriverVerify);
  const auto appPackageFilter =
      QStringLiteral(
          "APP 文件/升级包 (*.s19 *.srec *.s28 *.s37 *.mot *.hex *.bin *.tmp);;"
          "零跑 TMP 升级包 (*.tmp);;S-record/二进制 (*.s19 *.srec *.s28 *.s37 *.mot *.hex *.bin);;"
          "所有文件 (*.*)");
  ui_->appBrowseButton->setProperty("fileDialogFilter", appPackageFilter);
  connect(ui_->appBrowseButton, &QPushButton::clicked, this,
          [this, srecordFilter, appPackageFilter] {
            const auto supports_package =
                selectedProfileSupportsAppTmpPackage();
            const auto selected = selectFile(
                this, ui_->appPathLineEdit,
                supports_package ? QStringLiteral("选择 APP 文件或 TMP 升级包")
                                 : QStringLiteral("选择 APP 文件"),
                supports_package ? appPackageFilter : srecordFilter);
            if (selected.isEmpty()) return;
            if (!storeSelectedFlashFile(FlashFileField::App, selected,
                                        ui_->appPathLineEdit,
                                        QStringLiteral("APP"))) {
              return;
            }
            if (supports_package) {
              // A newly selected standalone APP must never silently reuse a
              // certificate that belonged to the previous APP/package.
              showPath(ui_->appVerifyPathLineEdit, {});
              saveRuntimeFileSelection();
            }
            updateAppPackagePresentation(true);
          });
  ui_->appVerifyBrowseButton->setProperty("fileDialogFilter",
                                          verificationFilter);
  connect(ui_->appVerifyBrowseButton, &QPushButton::clicked, this,
          [this, verificationFilter] {
            if (ui_->appVerifyPathLineEdit
                    ->property(kEmbeddedVerificationProperty)
                    .toBool()) {
              QMessageBox::information(
                  this, QStringLiteral("TMP 内置验签详情"),
                  ui_->appVerifyPathLineEdit->toolTip());
              return;
            }
            const auto selected = selectFile(
                this, ui_->appVerifyPathLineEdit,
                QStringLiteral("选择 APP 校验文件"), verificationFilter);
            if (selected.isEmpty()) return;
            storeSelectedFlashFile(FlashFileField::AppVerify, selected,
                                   ui_->appVerifyPathLineEdit,
                                   QStringLiteral("APP 校验文件"));
          });
  connectFileButton(ui_->calBrowseButton, ui_->calPathLineEdit,
                    QStringLiteral("选择 CAL 文件"), srecordFilter,
                    QStringLiteral("CAL"), FlashFileField::Cal);
  connectFileButton(ui_->calVerifyBrowseButton,
                    ui_->calVerifyPathLineEdit,
                    QStringLiteral("选择 CALData 文件"),
                    verificationFilter, QStringLiteral("CALData"),
                    FlashFileField::CalVerify);
  connectFileButton(ui_->seedKeyDllBrowseButton,
                    ui_->seedKeyDllPathLineEdit,
                    QStringLiteral("选择 SeedKey DLL"),
                    QStringLiteral("动态链接库 (*.dll);;所有文件 (*.*)"),
                    QStringLiteral("SeedKey DLL"), FlashFileField::SeedKeyDll);

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
  auto* log_scrollbar = ui_->logPlainTextEdit->verticalScrollBar();
  connect(log_scrollbar, &QScrollBar::actionTriggered, this,
          [this, log_scrollbar](int) {
            // Only user navigation emits actionTriggered; setValue() used by
            // tail following does not. Dragging or scrolling to the bottom
            // therefore resumes following, while moving upward pauses it.
            execution_log_follow_tail_ =
                log_scrollbar->sliderPosition() >= log_scrollbar->maximum();
          });
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
            saveActiveProfileState();
            saveComboSelections();
            syncVersionContext();
            syncDiagnosticRequestContext();
            followSelectedBusMonitorContext();
          });
  connect(ui_->txIdLineEdit, &QLineEdit::editingFinished, this,
          [this] {
            saveActiveProfileState();
            syncVersionContext();
            syncBusMonitorContext();
            syncDiagnosticRequestContext();
          });
  connect(ui_->rxIdLineEdit, &QLineEdit::editingFinished, this,
          [this] {
            saveActiveProfileState();
            syncVersionContext();
            syncBusMonitorContext();
            syncDiagnosticRequestContext();
          });
  connect(ui_->entryModeComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] {
            saveActiveProfileState();
            saveComboSelections();
          });
  connect(ui_->repeatCountSpinBox,
          QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this] {
            saveActiveProfileState();
            saveComboSelections();
          });
}

void MainWindow::connectControllerActions() {
  connect(this, &MainWindow::probeRequested, controller_bridge_.get(),
          &ControllerBridge::startProbe);
  connect(this, &MainWindow::flashRequested, controller_bridge_.get(),
          &ControllerBridge::startFlash);
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
  connect(diagnostic_request_page_, &DiagnosticRequestPage::sendRequested,
          this,
          [this](int profile_index, const QString& target_id, unsigned channel,
                 quint32 tx_id, quint32 rx_id, const QString& payload,
                 unsigned timeout_ms) {
            followSelectedBusMonitorContext();
            if (!monitorMatchesSelectedHardware(profile_index)) {
              QMessageBox::warning(
                  this, QStringLiteral("监听通道配置不一致"),
                  QStringLiteral("自动监听未能切换到当前 CAN 后端、通道或速率配置，"
                                 "本次诊断请求已阻止。"));
              return;
            }
            controller_bridge_->startDiagnosticRequest(
                profile_index, target_id, channel, tx_id, rx_id, payload,
                timeout_ms);
          });
  connect(diagnostic_request_page_, &DiagnosticRequestPage::stopRequested,
          controller_bridge_.get(), &ControllerBridge::requestDiagnosticStop);
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
          &MainWindow::handleProgressChanged);
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
          &MainWindow::handleVersionCheckRunningChanged);
  connect(controller_bridge_.get(), &ControllerBridge::versionCheckRow,
          version_page_, &VersionConfirmationPage::appendResult);
  connect(controller_bridge_.get(), &ControllerBridge::versionCheckFinished,
          this, &MainWindow::handleVersionCheckFinished);
  connect(controller_bridge_.get(),
          &ControllerBridge::diagnosticRunningChanged, this,
          [this](bool running) {
            diagnostic_request_running_ = running;
            diagnostic_request_page_->setRunning(running);
            updateEnabledState();
          });
  connect(controller_bridge_.get(), &ControllerBridge::diagnosticFinished,
          this,
          [this](bool success, bool cancelled, const QString& request,
                 const QString& response, const QString& detail,
                 unsigned elapsed_ms, quint8 nrc) {
            diagnostic_request_running_ = false;
            diagnostic_request_page_->finish(success, cancelled, request,
                                              response, detail, elapsed_ms, nrc);
            appendUiLog(QStringLiteral("诊断报文：TX %1；RX %2；%3")
                            .arg(request,
                                 response.isEmpty() ? QStringLiteral("<无响应>")
                                                    : response,
                                 detail),
                        success ? UiLogTone::Success : UiLogTone::Failure);
            updateEnabledState();
            updateStatusBar();
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
  if (vendor_index < 0) {
    vendor_index = 0;
    for (int candidate = 0; candidate < ui_->projectComboBox->count();
         ++candidate) {
      const auto profile_indexes =
          ui_->projectComboBox->itemData(candidate).toList();
      const auto has_usable_profile = std::any_of(
          profile_indexes.cbegin(), profile_indexes.cend(),
          [&profiles](const QVariant& value) {
            bool valid{};
            const auto profile_index = value.toInt(&valid);
            return valid && profile_index >= 0 &&
                   static_cast<std::size_t>(profile_index) < profiles.size() &&
                   !profiles[profile_index].placeholder;
          });
      if (has_usable_profile) {
        vendor_index = candidate;
        break;
      }
    }
  }
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
  auto project_index = 0;
  {
    QSignalBlocker blocker(ui_->deviceComboBox);
    ui_->deviceComboBox->clear();
    for (auto project = projects.cbegin(); project != projects.cend();
         ++project) {
      ui_->deviceComboBox->addItem(project.key(), project.value());
    }
    if (ui_->deviceComboBox->count() == 0) return;

    QSettings settings;
    const auto vendor_name = ui_->projectComboBox->itemText(vendor_index);
    auto saved_project_name =
        settings.value(vendorProjectSettingsKey(vendor_name)).toString();
    const auto saved_profile_id =
        restoring_combo_selections_
            ? settings.value(QStringLiteral("selectors/profile_id")).toString()
            : QString{};
    if (saved_project_name.isEmpty() && restoring_combo_selections_) {
      saved_project_name =
          settings.value(QStringLiteral("selectors/project_name")).toString();
    }
    if (!saved_profile_id.isEmpty()) {
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
        if (contains_saved_profile) {
          project_index = index;
          break;
        }
      }
    } else if (!saved_project_name.isEmpty()) {
      const auto saved_project_index =
          ui_->deviceComboBox->findText(saved_project_name);
      if (saved_project_index >= 0) project_index = saved_project_index;
    }
    ui_->deviceComboBox->setCurrentIndex(project_index);
  }
  // The device signal was blocked while rebuilding the list, so apply the
  // selected project exactly once here.
  populateTargetOptions(project_index);
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
  QSettings settings;
  const auto device_group = projectDeviceSettingsGroup(
      ui_->projectComboBox->currentText(), ui_->deviceComboBox->currentText());
  auto saved_profile_id =
      settings.value(device_group + QStringLiteral("/profile_id")).toString();
  auto saved_target_id =
      settings.value(device_group + QStringLiteral("/target_id")).toString();
  if (saved_profile_id.isEmpty() && restoring_combo_selections_) {
    saved_profile_id =
        settings.value(QStringLiteral("selectors/profile_id")).toString();
    saved_target_id =
        settings.value(QStringLiteral("selectors/target/%1")
                           .arg(saved_profile_id))
            .toString();
  }
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
  if (ui_->radarComboBox->count() > 0) {
    ui_->radarComboBox->setCurrentIndex(target_index);
  }
  blocker.unblock();
  applySelectedProfile(target_index);
}

void MainWindow::applySelectedProfile(int device_index) {
  if (device_index < 0) return;
  saveActiveProfileState();
  // Save the paths shown for the previously active Profile/target before
  // rebuilding the fields. RuntimeFileSelection is also backed by QSettings
  // so each device keeps its explicit operator overrides across restarts.
  saveRuntimeFileSelection();
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
                 : profile.supports_app_tmp_package
                       ? QStringLiteral("APP 文件/升级包")
                       : QStringLiteral("APP 文件"));
  ui_->calPathLabel->setText(
      geely_p416 ? QStringLiteral("ESS VBF 文件")
                 : QStringLiteral("CAL 文件"));
  ui_->seedKeyDllPathLabel->setText(
      geely_p416 ? QStringLiteral("SeedKey（内置）")
                 : QStringLiteral("SeedKey 算法库"));
  QSignalBlocker entry_mode_blocker(ui_->entryModeComboBox);
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
#if defined(UDS_EXPOSE_ARC331_BOOT_RECOVERY)
  if (profile.flow_id == QStringLiteral("chuneng_arc331")) {
    ui_->entryModeComboBox->addItem(
        QStringLiteral("BOOT→APP（仅Boot）"), QStringLiteral("boot"));
  }
#endif
  if (profile.supports_ft_entry) {
    ui_->entryModeComboBox->addItem(
        profile.ft_entry_label.isEmpty()
            ? QStringLiteral("FT")
            : profile.ft_entry_label,
        QStringLiteral("ft"));
  }
  if (profile.supports_cal_download) {
    ui_->entryModeComboBox->addItem(QStringLiteral("CAL"),
                                    QStringLiteral("cal"));
    ui_->entryModeComboBox->addItem(QStringLiteral("APP+CAL"),
                                    QStringLiteral("app_cal"));
  }
  const auto entry_index =
      ui_->entryModeComboBox->findData(profile.default_entry_mode);
  ui_->entryModeComboBox->setCurrentIndex(entry_index < 0 ? 0 : entry_index);
  showPath(ui_->driverPathLineEdit, profile.driver_path);
  showPath(ui_->appPathLineEdit, profile.app_path);
  showPath(ui_->calPathLineEdit, profile.cal_path);
  showPath(ui_->driverVerifyPathLineEdit, profile.driver_verify_path);
  showPath(ui_->appVerifyPathLineEdit, profile.app_verify_path);
  ui_->appVerifyPathLabel->setText(QStringLiteral("APP 校验文件"));
  showPath(ui_->calVerifyPathLineEdit, profile.cal_verify_path);
  showPath(ui_->seedKeyDllPathLineEdit, profile.seed_key_dll_path);
  // Profile IDs remain the defaults and are restored by double-clicking the
  // corresponding label. Operators may override either ID for every project
  // without editing the Profile on disk.
  ui_->txIdLineEdit->setReadOnly(false);
  ui_->rxIdLineEdit->setReadOnly(false);
  applySelectedRadar(false);
  restoreRuntimeFileSelection();
  restoreCurrentProfileState();
  updateAppPackagePresentation(false);
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

void MainWindow::saveActiveProfileState() const {
  if (active_profile_state_key_.isEmpty()) return;

  QSettings settings;
  settings.beginGroup(profileStateSettingsGroup(active_profile_state_key_));
  const auto entry_mode = ui_->entryModeComboBox->currentData().toString();
  if (!entry_mode.isEmpty()) {
    settings.setValue(QStringLiteral("entry_mode"), entry_mode);
  }
  bool tx_valid{}, rx_valid{};
  const auto tx_text = ui_->txIdLineEdit->text().trimmed();
  const auto rx_text = ui_->rxIdLineEdit->text().trimmed();
  tx_text.toUInt(&tx_valid, 0);
  rx_text.toUInt(&rx_valid, 0);
  if (tx_valid) settings.setValue(QStringLiteral("tx_id"), tx_text);
  if (rx_valid) settings.setValue(QStringLiteral("rx_id"), rx_text);
  settings.setValue(QStringLiteral("repeat_count"),
                    ui_->repeatCountSpinBox->value());
  bool channel_valid{};
  const auto channel =
      ui_->vectorChannelComboBox->currentData().toUInt(&channel_valid);
  if (channel_valid && channel > 0) {
    settings.setValue(QStringLiteral("channel/%1")
                          .arg(canVendorKey(default_can_vendor())),
                      channel);
  }
  settings.endGroup();
  settings.sync();
}

void MainWindow::restoreCurrentProfileState() {
  const auto profile_state_key = selectedLogTargetKey();
  active_profile_state_key_ = profile_state_key;
  if (profile_state_key.isEmpty()) return;

  QSettings settings;
  const auto state_group = profileStateSettingsGroup(profile_state_key);
  bool legacy_owner{};
  if (restoring_combo_selections_) {
    bool valid{};
    const auto profile_index = selectedProfileIndex(&valid);
    const auto& profiles = controller_bridge_->profileOptions();
    legacy_owner = valid && profile_index >= 0 &&
                   static_cast<std::size_t>(profile_index) < profiles.size() &&
                   settings.value(QStringLiteral("selectors/profile_id"))
                           .toString() == profiles[profile_index].profile_id;
  }
  auto saved_entry_mode =
      settings.value(state_group + QStringLiteral("/entry_mode")).toString();

  // Migrate the former single global mode only for the Profile that owned it.
  if (saved_entry_mode.isEmpty() && legacy_owner) {
    saved_entry_mode =
        settings.value(QStringLiteral("selectors/entry_mode")).toString();
  }

  const auto entry_index =
      ui_->entryModeComboBox->findData(saved_entry_mode);
  if (entry_index >= 0) {
    QSignalBlocker blocker(ui_->entryModeComboBox);
    ui_->entryModeComboBox->setCurrentIndex(entry_index);
  }

  const auto restore_id = [&settings, &state_group](const QString& name,
                                                    QLineEdit* editor) {
    const auto saved =
        settings.value(state_group + QLatin1Char('/') + name).toString();
    bool valid{};
    saved.toUInt(&valid, 0);
    if (valid) editor->setText(saved);
  };
  restore_id(QStringLiteral("tx_id"), ui_->txIdLineEdit);
  restore_id(QStringLiteral("rx_id"), ui_->rxIdLineEdit);

  auto repeat_count = static_cast<int>(uds::app::kMinFlashRepeatCount);
  const auto repeat_key = state_group + QStringLiteral("/repeat_count");
  if (settings.contains(repeat_key)) {
    repeat_count = settings.value(repeat_key).toInt();
  } else if (legacy_owner &&
             settings.contains(QStringLiteral("selectors/repeat_count"))) {
    // Migrate the former global value only to the Profile/target that was
    // selected by the old release. Other devices retain the safe default 1.
    repeat_count =
        settings.value(QStringLiteral("selectors/repeat_count")).toInt();
  }
  repeat_count = std::clamp(
      repeat_count, static_cast<int>(uds::app::kMinFlashRepeatCount),
      static_cast<int>(uds::app::kMaxFlashRepeatCount));
  {
    QSignalBlocker blocker(ui_->repeatCountSpinBox);
    ui_->repeatCountSpinBox->setValue(repeat_count);
  }

  restoreCurrentBackendChannel(currentProfileDefaultChannel());

  // Store a migrated value under the new per-Profile/target key so all future
  // switches are independent of the legacy global selector.
  saveActiveProfileState();
}

void MainWindow::saveRuntimeFileSelection() {
  if (active_file_selection_key_.isEmpty()) return;
  const RuntimeFileSelection selection{
      fullPath(ui_->driverPathLineEdit),
      fullPath(ui_->appPathLineEdit),
      fullPath(ui_->calPathLineEdit),
      fullPath(ui_->driverVerifyPathLineEdit),
      fullPath(ui_->appVerifyPathLineEdit),
      fullPath(ui_->calVerifyPathLineEdit),
      fullPath(ui_->seedKeyDllPathLineEdit),
  };
  runtime_file_selections_.insert(active_file_selection_key_, selection);

  QSettings settings;
  settings.beginGroup(QStringLiteral("flash_file_selections"));
  settings.beginGroup(active_file_selection_key_);
  settings.setValue(QStringLiteral("driver"), selection.driver_path);
  settings.setValue(QStringLiteral("app"), selection.app_path);
  settings.setValue(QStringLiteral("cal"), selection.cal_path);
  settings.setValue(QStringLiteral("driver_verify"),
                    selection.driver_verify_path);
  settings.setValue(QStringLiteral("app_verify"), selection.app_verify_path);
  settings.setValue(QStringLiteral("cal_verify"), selection.cal_verify_path);
  settings.setValue(QStringLiteral("seed_key_dll"),
                    selection.seed_key_dll_path);
  settings.endGroup();
  settings.endGroup();
  settings.sync();
}

void MainWindow::restoreRuntimeFileSelection() {
  active_file_selection_key_ = selectedLogTargetKey();
  if (active_file_selection_key_.isEmpty()) return;
  auto saved = runtime_file_selections_.constFind(active_file_selection_key_);
  if (saved == runtime_file_selections_.cend()) {
    QSettings settings;
    settings.beginGroup(QStringLiteral("flash_file_selections"));
    settings.beginGroup(active_file_selection_key_);
    if (settings.contains(QStringLiteral("driver"))) {
      runtime_file_selections_.insert(
          active_file_selection_key_,
          RuntimeFileSelection{
              settings.value(QStringLiteral("driver")).toString(),
              settings.value(QStringLiteral("app")).toString(),
              settings.value(QStringLiteral("cal")).toString(),
              settings.value(QStringLiteral("driver_verify")).toString(),
              settings.value(QStringLiteral("app_verify")).toString(),
              settings.value(QStringLiteral("cal_verify")).toString(),
              settings.value(QStringLiteral("seed_key_dll")).toString(),
          });
      saved = runtime_file_selections_.constFind(active_file_selection_key_);
    }
    settings.endGroup();
    settings.endGroup();
  }
  if (saved == runtime_file_selections_.cend()) return;

  showPath(ui_->driverPathLineEdit, saved->driver_path);
  showPath(ui_->appPathLineEdit, saved->app_path);
  showPath(ui_->calPathLineEdit, saved->cal_path);
  showPath(ui_->driverVerifyPathLineEdit, saved->driver_verify_path);
  showPath(ui_->appVerifyPathLineEdit, saved->app_verify_path);
  showPath(ui_->calVerifyPathLineEdit, saved->cal_verify_path);
  showPath(ui_->seedKeyDllPathLineEdit, saved->seed_key_dll_path);
}

QString MainWindow::configuredDefaultFlashFile(FlashFileField field) const {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return {};
  }

  const auto path_from = [field](const auto& option) -> QString {
    switch (field) {
    case FlashFileField::Driver:
      return option.driver_path;
    case FlashFileField::DriverVerify:
      return option.driver_verify_path;
    case FlashFileField::App:
      return option.app_path;
    case FlashFileField::AppVerify:
      return option.app_verify_path;
    case FlashFileField::Cal:
      return option.cal_path;
    case FlashFileField::CalVerify:
      return option.cal_verify_path;
    case FlashFileField::SeedKeyDll:
      return option.seed_key_dll_path;
    }
    return {};
  };

  const auto& profile = profiles[profile_index];
  auto default_path = path_from(profile);
  if (!profile.target_options.empty()) {
    const auto target_id = selectedTargetId();
    const auto target = std::find_if(
        profile.target_options.cbegin(), profile.target_options.cend(),
        [&target_id](const ControllerTargetOption& option) {
          return option.target_id == target_id;
        });
    if (target != profile.target_options.cend()) {
      default_path = path_from(*target);
    }
  }
  return default_path;
}

bool MainWindow::storeSelectedFlashFile(FlashFileField field,
                                        const QString& selected,
                                        QLineEdit* editor,
                                        const QString& log_name) {
  auto default_path = configuredDefaultFlashFile(field);
  // Some package formats embed verification data, so their verification slot
  // intentionally has no default file. Use the corresponding payload file as
  // a project-owned directory anchor without introducing project branches.
  if (default_path.isEmpty()) {
    switch (field) {
    case FlashFileField::DriverVerify:
      default_path = configuredDefaultFlashFile(FlashFileField::Driver);
      break;
    case FlashFileField::AppVerify:
      default_path = configuredDefaultFlashFile(FlashFileField::App);
      break;
    case FlashFileField::CalVerify:
      default_path = configuredDefaultFlashFile(FlashFileField::Cal);
      break;
    default:
      break;
    }
  }
  const auto resources_root = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("resources"));
  const auto result = replaceConfiguredResourceFile(
      selected, default_path, resources_root);
  if (!result.success) {
    QMessageBox::warning(this, QStringLiteral("替换默认资源失败"), result.error);
    appendUiLog(QStringLiteral("%1默认资源替换失败：%2")
                    .arg(log_name, result.error),
                UiLogTone::Failure);
    return false;
  }

  showPath(editor, result.stored_path);
  saveRuntimeFileSelection();
  appendUiLog(QStringLiteral("%1已复制到默认资源目录（保留原文件名）：%2 → %3")
                  .arg(log_name, QDir::toNativeSeparators(selected),
                       QDir::toNativeSeparators(result.stored_path)));
  return true;
}

void MainWindow::restoreDefaultFlashFile(FlashFileField field) {
  const auto default_path = configuredDefaultFlashFile(field);

  QLineEdit* editor{};
  QString field_name;
  switch (field) {
  case FlashFileField::Driver:
    editor = ui_->driverPathLineEdit;
    field_name = ui_->driverPathLabel->text();
    break;
  case FlashFileField::DriverVerify:
    editor = ui_->driverVerifyPathLineEdit;
    field_name = ui_->driverVerifyPathLabel->text();
    break;
  case FlashFileField::App:
    editor = ui_->appPathLineEdit;
    field_name = ui_->appPathLabel->text();
    break;
  case FlashFileField::AppVerify:
    editor = ui_->appVerifyPathLineEdit;
    field_name = ui_->appVerifyPathLabel->text();
    break;
  case FlashFileField::Cal:
    editor = ui_->calPathLineEdit;
    field_name = ui_->calPathLabel->text();
    break;
  case FlashFileField::CalVerify:
    editor = ui_->calVerifyPathLineEdit;
    field_name = ui_->calVerifyPathLabel->text();
    break;
  case FlashFileField::SeedKeyDll:
    editor = ui_->seedKeyDllPathLineEdit;
    field_name = ui_->seedKeyDllPathLabel->text();
    break;
  }
  if (!editor) return;

  if (!default_path.isEmpty() && !QFileInfo(default_path).isFile()) {
    const auto message =
        QStringLiteral("Profile原默认%1文件不存在，无法恢复：%2")
            .arg(field_name, QDir::toNativeSeparators(default_path));
    appendUiLog(message, UiLogTone::Pending);
    QMessageBox::warning(this, QStringLiteral("原默认资源已不存在"), message);
    return;
  }

  showPath(editor, default_path);
  if (field == FlashFileField::App || field == FlashFileField::AppVerify) {
    updateAppPackagePresentation(false);
  }
  saveRuntimeFileSelection();
  appendUiLog(
      QStringLiteral("已恢复当前项目/设备默认%1：%2")
          .arg(field_name,
               default_path.isEmpty()
                   ? QStringLiteral("<未配置>")
                   : QDir::toNativeSeparators(default_path)));
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

bool MainWindow::selectedProfileSupportsAppTmpPackage() const {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  return valid && profile_index >= 0 &&
         static_cast<std::size_t>(profile_index) < profiles.size() &&
         profiles[profile_index].supports_app_tmp_package;
}

void MainWindow::updateAppPackagePresentation(bool report_error) {
  auto* verification = ui_->appVerifyPathLineEdit;
  bool profile_valid{};
  const auto profile_index = selectedProfileIndex(&profile_valid);
  const auto& profiles = controller_bridge_->profileOptions();
  const auto optional_lingpao_certificate =
      profile_valid && profile_index >= 0 &&
      static_cast<std::size_t>(profile_index) < profiles.size() &&
      (profiles[profile_index].flow_id == QStringLiteral("lp_arf") ||
       profiles[profile_index].flow_id == QStringLiteral("lp_arc"));
  verification->setProperty(kEmbeddedVerificationProperty, false);
  ui_->appPathLineEdit->setProperty(kPackageValidProperty, true);
  ui_->appVerifyPathLabel->setText(
      optional_lingpao_certificate
          ? QStringLiteral("APP 验签文件（可选）")
          : QStringLiteral("APP 校验文件"));
  ui_->appVerifyBrowseButton->setText(QStringLiteral("浏览"));
  verification->setStyleSheet({});

  const auto app_path = fullPath(ui_->appPathLineEdit);
  if (!selectedProfileSupportsAppTmpPackage() ||
      !app_path.endsWith(QStringLiteral(".tmp"), Qt::CaseInsensitive)) {
    verification->setToolTip(fullPath(verification));
    return;
  }

  try {
    const auto package = load_leapmotor_tmp(
        std::filesystem::path(app_path.toStdWString()));
    const auto summary =
        QStringLiteral(
            "来源：%1\nAPP 地址：0x%2\nAPP 长度：0x%3（%4 bytes）\n"
            "APP声明CRC32：0x%5\nCertificate：%6 bytes\n状态：TMP结构解析完成；CRC及验签由ECU判定")
            .arg(QDir::toNativeSeparators(app_path))
            .arg(package.app.address, 8, 16, QLatin1Char('0'))
            .arg(package.app.data.size(), 0, 16)
            .arg(package.app.data.size())
            .arg(package.app_crc32, 8, 16, QLatin1Char('0'))
            .arg(package.certificate.size())
            .toUpper();
    verification->setProperty(kFullPathProperty, QString{});
    verification->setProperty(kEmbeddedVerificationProperty, true);
    verification->setText(
        QStringLiteral("TMP 内置 Certificate · %1 B · 已解析")
            .arg(package.certificate.size()));
    verification->setToolTip(summary);
    verification->setCursorPosition(0);
    verification->setStyleSheet(
        QStringLiteral("QLineEdit { color: #16803C; font-weight: 600; }"));
    ui_->appVerifyPathLabel->setText(QStringLiteral("APP 验签（内置）"));
    ui_->appVerifyBrowseButton->setText(QStringLiteral("详情"));
    if (report_error) {
      appendUiLog(
          QStringLiteral("TMP解析通过：APP 0x%1 bytes @ 0x%2；内置Certificate %3 bytes")
              .arg(package.app.data.size(), 0, 16)
              .arg(package.app.address, 8, 16, QLatin1Char('0'))
              .arg(package.certificate.size()),
          UiLogTone::Success);
    }
  } catch (const std::exception& error) {
    showPath(verification, {});
    ui_->appPathLineEdit->setProperty(kPackageValidProperty, false);
    verification->setPlaceholderText(QStringLiteral("TMP 解析失败，禁止刷写"));
    const auto message =
        QStringLiteral("TMP升级包解析失败：%1")
            .arg(QString::fromUtf8(error.what()));
    verification->setToolTip(message);
    appendUiLog(message, UiLogTone::Failure);
    if (report_error) {
      QMessageBox::warning(this, QStringLiteral("TMP升级包无效"), message);
    }
  }
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
  updateAppPackagePresentation(false);
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
  syncDiagnosticRequestContext();
}

void MainWindow::restoreDefaultDiagnosticId(bool restore_tx) {
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) {
    return;
  }

  const auto& profile = profiles[profile_index];
  auto default_id = restore_tx ? profile.tx_id : profile.rx_id;
  if (hasRadarSelector()) {
    const auto target_id = selectedTargetId();
    const auto selected = std::find_if(
        profile.target_options.cbegin(), profile.target_options.cend(),
        [&target_id](const ControllerTargetOption& target) {
          return target.target_id == target_id;
        });
    if (selected != profile.target_options.cend()) {
      default_id = restore_tx ? selected->tx_id : selected->rx_id;
    }
  }

  auto* editor = restore_tx ? ui_->txIdLineEdit : ui_->rxIdLineEdit;
  editor->setText(QStringLiteral("0x%1").arg(
      QString::number(default_id, 16).toUpper()));
  saveActiveProfileState();
  appendUiLog(QStringLiteral("已恢复当前设备默认 %1：%2")
                  .arg(restore_tx ? QStringLiteral("Tx ID")
                                  : QStringLiteral("Rx ID"),
                       editor->text()));
  syncVersionContext();
  syncBusMonitorContext();
  syncDiagnosticRequestContext();
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

void MainWindow::syncDiagnosticRequestContext() {
  if (!diagnostic_request_page_ || !controller_bridge_) return;
  bool valid{};
  const auto profile_index = selectedProfileIndex(&valid);
  const auto& profiles = controller_bridge_->profileOptions();
  if (!valid || profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles.size()) return;
  const auto& profile = profiles[static_cast<std::size_t>(profile_index)];
  QString target_name = ui_->radarComboBox->currentText();
  if (target_name.isEmpty()) target_name = profile.device_name;
  bool tx_ok{};
  bool rx_ok{};
  const auto tx_id = ui_->txIdLineEdit->text().toUInt(&tx_ok, 0);
  const auto rx_id = ui_->rxIdLineEdit->text().toUInt(&rx_ok, 0);
  diagnostic_request_page_->setContext(
      profile_index, selectedTargetId(),
      canVendorDisplayName(default_can_vendor()), profile.vendor_name,
      profile.project_name, target_name,
      ui_->vectorChannelComboBox->currentData().toUInt(),
      profile.nominal_bitrate, profile.data_bitrate, profile.can_fd,
      tx_ok ? tx_id : profile.tx_id, rx_ok ? rx_id : profile.rx_id,
      profile.functional_id);
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
  if (active_profile_state_key_.isEmpty()) return;
  bool valid{};
  const auto channel =
      ui_->vectorChannelComboBox->currentData().toUInt(&valid);
  if (!valid || channel == 0) return;
  QSettings settings;
  settings.setValue(
      profileStateSettingsGroup(active_profile_state_key_) +
          QStringLiteral("/channel/%1")
              .arg(canVendorKey(default_can_vendor())),
      channel);
}

void MainWindow::restoreCurrentBackendChannel(
    unsigned profile_default_channel) {
  QSettings settings;
  const auto vendor = default_can_vendor();
  const auto key = profileStateSettingsGroup(selectedLogTargetKey()) +
                   QStringLiteral("/channel/%1").arg(canVendorKey(vendor));
  auto fallback =
      vendor == CanVendor::Vector ? std::max(1U, profile_default_channel) : 1U;
  // Migrate a legacy backend-wide channel only to the Profile/target selected
  // by the old release. Never let that value become another device's default.
  if (!settings.contains(key) && restoring_combo_selections_) {
    bool valid{};
    const auto profile_index = selectedProfileIndex(&valid);
    const auto& profiles = controller_bridge_->profileOptions();
    const auto legacy_owner =
        valid && profile_index >= 0 &&
        static_cast<std::size_t>(profile_index) < profiles.size() &&
        settings.value(QStringLiteral("selectors/profile_id")).toString() ==
            profiles[profile_index].profile_id;
    if (legacy_owner) {
      const auto backend_key = canChannelSettingsKey(vendor);
      if (settings.contains(backend_key)) {
        const auto legacy = settings.value(backend_key).toUInt();
        if (legacy > 0) fallback = legacy;
      } else if (vendor == CanVendor::Vector &&
                 settings.contains(QStringLiteral("selectors/channel"))) {
        const auto legacy =
            settings.value(QStringLiteral("selectors/channel")).toUInt();
        if (legacy > 0) fallback = legacy;
      }
    }
  }
  const auto channel =
      std::max(1U, settings.value(key, fallback).toUInt());

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
  settings.setValue(
      vendorProjectSettingsKey(ui_->projectComboBox->currentText()),
      ui_->deviceComboBox->currentText());
  // Keep the old key as a vendor alias so existing installations retain their
  // last selection across this UI-only hierarchy migration.
  settings.setValue(QStringLiteral("selectors/project"),
                    ui_->projectComboBox->currentText());
  settings.setValue(QStringLiteral("selectors/profile_id"),
                    profiles[profile_index].profile_id);
  const auto device_group = projectDeviceSettingsGroup(
      ui_->projectComboBox->currentText(), ui_->deviceComboBox->currentText());
  settings.setValue(device_group + QStringLiteral("/profile_id"),
                    profiles[profile_index].profile_id);
  settings.setValue(device_group + QStringLiteral("/target_id"),
                    selectedTargetId());
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
  const auto optional_lingpao_certificate =
      profile.flow_id == QStringLiteral("lp_arf") ||
      profile.flow_id == QStringLiteral("lp_arc");
  const auto needs_app =
      entry_mode != QStringLiteral("cal");
  const auto embedded_tmp =
      needs_app && profile.supports_app_tmp_package &&
      fullPath(ui_->appPathLineEdit)
          .endsWith(QStringLiteral(".tmp"), Qt::CaseInsensitive);
  if (embedded_tmp &&
      !ui_->appPathLineEdit->property(kPackageValidProperty).toBool()) {
    QMessageBox::warning(
        this, QStringLiteral("TMP升级包无效"),
        QStringLiteral("当前TMP结构解析失败，禁止刷写。"));
    return;
  }
  if (needs_app && profile.supports_app_tmp_package && !embedded_tmp &&
      !optional_lingpao_certificate &&
      fullPath(ui_->appVerifyPathLineEdit).isEmpty()) {
    QMessageBox::warning(
        this, QStringLiteral("缺少APP验签文件"),
        QStringLiteral("当前导入的是S19/SREC/BIN，请手动选择与APP匹配的ASC或TMP验签文件。"));
    return;
  }
  const auto needs_app_verification =
      (needs_app && !embedded_tmp && !optional_lingpao_certificate) ||
      (profile.profile_id == QStringLiteral("chery_t22") &&
       entry_mode == QStringLiteral("cal"));
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
                  needs_app_verification,
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

void MainWindow::updateEnabledState() {
  const auto busy = probe_running_ || flash_running_ ||
                    version_check_running_ || diagnostic_request_running_;
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
  ui_->txIdLineEdit->setReadOnly(false);
  ui_->rxIdLineEdit->setReadOnly(false);
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
  if (diagnostic_request_page_) {
    diagnostic_request_page_->setOperationBusy(
        busy && !diagnostic_request_running_);
  }
  updateStatusBar();
}

void MainWindow::initializeExecutionLog() {
  QDir application_directory(QCoreApplication::applicationDirPath());
  if (!application_directory.mkpath(QStringLiteral("logs/execution"))) return;
  const auto file_name = QStringLiteral("execution_%1.log")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd_HHmmss_zzz")));
  auto file = std::make_unique<QFile>(
      application_directory.filePath(
          QStringLiteral("logs/execution/%1").arg(file_name)));
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
  const auto display_timestamp =
      QStringLiteral("[%1]").arg(now.toString(QStringLiteral("HH:mm:ss")));
  if (active_log_target_key_.isEmpty()) {
    active_log_target_key_ = selectedLogTargetKey();
    if (active_log_target_key_.isEmpty()) {
      active_log_target_key_ = QStringLiteral("__application__");
    }
  }
  auto& target_entries = target_log_entries_[active_log_target_key_];
  target_entries.push_back(UiLogEntry{display_timestamp, displayed_message,
                                      tone,
                                      parseUiLogMessage(displayed_message)});
  constexpr qsizetype kMaximumUiLogLinesPerTarget = 5000;
  while (target_entries.size() > kMaximumUiLogLinesPerTarget) {
    target_entries.removeFirst();
  }
  auto* scrollbar = ui_->logPlainTextEdit->verticalScrollBar();
  const auto old_scroll_value = scrollbar->value();
  const auto old_cursor = ui_->logPlainTextEdit->textCursor();
  appendUiLogEntryToView(target_entries.back());
  if (execution_log_follow_tail_) {
    scheduleExecutionLogTailFollow();
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

void MainWindow::scheduleExecutionLogTailFollow() {
  // QTextDocument layout and the scrollbar range can update after text
  // insertion returns. Queue the scroll so End means a persistent tail-follow
  // mode instead of only jumping to the document end that existed at keypress.
  QTimer::singleShot(0, this, [this] {
    if (!execution_log_follow_tail_ || !ui_) return;
    auto cursor = ui_->logPlainTextEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui_->logPlainTextEdit->setTextCursor(cursor);
    auto* scrollbar = ui_->logPlainTextEdit->verticalScrollBar();
    scrollbar->setValue(scrollbar->maximum());
  });
}

void MainWindow::appendUiLogEntryToView(const UiLogEntry& entry) {
  QTextCharFormat timestamp_format;
  timestamp_format.setForeground(QColor(QStringLiteral("#6B7280")));

  QTextCharFormat normal_format;
  normal_format.setForeground(QColor(QStringLiteral("#263238")));

  QTextCharFormat semantic_format = normal_format;
  switch (entry.tone) {
  case UiLogTone::Success:
    semantic_format.setForeground(QColor(QStringLiteral("#16803C")));
    semantic_format.setFontWeight(QFont::Bold);
    break;
  case UiLogTone::Failure:
    semantic_format.setForeground(QColor(QStringLiteral("#C62828")));
    semantic_format.setFontWeight(QFont::Bold);
    break;
  case UiLogTone::Pending:
    semantic_format.setForeground(QColor(QStringLiteral("#A85D00")));
    semantic_format.setFontWeight(QFont::Bold);
    break;
  case UiLogTone::Normal:
    break;
  }

  auto cursor = ui_->logPlainTextEdit->textCursor();
  cursor.movePosition(QTextCursor::End);
  if (!ui_->logPlainTextEdit->document()->isEmpty()) cursor.insertBlock();
  cursor.insertText(entry.timestamp, timestamp_format);
  cursor.insertText(QStringLiteral(" "), normal_format);

  if (entry.parsed.direction == LogDirection::None) {
    cursor.insertText(entry.message, semantic_format);
  } else {
    if (!entry.parsed.leadingPrefix.isEmpty()) {
      cursor.insertText(entry.parsed.leadingPrefix, timestamp_format);
      cursor.insertText(QStringLiteral(" "), normal_format);
    }

    QTextCharFormat direction_format = semantic_format;
    if (entry.tone == UiLogTone::Normal) {
      direction_format.setForeground(QColor(
          entry.parsed.direction == LogDirection::Tx
              ? QStringLiteral("#1565C0")
              : QStringLiteral("#00897B")));
      direction_format.setFontWeight(QFont::Bold);
    }
    cursor.insertText(entry.parsed.directionAndCanId, direction_format);
    if (!entry.parsed.payload.isEmpty()) {
      cursor.insertText(QStringLiteral(" "), normal_format);
      cursor.insertText(entry.parsed.payload, semantic_format);
    }
  }
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

void MainWindow::handleProgressChanged(int percent, const QString& message) {
  if (probe_running_) {
    // Online detection is a binary verdict: 0 until a validated physical
    // diagnostic response is received, then 100.
    ui_->progressBar->setValue(percent >= 100 ? 100 : 0);
  } else if (flash_running_) {
    flash_progress_ =
        std::max(flash_progress_, std::clamp(percent, 0, 100));
    ui_->progressBar->setValue(flash_progress_);
  }

  if (version_check_running_) {
    // Version reading belongs to its own page. Keep the flash page's final
    // result visible and use the status bar for transient version progress.
    statusBar()->showMessage(message);
    return;
  }
  ui_->progressStatusLabel->setText(message);
}

void MainWindow::handleVersionCheckRunningChanged(bool running) {
  version_check_running_ = running;
  version_page_->setRunning(running);
  if (running) {
    appendUiLog(QStringLiteral("========== 开始版本读取 =========="),
                UiLogTone::Pending);
    statusBar()->showMessage(QStringLiteral("版本读取中……"));
  }
  updateEnabledState();
}

void MainWindow::handleVersionCheckFinished(bool success, bool cancelled,
                                            const QString& message) {
  version_check_running_ = false;
  version_page_->finish(success, cancelled, message);
  const auto tone = success ? UiLogTone::Success : UiLogTone::Failure;
  appendUiLog(message, tone);
  appendUiLog(cancelled
                  ? QStringLiteral("========== 版本读取已停止 ==========")
                  : success
                        ? QStringLiteral("========== 版本读取成功 ==========")
                        : QStringLiteral("========== 版本读取失败 =========="),
              tone);
  updateEnabledState();
  updateStatusBar();
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
  execution_log_follow_tail_ = true;
  scheduleExecutionLogTailFollow();
}

void MainWindow::clearActiveUiLog() {
  if (!active_log_target_key_.isEmpty()) {
    target_log_entries_.remove(active_log_target_key_);
  }
  ui_->logPlainTextEdit->clear();
  execution_log_follow_tail_ = true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
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
    const auto modifiers = key_event->modifiers();
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
        ui_->logPlainTextEdit->ensureCursorVisible();
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
