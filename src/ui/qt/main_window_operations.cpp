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
#include <QStyle>
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

void MainWindow::startProbeFromUi() {
  const auto entry_mode = ui_->entryModeComboBox->currentData().toString();
  if (entry_mode.isEmpty()) {
    appendUiLog(
        QStringLiteral(
            "未配置刷写模式：请先选择刷写模式后再执行“能否刷写”。"),
        UiLogTone::Failure, UiLogDestination::ViewOnly);
    return;
  }
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
  const auto& profile = controller_bridge_->profileOptions()[
      static_cast<std::size_t>(profile_index)];
  const auto target_name = hasRadarSelector()
                               ? ui_->radarComboBox->currentText()
                               : profile.device_name;
  const auto detailed_request =
      QStringLiteral("请求在线探测：%1CH%2，入口=%3，界面APP端点 TX=0x%4，RX=0x%5；实际寻址按项目探测策略执行")
                  .arg(hasRadarSelector()
                           ? ui_->radarComboBox->currentText() +
                                 QStringLiteral("；")
                           : QString{})
                  .arg(channel)
                  .arg(entry_mode.toUpper())
                  .arg(QString::number(tx_id, 16).toUpper())
                  .arg(QString::number(rx_id, 16).toUpper());
  appendUiLog(detailed_request, UiLogTone::Normal,
              UiLogDestination::FileOnly);
  probe_ui_log_active_ = true;
  probe_refresh_entry_checked_ = false;
  probe_can_open_summary_ =
      QStringLiteral("CAN已打开：%1，CH%2，TX 0x%3，RX 0x%4")
          .arg(canVendorDisplayName(default_can_vendor()))
          .arg(channel)
          .arg(QString::number(tx_id, 16).toUpper())
          .arg(QString::number(rx_id, 16).toUpper());
  appendUiLog(QStringLiteral("开始在线探测：%1 / %2 / %3 / %4")
                  .arg(profile.vendor_name, profile.project_name, target_name,
                       entry_mode.toUpper()),
              UiLogTone::Normal, UiLogDestination::ViewOnly);
  emit probeRequested(profile_index, selectedTargetId(), entry_mode, channel,
                      tx_id, rx_id);
}

