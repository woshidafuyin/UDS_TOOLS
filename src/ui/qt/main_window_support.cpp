#include "ui/qt/main_window_support.hpp"

#include "ui/qt/ui_log_message_parser.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextCursor>
#include <QUrl>
#include <QVariant>
#include <QWidget>

namespace uds::ui::qt::main_window_support {
namespace {

QString settingsKeyComponent(const QString& value) {
  return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

} // namespace

std::optional<std::uint8_t> nrcFromLogLine(const QString& message) {
  static const QRegularExpression explicit_nrc(
      QStringLiteral(R"(NRC\s*(?:=|:)?\s*0x([0-9A-Fa-f]{2}))"),
      QRegularExpression::CaseInsensitiveOption);
  auto match = explicit_nrc.match(message);
  if (match.hasMatch()) {
    bool ok{};
    const auto value = match.captured(1).toUInt(&ok, 16);
    if (ok) return static_cast<std::uint8_t>(value);
  }

  const auto parsed = parseUiLogMessage(message);
  if (parsed.direction != LogDirection::Rx) return std::nullopt;
  static const QRegularExpression raw_negative(
      QStringLiteral(
          R"(^\s*7F\s+[0-9A-Fa-f]{2}\s+([0-9A-Fa-f]{2})(?=\s|$|\|))"),
      QRegularExpression::CaseInsensitiveOption);
  match = raw_negative.match(parsed.payload);
  if (!match.hasMatch()) return std::nullopt;
  bool ok{};
  const auto value = match.captured(1).toUInt(&ok, 16);
  return ok ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(value))
            : std::nullopt;
}

std::optional<UdsRoutineResult> failedRoutineFromLogLine(
    const QString& message) {
  const auto parsed = parseUiLogMessage(message);
  const auto response_line =
      parsed.direction == LogDirection::Rx ||
      message.contains(QStringLiteral("响应")) ||
      message.contains(QStringLiteral("response"), Qt::CaseInsensitive);
  if (!response_line) return std::nullopt;
  static const QRegularExpression raw_result(
      QStringLiteral(
          R"((?:^|\s|\[)71\s+[0-9A-Fa-f]{2}\s+([0-9A-Fa-f]{2})\s+([0-9A-Fa-f]{2})\s+(05)(?=\s|$|\]|\|))"),
      QRegularExpression::CaseInsensitiveOption);
  const auto match = raw_result.match(message);
  if (!match.hasMatch()) return std::nullopt;
  bool high_ok{}, low_ok{}, status_ok{};
  const auto high = match.captured(1).toUInt(&high_ok, 16);
  const auto low = match.captured(2).toUInt(&low_ok, 16);
  const auto status = match.captured(3).toUInt(&status_ok, 16);
  if (!high_ok || !low_ok || !status_ok) return std::nullopt;
  const UdsRoutineResult result{
      static_cast<std::uint16_t>((high << 8U) | low),
      static_cast<std::uint8_t>(status),
      status == 0x05U && ((high << 8U) | low) != 0x0203U};
  return result.failure ? std::optional<UdsRoutineResult>(result)
                        : std::nullopt;
}

QString fullPath(const QLineEdit* path_edit) {
  if (path_edit->property(kEmbeddedVerificationProperty).toBool()) return {};
  const auto stored = path_edit->property(kFullPathProperty).toString();
  return stored.isEmpty() ? path_edit->text().trimmed() : stored;
}

void showPath(QLineEdit* path_edit, const QString& path) {
  const auto normalized = path.isEmpty() ? QString{} : QDir::toNativeSeparators(path);
  const auto configured_placeholder =
      path_edit->property(kConfiguredPlaceholderProperty);
  if (!configured_placeholder.isValid()) {
    path_edit->setProperty(kConfiguredPlaceholderProperty,
                           path_edit->placeholderText());
  }
  path_edit->setPlaceholderText(
      normalized.isEmpty()
          ? QString{}
          : path_edit->property(kConfiguredPlaceholderProperty).toString());
  path_edit->setProperty(kFullPathProperty, normalized);
  path_edit->setToolTip(normalized);
  path_edit->setText(normalized.isEmpty()
                         ? QString{}
                         : QFileInfo(normalized).fileName());
  path_edit->setCursorPosition(0);
}

QString selectFile(QWidget* parent, const QLineEdit* path_edit,
                   const QString& caption, const QString& filter) {
  QString initial_directory = QDir::homePath();
  const auto current_path = fullPath(path_edit);
  const QFileInfo current(current_path);
  if (!current_path.isEmpty()) {
    initial_directory = current.isDir() ? current.absoluteFilePath()
                                        : current.absolutePath();
  }
  return QFileDialog::getOpenFileName(parent, caption, initial_directory,
                                      filter);
}

QString newestReportPath() {
  QDir logs(QDir(QCoreApplication::applicationDirPath()).filePath(
      QStringLiteral("logs/reports")));
  const auto reports = logs.entryInfoList(
      {QStringLiteral("*.html"), QStringLiteral("*.htm")},
      QDir::Files | QDir::Readable, QDir::Time);
  return reports.isEmpty()
             ? QString{}
             : QDir::toNativeSeparators(reports.front().absoluteFilePath());
}

std::optional<QString> localFileLinkAt(
    const QPlainTextEdit* log_view, const QPoint& viewport_position) {
  if (!log_view) return std::nullopt;
  auto cursor = log_view->cursorForPosition(viewport_position);
  auto format = cursor.charFormat();
  if (!format.isAnchor() && cursor.movePosition(QTextCursor::NextCharacter)) {
    format = cursor.charFormat();
  }
  if (!format.isAnchor()) return std::nullopt;

  const QUrl link(format.anchorHref());
  if (!link.isLocalFile()) return std::nullopt;
  const auto path = QDir::toNativeSeparators(link.toLocalFile());
  return path.isEmpty() ? std::nullopt
                        : std::optional<QString>(path);
}

QString canVendorKey(CanVendor vendor) {
  switch (vendor) {
  case CanVendor::Vector:
    return QStringLiteral("vector");
  case CanVendor::Tosun:
    return QStringLiteral("tosun");
  case CanVendor::Zlg:
    return QStringLiteral("zlg");
  case CanVendor::Kvaser:
    return QStringLiteral("kvaser");
  case CanVendor::Other:
    return QStringLiteral("other");
  }
  return QStringLiteral("vector");
}

CanVendor canVendorFromKey(const QString& key) {
  if (key.compare(QStringLiteral("tosun"), Qt::CaseInsensitive) == 0) {
    return CanVendor::Tosun;
  }
  if (key.compare(QStringLiteral("zlg"), Qt::CaseInsensitive) == 0) {
    return CanVendor::Zlg;
  }
  if (key.compare(QStringLiteral("kvaser"), Qt::CaseInsensitive) == 0) {
    return CanVendor::Kvaser;
  }
  return CanVendor::Vector;
}

QString canChannelSettingsKey(CanVendor vendor) {
  return QStringLiteral("hardware/channel/%1").arg(canVendorKey(vendor));
}

QString canVendorDisplayName(CanVendor vendor) {
  return QString::fromUtf8(can_vendor_name(vendor).data(),
                           static_cast<int>(can_vendor_name(vendor).size()));
}

QString vendorProjectSettingsKey(const QString& vendor) {
  return QStringLiteral("selectors/vendor_projects/%1")
      .arg(settingsKeyComponent(vendor));
}

QString projectDeviceSettingsGroup(const QString& vendor,
                                   const QString& project) {
  return QStringLiteral("selectors/project_devices/%1/%2")
      .arg(settingsKeyComponent(vendor), settingsKeyComponent(project));
}

QString profileStateSettingsGroup(const QString& profile_target_key) {
  return QStringLiteral("selectors/profile_state/%1").arg(profile_target_key);
}

} // namespace uds::ui::qt::main_window_support
