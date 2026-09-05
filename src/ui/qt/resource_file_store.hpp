#pragma once

#include <QString>

namespace uds::ui::qt {

struct ResourceFileReplaceResult {
  bool success{};
  QString stored_path;
  QString error;
};

struct PersistedResourcePathResolution {
  QString absolute_path;
  QString persisted_path;
  bool migrated{};
};

// Stores files managed under <application_directory>/resources as portable
// paths relative to CH_Diagnostic_Studio.exe. Paths outside the managed resources
// tree remain absolute so external-file semantics are not changed.
[[nodiscard]] QString resourcePathForPersistence(
    const QString& path, const QString& application_directory);

// Resolves a persisted path against the current CH_Diagnostic_Studio.exe directory.
// Legacy absolute paths from a moved dist are migrated only when their
// resources/... suffix exists under the current application directory.
[[nodiscard]] PersistedResourcePathResolution resolvePersistedResourcePath(
    const QString& persisted_path, const QString& application_directory);

// Copies the selected file into the configured resource directory while
// retaining its original filename. Existing files with other names, including
// the configured default and previous selections, are never removed. The
// selected source is also left untouched.
// Same-name collisions are stored in unique subdirectories: existing resource
// contents remain unchanged even when another field or saved job refers to them.
[[nodiscard]] ResourceFileReplaceResult replaceConfiguredResourceFile(
    const QString& selected_path, const QString& configured_default_path,
    const QString& resources_root);

} // namespace uds::ui::qt
