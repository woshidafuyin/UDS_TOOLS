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

} // namespace uds::ui::qt
