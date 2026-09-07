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
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace uds::ui::qt {
using namespace main_window_support;

void MainWindow::editProjectFlashParameters() {
  bool valid{};
  const int index = selectedProfileIndex(&valid);
  if (!valid) return;
  auto values = controller_bridge_->projectFlashSettings(index);
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("projectFlashParametersDialog"));
  dialog.setWindowTitle(QStringLiteral("P02C 项目刷写参数"));
  auto* layout = new QFormLayout(&dialog);
  auto* description = new QLabel(QStringLiteral("按 ECU 定义选择 CRC；身份字段前 10 字节为维修站代码，后 17 字节为测试仪序列号。\nS19 自动读取地址和长度；只有使用 BIN 时才需要填写对应地址。"), &dialog);
  description->setWordWrap(true);
  layout->addRow(description);
  auto* crc = new QComboBox(&dialog);
  crc->setObjectName("programmingCrcComboBox");
  crc->addItem(QStringLiteral("请选择已确认的 CRC 方式"), QString{});
  crc->addItem(QStringLiteral("Reflected"), QStringLiteral("reflected"));
  crc->addItem(QStringLiteral("Non-reflected"), QStringLiteral("non_reflected"));
  crc->setCurrentIndex(std::max(0, crc->findData(values.crc_variant)));
  layout->addRow(QStringLiteral("CRC 方式"), crc);
  const auto edit = [&](const QString& label, const char* name, const QString& value) {
    auto* field = new QLineEdit(value, &dialog); field->setObjectName(name);
    layout->addRow(label, field); return field;
  };
  auto* identity = edit(QStringLiteral("测试仪身份"), "programmingIdentityLineEdit", values.tester_identity);
  auto* driver = edit(QStringLiteral("Driver BIN 地址（可留空）"), "driverBinAddressLineEdit", values.driver_bin_address);
  auto* app = edit(QStringLiteral("APP BIN 地址（可留空）"), "appBinAddressLineEdit", values.app_bin_address);
  auto* cal = edit(QStringLiteral("CAL BIN 地址（可留空）"), "calBinAddressLineEdit", values.cal_bin_address);
  auto* errors = new QLabel(&dialog); errors->setWordWrap(true); errors->setStyleSheet("color: #D93025;");
  layout->addRow(errors);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  layout->addRow(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    values = {crc->currentData().toString(), identity->text(), driver->text().trimmed(),
              app->text().trimmed(), cal->text().trimmed()};
    const auto issues = validateProjectFlashSettings(values);
    if (!issues.isEmpty()) { errors->setText(issues.join('\n')); return; }
    if (!controller_bridge_->saveProjectParameters(index, values)) {
      errors->setText(QStringLiteral("参数保存失败，请检查本机设置写入权限。")); return;
    }
    dialog.accept();
  });
  if (dialog.exec() == QDialog::Accepted)
    appendUiLog(QStringLiteral("P02C 刷写参数已保存并立即生效，下次启动自动恢复。"));
}

