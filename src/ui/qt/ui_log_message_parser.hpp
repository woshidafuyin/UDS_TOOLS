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

enum class ProbeUiLogKind {
  Hidden,
  CanOpened,
  WireMessage,
  RefreshWarning,
  TraceWarning,
};

struct ProbeUiLogSummary {
  ProbeUiLogKind kind{ProbeUiLogKind::Hidden};
  QString message;
};

enum class FlashPreparationUiLogKind {
  Unclassified,
  Hidden,
  Qualification,
  CanConfiguration,
  DriverFile,
  AppFile,
  SeedKey,
  PairMatch,
  Trace,
  Cycle,
  RuntimeStarted,
};

struct FlashPreparationUiLogSummary {
  FlashPreparationUiLogKind kind{
      FlashPreparationUiLogKind::Unclassified};
  QString message;
  bool warning{};
};

// Parses only directional wire-log lines. Optional bracketed context prefixes
// are preserved so future forms such as "[device 1] [第2/3次] TX [...]" do
// not need special-case color rules. Non-directional messages are returned in
// payload unchanged.
[[nodiscard]] ParsedUiLogMessage parseUiLogMessage(const QString& message);

// Selects only operator-relevant online-probe messages for the runtime view.
// The caller remains responsible for persisting the original message so this
// presentation filter cannot reduce execution-log or ASC/BLF evidence.
[[nodiscard]] ProbeUiLogSummary summarizeProbeUiLog(
    const QString& message, const QString& can_open_summary);

// Produces the short operator-facing preparation summary while leaving the
// original configuration, file, CBF and trace details available to the file
// log and HTML report.
[[nodiscard]] FlashPreparationUiLogSummary summarizeFlashPreparationUiLog(
    const QString& message);

} // namespace uds::ui::qt
