#pragma once

#include <QString>

namespace uds::ui::qt {

struct ResourceFileReplaceResult {
  bool success{};
  QString stored_path;
  QString removed_path;
  QString cleanup_warning;
  QString error;
};

// Copies the selected file into the configured resource directory while
// retaining its original filename. After the new file is safely stored, the
// previous file managed by this field is removed only when it has the same
// suffix. Other files in the directory and the selected source are untouched.
[[nodiscard]] ResourceFileReplaceResult replaceConfiguredResourceFile(
    const QString& selected_path, const QString& configured_default_path,
    const QString& resources_root, const QString& previous_stored_path = {});

} // namespace uds::ui::qt
