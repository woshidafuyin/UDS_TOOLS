#include "ui/qt/ui_log_message_parser.hpp"

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

} // namespace uds::ui::qt
