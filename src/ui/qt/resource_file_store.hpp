#pragma once

#include <QString>

namespace uds::ui::qt {

struct ResourceFileReplaceResult {
  bool success{};
  QString stored_path;
  QString error;
};

// Copies the selected file into the configured resource directory while
// retaining its original filename. Existing files with other names, including
// the configured default and previous selections, are never removed. The
// selected source is also left untouched.
[[nodiscard]] ResourceFileReplaceResult replaceConfiguredResourceFile(
    const QString& selected_path, const QString& configured_default_path,
    const QString& resources_root);

} // namespace uds::ui::qt
