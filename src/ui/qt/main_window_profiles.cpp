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
  const auto chery_e0y =
      profile.flow_id == QStringLiteral("chery_e0y");
  ui_->updatePublicKeyLabel->setVisible(chery_e0y);
  ui_->updatePublicKeyCheckBox->setVisible(chery_e0y);
  ui_->updatePublicKeyCheckBox->setChecked(false);
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
  // Flash entry is an operator decision. Never infer it from the Profile or a
  // previous run. Use QComboBox's non-selectable placeholder so the popup only
  // contains real operation modes.
  ui_->entryModeComboBox->setPlaceholderText(QStringLiteral("请选择"));
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
  ui_->entryModeComboBox->setCurrentIndex(-1);
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
  // Entry mode is intentionally session-only and must be chosen again after
  // every Profile/target change and every application restart.
  settings.remove(QStringLiteral("entry_mode"));
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
  settings.remove(QStringLiteral("selectors/entry_mode"));
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
  // Do not restore the previous entry mode. The combo was reset to its
  // non-selectable "请选择" placeholder by applySelectedProfile(). Remove
  // obsolete persisted values so older releases cannot reintroduce a hidden
  // default.
  settings.remove(state_group + QStringLiteral("/entry_mode"));
  settings.remove(QStringLiteral("selectors/entry_mode"));

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

} // namespace uds::ui::qt
