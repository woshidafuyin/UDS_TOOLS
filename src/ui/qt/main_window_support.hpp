#pragma once

#include "drivers/can/can_bus_provider.hpp"
#include "core/uds_nrc.hpp"

#include <QString>

#include <cstdint>
#include <optional>

class QLineEdit;
class QPlainTextEdit;
class QPoint;
class QWidget;

namespace uds::ui::qt::main_window_support {

inline constexpr auto kFullPathProperty = "fullPath";
inline constexpr auto kConfiguredPlaceholderProperty =
    "configuredPathPlaceholder";
inline constexpr auto kEmbeddedVerificationProperty = "embeddedVerification";
inline constexpr auto kPackageValidProperty = "appPackageValid";
inline constexpr auto kFlashFileFieldProperty = "flashFileField";

std::optional<std::uint8_t> nrcFromLogLine(const QString& message);
std::optional<UdsRoutineResult> failedRoutineFromLogLine(
    const QString& message);

QString fullPath(const QLineEdit* path_edit);
void showPath(QLineEdit* path_edit, const QString& path);
QString selectFile(QWidget* parent, const QLineEdit* path_edit,
                   const QString& caption, const QString& filter);
QString newestReportPath();
std::optional<QString> localFileLinkAt(const QPlainTextEdit* log_view,
                                       const QPoint& viewport_position);

QString canVendorKey(CanVendor vendor);
CanVendor canVendorFromKey(const QString& key);
QString canChannelSettingsKey(CanVendor vendor);
QString canVendorDisplayName(CanVendor vendor);

QString vendorProjectSettingsKey(const QString& vendor);
QString projectDeviceSettingsGroup(const QString& vendor,
                                   const QString& project);
QString profileStateSettingsGroup(const QString& profile_target_key);

} // namespace uds::ui::qt::main_window_support
