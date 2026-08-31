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

} // namespace uds::ui::qt
