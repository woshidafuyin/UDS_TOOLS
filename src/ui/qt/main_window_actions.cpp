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

} // namespace uds::ui::qt