void MainWindow::startFlashFromUi() {
  const auto entry_mode = ui_->entryModeComboBox->currentData().toString();
  if (entry_mode.isEmpty()) {
    QMessageBox::warning(
        this, QStringLiteral("请选择刷写模式"),
        QStringLiteral("请先选择 APP、FT 或其他可用刷写模式。"));
    return;
  }
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
  const auto update_public_key = ui_->updatePublicKeyCheckBox->isChecked();
  if (profile.supports_ft_entry && entry_mode == QStringLiteral("ft")) {
    appendUiLog(QStringLiteral(
        "提示：已选择FT恢复入口，将使用当前目标Profile配置的FT端点切换，"
        "随后继续执行Driver与APP下载。"), UiLogTone::Normal,
        UiLogDestination::FileOnly);
  }
  if (profile.flow_id == QStringLiteral("chuneng_arc331") &&
      entry_mode == QStringLiteral("boot")) {
    appendUiLog(QStringLiteral(
        "提示：已选择BOOT→APP入口；使用当前设备物理诊断ID，跳过APP态0203/85/28，"
        "正式刷写全程保持0x520/500ms唤醒。"), UiLogTone::Normal,
        UiLogDestination::FileOnly);
  }
  if (entry_mode == QStringLiteral("cal")) {
    appendUiLog(
        QStringLiteral(
            "提示：CAL模式将按CANoe顺序执行 Driver + CAL，APP文件不会下载。"),
        UiLogTone::Normal, UiLogDestination::FileOnly);
  } else if (entry_mode == QStringLiteral("app_cal")) {
    appendUiLog(
        QStringLiteral(
            "提示：APP+CAL模式将按CANoe顺序执行 Driver + APP + CAL。"),
        UiLogTone::Normal, UiLogDestination::FileOnly);
  }

  flash_progress_ = 0;
  ui_->progressBar->setValue(0);
  ui_->progressStatusLabel->setText(QStringLiteral("正在启动完整刷写……"));
  const auto flash_target = QStringLiteral("%1 / %2 / %3")
                                .arg(profile.vendor_name,
                                     profile.project_name,
                                     ui_->radarComboBox->currentText());
  const auto detailed_start = QStringLiteral(
                  "直接开始刷写：%1，次数 %2，CH%3，TX 0x%4 -> RX 0x%5，FUNC 0x%6，模式 %7，Update_PublicKey=%8")
                    .arg(flash_target)
                    .arg(repeat_count)
                    .arg(channel)
                   .arg(QString::number(tx_id, 16).toUpper())
                   .arg(QString::number(rx_id, 16).toUpper())
                   .arg(QString::number(profile.functional_id, 16).toUpper())
                    .arg(entry_mode.toUpper())
                    .arg(update_public_key ? QStringLiteral("ON")
                                           : QStringLiteral("OFF"));
  appendUiLog(detailed_start, UiLogTone::Normal,
              UiLogDestination::FileOnly);
  beginFlashUiLog();
  appendUiLog(QStringLiteral("准备开始刷写：%1 / %2")
                  .arg(flash_target, entry_mode.toUpper()),
              UiLogTone::Normal, UiLogDestination::ViewOnly);
  emit flashRequested(
       profile_index, selectedTargetId(), entry_mode, update_public_key,
       repeat_count, channel, tx_id, rx_id, fullPath(ui_->driverPathLineEdit),
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
  const auto entry_mode_selected =
      !ui_->entryModeComboBox->currentData().toString().isEmpty();
  const auto mode_unselected = !entry_mode_selected;
  if (entry_mode_placeholder_) {
    entry_mode_placeholder_->setVisible(mode_unselected);
    if (mode_unselected) entry_mode_placeholder_->raise();
  }
  if (ui_->entryModeComboBox->property("modeUnselected").toBool() !=
      mode_unselected) {
    ui_->entryModeComboBox->setProperty("modeUnselected", mode_unselected);
    ui_->entryModeComboBox->style()->unpolish(ui_->entryModeComboBox);
    ui_->entryModeComboBox->style()->polish(ui_->entryModeComboBox);
    ui_->entryModeComboBox->update();
  }

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
  const auto e0y_endpoint_locked =
      profile_valid &&
      profiles[profile_index].flow_id == QStringLiteral("chery_e0y");
  ui_->txIdLineEdit->setReadOnly(e0y_endpoint_locked);
  ui_->rxIdLineEdit->setReadOnly(e0y_endpoint_locked);
  ui_->entryModeComboBox->setEnabled(!busy && profile_valid);
  ui_->repeatCountSpinBox->setEnabled(!busy && usable);
  const auto e0y_public_key_compatible =
      usable && entry_mode_selected &&
      profiles[profile_index].flow_id == QStringLiteral("chery_e0y");
  if (!e0y_public_key_compatible) {
    ui_->updatePublicKeyCheckBox->setChecked(false);
  }
  ui_->updatePublicKeyCheckBox->setEnabled(
      !busy && e0y_public_key_compatible);
  // Placeholder profiles remain fail-closed for CAN operations, but file
  // selection is an offline preparation action and must stay available.
  ui_->filesGroupBox->setEnabled(!busy && profile_valid);
  // Keep the probe action discoverable before a mode is selected. The click
  // handler reports the missing mode in the operator log and returns before
  // any monitor synchronization, CAN open, or probe request can occur.
  ui_->probeButton->setEnabled(!busy && usable);
  ui_->startFlashButton->setEnabled(!busy && usable && entry_mode_selected);
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

} // namespace uds::ui::qt
