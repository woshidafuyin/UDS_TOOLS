#pragma once

#include <QString>

namespace uds::ui::qt {

struct ResourceFileReplaceResult {
  bool success{};
  QString stored_path;
  QString error;
};

// Atomically replaces the configured default resource with the selected file.
// Keeping the configured destination path stable means Profile files, restart
// defaults and the running UI all continue to reference the same resource.
[[nodiscard]] ResourceFileReplaceResult replaceConfiguredResourceFile(
    const QString& selected_path, const QString& configured_default_path,
    const QString& resources_root);

} // namespace uds::ui::qt
