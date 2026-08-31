#include "ui/qt/ui_log_message_parser.hpp"

#include <QLocale>
#include <QRegularExpression>

namespace uds::ui::qt {

ParsedUiLogMessage parseUiLogMessage(const QString& message) {
  ParsedUiLogMessage parsed;
  parsed.payload = message;

  // Keep the match anchored at the start. This accepts contextual tags before
  // a real TX/RX record without misclassifying prose that merely mentions
  // diagnostic IDs, for example "selected: TX=...; RX=...".
  static const QRegularExpression directional_line(
      QStringLiteral(
          R"(^\s*((?:\[[^\]\r\n]+\]\s*)*)(TX|RX)\s*(\[[^\]\r\n]+\])(?:\s*(.*))$)"),
      QRegularExpression::CaseInsensitiveOption);
  const auto match = directional_line.match(message);
  if (!match.hasMatch()) return parsed;

  parsed.leadingPrefix = match.captured(1).trimmed();
  parsed.direction = match.captured(2).compare(
                         QStringLiteral("TX"), Qt::CaseInsensitive) == 0
                         ? LogDirection::Tx
                         : LogDirection::Rx;
  parsed.directionAndCanId =
      match.captured(2).toUpper() + QStringLiteral(" ") + match.captured(3);
  parsed.payload = match.captured(4).trimmed();

  static const QRegularExpression round_prefix(
      QStringLiteral(R"(\[第\s*\d+\s*/\s*\d+\s*次\])"));
  const auto round_match = round_prefix.match(parsed.leadingPrefix);
  if (round_match.hasMatch()) parsed.roundPrefix = round_match.captured(0);
  return parsed;
}

ProbeUiLogSummary summarizeProbeUiLog(const QString& message,
                                      const QString& can_open_summary) {
  if (message.contains(QStringLiteral("PASS：CAN硬件物理CH")) &&
      message.contains(QStringLiteral("已打开"))) {
    return {ProbeUiLogKind::CanOpened,
            can_open_summary.isEmpty()
                ? QStringLiteral("CAN已打开")
                : can_open_summary};
  }

  if (parseUiLogMessage(message).direction != LogDirection::None) {
    return {ProbeUiLogKind::WireMessage, message};
  }

  if (message.contains(QStringLiteral("WARN"), Qt::CaseInsensitive) &&
      (message.contains(QStringLiteral("刷新条件")) ||
       message.contains(QStringLiteral("ProgrammingPrecondition"),
                        Qt::CaseInsensitive) ||
       message.contains(QStringLiteral("7F 31 31")))) {
    if (message.contains(QStringLiteral("0x05"), Qt::CaseInsensitive)) {
      return {ProbeUiLogKind::RefreshWarning,
              QStringLiteral(
                  "WARN：刷新条件状态 0x05，当前项目策略允许继续")};
    }
    if (message.contains(QStringLiteral("7F 31 31"))) {
      return {ProbeUiLogKind::RefreshWarning,
              QStringLiteral(
                  "WARN：刷新条件检查返回 NRC 0x31，当前项目策略允许继续")};
    }
    return {ProbeUiLogKind::RefreshWarning, message};
  }

  if (message.contains(QStringLiteral("WARN：ASC日志创建失败"))) {
    return {ProbeUiLogKind::TraceWarning,
            QStringLiteral("WARN：ASC日志创建失败，在线探测继续执行")};
  }

  return {};
}

FlashPreparationUiLogSummary summarizeFlashPreparationUiLog(
    const QString& message) {
  if (parseUiLogMessage(message).direction != LogDirection::None) {
    return {FlashPreparationUiLogKind::RuntimeStarted, message};
  }

  if (message.startsWith(QStringLiteral("Flash target:")) ||
      message.startsWith(QStringLiteral("预检查：")) ||
      message.contains(QStringLiteral("CBF identity:")) ||
      (message.contains(QStringLiteral("signature state machine"),
                        Qt::CaseInsensitive) &&
       !message.contains(QStringLiteral("paired input preflight passed"),
                         Qt::CaseInsensitive)) ||
      message.contains(QStringLiteral("dedicated flow selected"),
                       Qt::CaseInsensitive)) {
    return {FlashPreparationUiLogKind::Hidden, {}};
  }

  if (message.startsWith(QStringLiteral("Pre-flash qualification:"))) {
    static const QRegularExpression status_expression(
        QStringLiteral(R"(Status=([^;]+))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = status_expression.match(message);
    const auto status = match.hasMatch() ? match.captured(1).trimmed().toUpper()
                                         : QStringLiteral("UNKNOWN");
    if (status == QStringLiteral("PASS")) {
      return {FlashPreparationUiLogKind::Qualification,
              QStringLiteral("刷写前条件检查：通过")};
    }
    return {FlashPreparationUiLogKind::Qualification,
            QStringLiteral("WARN：刷写前条件检查状态 %1").arg(status), true};
  }

  if (message.startsWith(QStringLiteral("CAN configuration:"))) {
    static const QRegularExpression configuration(
        QStringLiteral(
            R"(Hardware backend=([^;]+); Channel=(\d+); Nominal bitrate=(\d+) bit/s; Data bitrate=(\d+) bit/s; CAN FD=(yes|no))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = configuration.match(message);
    if (!match.hasMatch()) {
      return {FlashPreparationUiLogKind::CanConfiguration,
              QStringLiteral("CAN配置：已读取")};
    }
    const auto bitrate = [](const QString& value) {
      bool ok{};
      const auto bits_per_second = value.toULongLong(&ok);
      if (!ok) return value;
      if (bits_per_second % 1000000ULL == 0ULL) {
        return QStringLiteral("%1M").arg(bits_per_second / 1000000ULL);
      }
      if (bits_per_second % 1000ULL == 0ULL) {
        return QStringLiteral("%1K").arg(bits_per_second / 1000ULL);
      }
      return QString::number(bits_per_second);
    };
    return {FlashPreparationUiLogKind::CanConfiguration,
            QStringLiteral("CAN配置：%1，CH%2，%3/%4，%5")
                .arg(match.captured(1).trimmed(), match.captured(2),
                     bitrate(match.captured(3)), bitrate(match.captured(4)),
                     match.captured(5).compare(QStringLiteral("yes"),
                                               Qt::CaseInsensitive) == 0
                         ? QStringLiteral("CAN FD")
                         : QStringLiteral("CAN"))};
  }

  if (message.startsWith(QStringLiteral("Flash file:"))) {
    static const QRegularExpression configured_file(
        QStringLiteral(
            R"(^Flash file: ([^=]+)=.*; exists=(yes|no); size=(\d+) bytes$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = configured_file.match(message);
    if (!match.hasMatch()) {
      return {FlashPreparationUiLogKind::Hidden, {}};
    }
    const auto label = match.captured(1).trimmed();
    const auto exists = match.captured(2).compare(
                            QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
    bool size_ok{};
    const auto size = match.captured(3).toLongLong(&size_ok);
    const auto formatted_size =
        size_ok ? QLocale(QLocale::English, QLocale::UnitedStates).toString(size)
                : match.captured(3);
    if (label == QStringLiteral("Boot Driver")) {
      return {FlashPreparationUiLogKind::DriverFile,
              exists
                  ? QStringLiteral("Driver文件检查：已找到（%1 bytes）")
                        .arg(formatted_size)
                  : QStringLiteral("Driver文件检查：未找到"),
              !exists};
    }
    if (label == QStringLiteral("APP")) {
      return {FlashPreparationUiLogKind::AppFile,
              exists ? QStringLiteral("APP文件检查：已找到（%1 bytes）")
                           .arg(formatted_size)
                     : QStringLiteral("APP文件检查：未找到"),
              !exists};
    }
    if (label == QStringLiteral("SeedKey")) {
      return {FlashPreparationUiLogKind::SeedKey,
              exists ? QStringLiteral("Seed-Key组件检查：已配置")
                     : QStringLiteral("Seed-Key组件检查：未找到"),
              !exists};
    }
    return {FlashPreparationUiLogKind::Hidden, {}};
  }

  if ((message.contains(QStringLiteral("paired input preflight passed"),
                        Qt::CaseInsensitive) ||
       message.contains(QStringLiteral("Driver/APP"), Qt::CaseInsensitive) &&
           (message.contains(QStringLiteral("matched"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("匹配")))) &&
      !message.contains(QStringLiteral("failed"), Qt::CaseInsensitive)) {
    return {FlashPreparationUiLogKind::PairMatch,
            QStringLiteral("Driver与APP匹配检查：通过")};
  }

  if (message.contains(QStringLiteral("raw ASC"), Qt::CaseInsensitive) &&
      message.contains(QStringLiteral("raw BLF"), Qt::CaseInsensitive)) {
    const auto passed =
        message.contains(QStringLiteral("raw ASC PASS:"),
                         Qt::CaseInsensitive) &&
        message.contains(QStringLiteral("raw BLF PASS:"),
                         Qt::CaseInsensitive);
    return {FlashPreparationUiLogKind::Trace,
            passed ? QStringLiteral("ASC/BLF记录：已启动")
                   : QStringLiteral("WARN：ASC/BLF记录启动失败"),
            !passed};
  }

  static const QRegularExpression cycle_start(
      QStringLiteral(R"(^第(\d+)/(\d+)次完整刷写开始$)"));
  const auto cycle_match = cycle_start.match(message);
  if (cycle_match.hasMatch()) {
    return {FlashPreparationUiLogKind::Cycle,
            QStringLiteral("开始第%1/%2次刷写")
                .arg(cycle_match.captured(1), cycle_match.captured(2))};
  }

  return {};
}

} // namespace uds::ui::qt
