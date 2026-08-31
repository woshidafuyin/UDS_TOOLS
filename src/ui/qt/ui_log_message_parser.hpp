#pragma once

#include <QString>

namespace uds::ui::qt {

enum class LogDirection {
  None,
  Tx,
  Rx,
};

struct ParsedUiLogMessage {
  LogDirection direction{LogDirection::None};
  QString leadingPrefix;
  QString roundPrefix;
  QString directionAndCanId;
  QString payload;
};

// Parses only directional wire-log lines. Optional bracketed context prefixes
// are preserved so future forms such as "[device 1] [第2/3次] TX [...]" do
// not need special-case color rules. Non-directional messages are returned in
// payload unchanged.
[[nodiscard]] ParsedUiLogMessage parseUiLogMessage(const QString& message);

} // namespace uds::ui::qt
