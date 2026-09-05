#include "ui/qt/resource_file_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <memory>

namespace uds::ui::qt {
namespace {

QString canonicalOrAbsolute(const QString& path) {
  const QFileInfo info(path);
  const auto canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                             : canonical);
}

bool isInside(const QString& child_path, const QString& parent_path) {
  const auto child = QDir::fromNativeSeparators(child_path);
  auto parent = QDir::fromNativeSeparators(parent_path);
  if (!parent.endsWith(QLatin1Char('/'))) parent += QLatin1Char('/');
  return child.startsWith(parent, Qt::CaseInsensitive);
}

bool samePath(const QString& left, const QString& right) {
  return canonicalOrAbsolute(left).compare(canonicalOrAbsolute(right),
                                           Qt::CaseInsensitive) == 0;
}

QString normalizedAbsolute(const QString& path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString normalizedRelative(const QString& path) {
  return QDir::toNativeSeparators(
      QDir::cleanPath(QDir::fromNativeSeparators(path)));
}

QString managedRelativePath(const QString& absolute_path,
                            const QString& application_directory) {
  const auto application_root = normalizedAbsolute(application_directory);
  const auto resources_root =
      normalizedAbsolute(QDir(application_root).filePath(QStringLiteral("resources")));
  const auto candidate = normalizedAbsolute(absolute_path);
  if (!isInside(candidate, resources_root)) return {};
  return normalizedRelative(QDir(application_root).relativeFilePath(candidate));
}

QString legacyResourcesRelativePath(const QString& absolute_path) {
  const auto portable = QDir::fromNativeSeparators(QDir::cleanPath(absolute_path));
  const auto marker = QStringLiteral("/resources/");
  const auto marker_index =
      portable.lastIndexOf(marker, -1, Qt::CaseInsensitive);
  if (marker_index < 0) return {};
  return normalizedRelative(portable.mid(marker_index + 1));
}

} // namespace

QString resourcePathForPersistence(const QString& path,
                                   const QString& application_directory) {
  const auto trimmed = path.trimmed();
  if (trimmed.isEmpty()) return {};
  if (QDir::isRelativePath(trimmed)) return normalizedRelative(trimmed);
  const auto relative = managedRelativePath(trimmed, application_directory);
  return relative.isEmpty() ? normalizedAbsolute(trimmed) : relative;
}

PersistedResourcePathResolution resolvePersistedResourcePath(
    const QString& persisted_path, const QString& application_directory) {
  PersistedResourcePathResolution result;
  const auto trimmed = persisted_path.trimmed();
  if (trimmed.isEmpty()) return result;

  if (QDir::isRelativePath(trimmed)) {
    result.persisted_path = normalizedRelative(trimmed);
    result.absolute_path = normalizedAbsolute(
        QDir(application_directory).filePath(result.persisted_path));
    result.migrated = result.persisted_path != trimmed;
    return result;
  }

  const auto absolute = normalizedAbsolute(trimmed);
  const auto current_relative =
      managedRelativePath(absolute, application_directory);
  if (!current_relative.isEmpty()) {
    result.absolute_path = absolute;
    result.persisted_path = current_relative;
    result.migrated = true;
    return result;
  }

  const auto legacy_relative = legacyResourcesRelativePath(absolute);
  if (!legacy_relative.isEmpty()) {
    const auto current_candidate = normalizedAbsolute(
        QDir(application_directory).filePath(legacy_relative));
    if (QFileInfo::exists(current_candidate)) {
      result.absolute_path = current_candidate;
      result.persisted_path = legacy_relative;
      result.migrated = true;
      return result;
    }
  }

  result.absolute_path = absolute;
  result.persisted_path = absolute;
  return result;
}

ResourceFileReplaceResult replaceConfiguredResourceFile(
    const QString& selected_path, const QString& configured_default_path,
    const QString& resources_root) {
  ResourceFileReplaceResult result;
  const QFileInfo selected(selected_path);
  if (!selected.isFile() || !selected.isReadable()) {
    result.error = QStringLiteral("所选文件不存在或不可读取：%1")
                       .arg(QDir::toNativeSeparators(selected_path));
    return result;
  }
  if (configured_default_path.trimmed().isEmpty()) {
    result.error = QStringLiteral("当前配置项没有 resources 默认文件，无法确定替换目标");
    return result;
  }

  const auto root = canonicalOrAbsolute(resources_root);
  const auto configured = canonicalOrAbsolute(configured_default_path);
  if (!isInside(configured, root)) {
    result.error = QStringLiteral("默认文件不在 resources 目录内，已拒绝替换：%1")
                       .arg(QDir::toNativeSeparators(configured));
    return result;
  }

  const auto destination_directory =
      canonicalOrAbsolute(QFileInfo(configured).absolutePath());
  auto destination = canonicalOrAbsolute(
      QDir(destination_directory).filePath(selected.fileName()));
  if (!isInside(destination, root) ||
      !samePath(QFileInfo(destination).absolutePath(),
                destination_directory)) {
    result.error = QStringLiteral("保留原文件名后的目标超出默认资源目录：%1")
                       .arg(QDir::toNativeSeparators(destination));
    return result;
  }

  const auto selected_absolute = canonicalOrAbsolute(selected_path);
  // Paths may already be referenced by other fields, targets or saved jobs.
  // Never replace their contents when importing a different same-named file.
  // Keep the original filename in a fresh directory instead of guessing which
  // UI field owns an existing resource.
  std::unique_ptr<QTemporaryDir> import_directory;
  if (!samePath(selected_absolute, destination) && QFileInfo::exists(destination)) {
    import_directory = std::make_unique<QTemporaryDir>(
        QDir(destination_directory).filePath(QStringLiteral("selected-XXXXXX")));
    if (!import_directory->isValid()) {
      result.error = QStringLiteral("无法创建同名文件的独立保存目录：%1")
                         .arg(QDir::toNativeSeparators(destination_directory));
      return result;
    }
    destination = QDir(import_directory->path()).filePath(selected.fileName());
  }
  if (!samePath(selected_absolute, destination)) {
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
      result.error = QStringLiteral("无法写入默认资源文件：%1")
                         .arg(QDir::toNativeSeparators(destination));
      return result;
    }
    QFile input(selected.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly)) {
      output.cancelWriting();
      result.error = QStringLiteral("无法读取所选文件：%1")
                         .arg(QDir::toNativeSeparators(selected_path));
      return result;
    }
    constexpr qint64 kChunkSize = 1024 * 1024;
    while (!input.atEnd()) {
      const auto chunk = input.read(kChunkSize);
      if (chunk.isEmpty() && input.error() != QFile::NoError) {
        output.cancelWriting();
        result.error = QStringLiteral("读取所选文件失败：%1")
                           .arg(QDir::toNativeSeparators(selected_path));
        return result;
      }
      if (output.write(chunk) != chunk.size()) {
        output.cancelWriting();
        result.error = QStringLiteral("写入默认资源文件失败：%1")
                           .arg(QDir::toNativeSeparators(destination));
        return result;
      }
    }
    if (!output.commit()) {
      result.error = QStringLiteral("提交默认资源文件失败：%1")
                         .arg(QDir::toNativeSeparators(destination));
      return result;
    }
  }

  result.success = true;
  result.stored_path = destination;
  if (import_directory) import_directory->setAutoRemove(false);
  return result;
}

} // namespace uds::ui::qt
