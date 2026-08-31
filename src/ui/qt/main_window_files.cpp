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

  const auto application_directory = QCoreApplication::applicationDirPath();
  const auto persisted = [&application_directory](const QString& path) {
    return resourcePathForPersistence(path, application_directory);
  };

  QSettings settings;
  settings.beginGroup(QStringLiteral("flash_file_selections"));
  settings.beginGroup(active_file_selection_key_);
  settings.setValue(QStringLiteral("driver"), persisted(selection.driver_path));
  settings.setValue(QStringLiteral("app"), persisted(selection.app_path));
  settings.setValue(QStringLiteral("cal"), persisted(selection.cal_path));
  settings.setValue(QStringLiteral("driver_verify"),
                    persisted(selection.driver_verify_path));
  settings.setValue(QStringLiteral("app_verify"),
                    persisted(selection.app_verify_path));
  settings.setValue(QStringLiteral("cal_verify"),
                    persisted(selection.cal_verify_path));
  settings.setValue(QStringLiteral("seed_key_dll"),
                    persisted(selection.seed_key_dll_path));
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
      const auto application_directory = QCoreApplication::applicationDirPath();
      bool migrated{};
      const auto restored = [&settings, &application_directory, &migrated](
                                const QString& name) {
        const auto persisted = settings.value(name).toString();
        const auto resolved = resolvePersistedResourcePath(
            persisted, application_directory);
        if (resolved.migrated) {
          settings.setValue(name, resolved.persisted_path);
          migrated = true;
        }
        return resolved.absolute_path;
      };
      runtime_file_selections_.insert(
          active_file_selection_key_,
          RuntimeFileSelection{
              restored(QStringLiteral("driver")),
              restored(QStringLiteral("app")),
              restored(QStringLiteral("cal")),
              restored(QStringLiteral("driver_verify")),
              restored(QStringLiteral("app_verify")),
              restored(QStringLiteral("cal_verify")),
              restored(QStringLiteral("seed_key_dll")),
          });
      if (migrated) settings.sync();
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

} // namespace uds::ui::qt
