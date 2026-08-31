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

void MainWindow::appendUiLog(const QString& message, UiLogTone tone,
                             UiLogDestination destination) {
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
  const auto show_in_view = destination != UiLogDestination::FileOnly;
  const auto persist_to_file = destination != UiLogDestination::ViewOnly;
  if (show_in_view) {
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
  }
  if (persist_to_file && execution_log_file_ && execution_log_file_->isOpen()) {
    const auto persisted_line = QStringLiteral("[%1] %2\r\n")
                                    .arg(now.toString(QStringLiteral(
                                         "yyyy-MM-dd HH:mm:ss.zzz")),
                                         displayed_message)
                                    .toUtf8();
    execution_log_file_->write(persisted_line);
    execution_log_file_->flush();
  }
}

void MainWindow::appendProbeLogMessage(const QString& message) {
  // Preserve the full controller/service message in the detailed execution
  // log. Only the runtime view receives the concise summary selected below.
  appendUiLog(message, UiLogTone::Normal, UiLogDestination::FileOnly);

  // probe_start_description() carries the resolved endpoint after project
  // routing (for example FT or functional addressing). Use it to keep the CAN
  // summary accurate without exposing the strategy explanation itself.
  static const QRegularExpression resolved_endpoint(
      QStringLiteral(
          R"((?:功能|物理)寻址\s+0x([0-9A-Fa-f]+)\s*->\s*0x([0-9A-Fa-f]+))"));
  const auto endpoint_match = resolved_endpoint.match(message);
  if (endpoint_match.hasMatch()) {
    static const QRegularExpression summary_endpoint(
        QStringLiteral(R"(TX 0x[0-9A-Fa-f]+，RX 0x[0-9A-Fa-f]+$)"));
    probe_can_open_summary_.replace(
        summary_endpoint,
        QStringLiteral("TX 0x%1，RX 0x%2")
            .arg(endpoint_match.captured(1).toUpper(),
                 endpoint_match.captured(2).toUpper()));
  }

  const auto summary = summarizeProbeUiLog(message, probe_can_open_summary_);
  if (summary.kind == ProbeUiLogKind::Hidden) return;
  if (summary.kind == ProbeUiLogKind::WireMessage &&
      summary.message.contains(QStringLiteral("31 01 02 03"))) {
    probe_refresh_entry_checked_ = true;
  }
  const auto tone = summary.kind == ProbeUiLogKind::RefreshWarning ||
                            summary.kind == ProbeUiLogKind::TraceWarning
                        ? UiLogTone::Pending
                        : UiLogTone::Normal;
  appendUiLog(summary.message, tone, UiLogDestination::ViewOnly);
}

void MainWindow::flushPendingFlashPreparationSummary() {
  if (!pending_flash_trace_summary_.isEmpty()) {
    appendUiLog(pending_flash_trace_summary_, UiLogTone::Normal,
                UiLogDestination::ViewOnly);
    pending_flash_trace_summary_.clear();
  }
  if (!pending_flash_cycle_summary_.isEmpty()) {
    appendUiLog(pending_flash_cycle_summary_, UiLogTone::Normal,
                UiLogDestination::ViewOnly);
    pending_flash_cycle_summary_.clear();
  }
}

void MainWindow::beginFlashUiLog() {
  flash_ui_log_active_ = true;
  flash_preparation_ui_active_ = true;
  pending_flash_trace_summary_.clear();
  pending_flash_cycle_summary_.clear();
}

void MainWindow::appendFlashLogMessage(const QString& message) {
  appendUiLog(message, UiLogTone::Normal, UiLogDestination::FileOnly);
  const auto summary = summarizeFlashPreparationUiLog(message);

  if (summary.kind == FlashPreparationUiLogKind::Trace) {
    pending_flash_trace_summary_ = summary.message;
    return;
  }
  if (summary.kind == FlashPreparationUiLogKind::Cycle) {
    pending_flash_cycle_summary_ = summary.message;
    flash_preparation_ui_active_ = true;
    return;
  }

  if (flash_preparation_ui_active_) {
    if (summary.kind == FlashPreparationUiLogKind::RuntimeStarted) {
      flushPendingFlashPreparationSummary();
      flash_preparation_ui_active_ = false;
      appendUiLog(summary.message, UiLogTone::Normal,
                  UiLogDestination::ViewOnly);
      return;
    }
    if (summary.kind == FlashPreparationUiLogKind::PairMatch) {
      appendUiLog(summary.message, UiLogTone::Normal,
                  UiLogDestination::ViewOnly);
      flushPendingFlashPreparationSummary();
      return;
    }
    if (summary.kind == FlashPreparationUiLogKind::Unclassified ||
        summary.kind == FlashPreparationUiLogKind::Hidden) {
      return;
    }
    appendUiLog(summary.message,
                summary.warning ? UiLogTone::Pending : UiLogTone::Normal,
                UiLogDestination::ViewOnly);
    return;
  }

  if (summary.kind == FlashPreparationUiLogKind::Hidden ||
      summary.kind == FlashPreparationUiLogKind::Qualification ||
      summary.kind == FlashPreparationUiLogKind::CanConfiguration ||
      summary.kind == FlashPreparationUiLogKind::DriverFile ||
      summary.kind == FlashPreparationUiLogKind::AppFile ||
      summary.kind == FlashPreparationUiLogKind::SeedKey ||
      summary.kind == FlashPreparationUiLogKind::PairMatch) {
    return;
  }
  appendUiLog(summary.message.isEmpty() ? message : summary.message,
              summary.warning ? UiLogTone::Pending : UiLogTone::Normal,
              UiLogDestination::ViewOnly);
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
  flash_ui_log_active_ = false;
  flash_preparation_ui_active_ = false;
  pending_flash_trace_summary_.clear();
  pending_flash_cycle_summary_.clear();
}

void MainWindow::handleProbeFinished(bool success, bool cancelled,
                                     const QString& message) {
  probe_running_ = false;
  updateEnabledState();
  ui_->progressBar->setValue(success ? 100 : 0);
  ui_->progressStatusLabel->setText(message);
  const auto persisted_result =
      cancelled ? QStringLiteral("在线探测已停止：%1").arg(message)
                : (success ? QStringLiteral("● 在线：%1").arg(message)
                           : QStringLiteral("● 不在线：%1").arg(message));
  appendUiLog(persisted_result,
              success ? UiLogTone::Success : UiLogTone::Failure,
              UiLogDestination::FileOnly);
  if (cancelled) {
    appendUiLog(QStringLiteral("在线探测已停止"), UiLogTone::Failure,
                UiLogDestination::ViewOnly);
  } else if (success) {
    appendUiLog(probe_refresh_entry_checked_
                    ? QStringLiteral(
                          "在线探测成功：诊断响应正常，APP刷新入口判定可用")
                    : QStringLiteral("在线探测成功：设备在线"),
                UiLogTone::Success, UiLogDestination::ViewOnly);
  } else {
    appendUiLog(message.startsWith(QStringLiteral("在线探测"))
                    ? message
                    : QStringLiteral("在线探测失败：%1").arg(message),
                UiLogTone::Failure, UiLogDestination::ViewOnly);
  }
  probe_ui_log_active_ = false;
  probe_can_open_summary_.clear();
  probe_refresh_entry_checked_ = false;
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

} // namespace uds::ui::qt