bool MainWindow::validateFlashFilesFromUi(int profile_index,
                                         const QString& entry_mode) {
  const auto& profile = controller_bridge_->profileOptions().at(
      static_cast<std::size_t>(profile_index));
  bool valid = true;
  const auto reject = [this, &valid](const QString& message) {
    appendUiLog(message, UiLogTone::Failure);
    valid = false;
  };
  const bool chery = profile.flow_id.startsWith(QStringLiteral("chery_"));
  for (const auto& error : controller_bridge_->projectParameterErrors(profile_index, entry_mode,
       fullPath(ui_->driverPathLineEdit), fullPath(ui_->appPathLineEdit), fullPath(ui_->calPathLineEdit)))
    reject(error);
  const bool chuneng = profile.flow_id == QStringLiteral("chuneng_arc331");
  const bool xizhong = profile.flow_id == QStringLiteral("xizhong_rsmr") ||
                       profile.flow_id == QStringLiteral("xizhong_lsmr");
  const bool driver_cbf = chuneng && fullPath(ui_->driverPathLineEdit)
      .endsWith(QStringLiteral(".cbf"), Qt::CaseInsensitive);
  const bool app_cbf = chuneng && fullPath(ui_->appPathLineEdit)
      .endsWith(QStringLiteral(".cbf"), Qt::CaseInsensitive);
  if (chuneng && driver_cbf != app_cbf)
    reject(QStringLiteral("楚能 Driver 与 APP 必须同时使用 CBF，或同时使用 S-record 并配套校验文件。"));
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
    reject(QStringLiteral("刷写文件无效：当前 TMP 升级包结构解析失败，请重新选择。"));
  }
  const auto needs_app_verification =
      (needs_app && !embedded_tmp && !optional_lingpao_certificate &&
       (chery || xizhong || !profile.app_verify_path.isEmpty() ||
        profile.supports_app_tmp_package ||
        (chuneng && !app_cbf && !driver_cbf))) ||
      (profile.profile_id == QStringLiteral("chery_t22") &&
       entry_mode == QStringLiteral("cal"));
  const auto needs_cal =
      entry_mode == QStringLiteral("cal") ||
      entry_mode == QStringLiteral("app_cal") ||
      profile.flow_id == QStringLiteral("geely_p416");
  const auto required_files = std::array{
      std::tuple{QStringLiteral("Driver"), profile.flow_id != QStringLiteral("lp_arf"),
                 fullPath(ui_->driverPathLineEdit)},
      std::tuple{QStringLiteral("Driver 校验"),
                 chery || (chuneng && !driver_cbf) || !profile.driver_verify_path.isEmpty(),
                 fullPath(ui_->driverVerifyPathLineEdit)},
      std::tuple{QStringLiteral("APP"), needs_app,
                 fullPath(ui_->appPathLineEdit)},
      std::tuple{profile.app_verify_label.isEmpty()
                     ? QStringLiteral("APP 校验")
                     : profile.app_verify_label,
                  needs_app_verification,
                 fullPath(ui_->appVerifyPathLineEdit)},
      std::tuple{profile.flow_id == QStringLiteral("geely_p416") ? QStringLiteral("ESS") : QStringLiteral("CAL"), needs_cal,
                 fullPath(ui_->calPathLineEdit)},
      std::tuple{QStringLiteral("CAL 校验"), needs_cal && (chery || !profile.cal_verify_path.isEmpty()),
                 fullPath(ui_->calVerifyPathLineEdit)},
      std::tuple{profile.uses_oem_key_file ? QStringLiteral("OEM Key") : QStringLiteral("SeedKey 算法库"),
                 profile.uses_oem_key_file || chery || !profile.seed_key_dll_path.isEmpty(),
                 fullPath(ui_->seedKeyDllPathLineEdit)}};
  for (const auto& [label, required, selected_path] :
       required_files) {
    if (!required) continue;
    if (selected_path.trimmed().isEmpty()) {
      reject(QStringLiteral("未选择 %1 文件：请先选择后再执行“能否刷写”或“开始刷写”。")
                 .arg(label));
    } else {
      const QFileInfo file(selected_path);
      if (!file.isFile() || !file.isReadable()) {
        reject(QStringLiteral("%1 文件不存在或不可读取：%2").arg(label, selected_path));
      }
    }
  }
  return valid;
}

void MainWindow::startProbeFromUi() {
  const auto entry_mode = ui_->entryModeComboBox->currentData().toString();
  if (entry_mode.isEmpty()) {
    appendUiLog(
        QStringLiteral(
            "未配置刷写模式：请先选择刷写模式后再执行“能否刷写”。"),
        UiLogTone::Failure);
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
  if (!validateFlashFilesFromUi(profile_index, entry_mode)) return;
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
    appendUiLog(QStringLiteral("未配置刷写模式：请先选择刷写模式后再执行“开始刷写”。"),
                UiLogTone::Failure);
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
  if (!validateFlashFilesFromUi(profile_index, entry_mode)) return;

  followSelectedBusMonitorContext();
  if (!monitorMatchesSelectedHardware(profile_index)) {
    QMessageBox::warning(this, QStringLiteral("监听通道配置不一致"),
                         QStringLiteral("自动监听未能切换到当前 CAN 后端、通道或速率配置，"
                                        "本次刷写已阻止。"));
    return;
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
      (profiles[profile_index].flow_id == QStringLiteral("chery_e0y") ||
       profiles[profile_index].flow_id == QStringLiteral("perodua_p02c"));
  ui_->txIdLineEdit->setReadOnly(e0y_endpoint_locked);
  ui_->rxIdLineEdit->setReadOnly(e0y_endpoint_locked);
  ui_->entryModeComboBox->setEnabled(!busy && profile_valid);
  ui_->repeatCountSpinBox->setEnabled(!busy && usable);
  const bool project_parameters = profile_valid && profiles[profile_index].flow_id == QStringLiteral("perodua_p02c");
  ui_->projectParametersButton->setVisible(project_parameters);
  ui_->projectParametersButton->setEnabled(!busy && usable && project_parameters);
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
  const bool driver_used = profile_valid && profiles[profile_index].flow_id != QStringLiteral("lp_arf");
  ui_->driverPathLineEdit->setEnabled(driver_used);
  ui_->driverBrowseButton->setEnabled(driver_used);
  ui_->driverPathLabel->setEnabled(driver_used);
  // Both actions remain clickable without a mode so preflight can explain
  // missing inputs in the operator log before monitor synchronization or
  // dispatching a probe/flash request. Busy/placeholder profiles stay locked.
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

} // namespace uds::ui::qt
