#include "ui/qt/resource_file_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

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

} // namespace

ResourceFileReplaceResult replaceConfiguredResourceFile(
    const QString& selected_path, const QString& configured_default_path,
    const QString& resources_root, const QString& previous_stored_path) {
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
  const auto destination = canonicalOrAbsolute(
      QDir(destination_directory).filePath(selected.fileName()));
  if (!isInside(destination, root) ||
      !samePath(QFileInfo(destination).absolutePath(),
                destination_directory)) {
    result.error = QStringLiteral("保留原文件名后的目标超出默认资源目录：%1")
                       .arg(QDir::toNativeSeparators(destination));
    return result;
  }

  const auto selected_absolute = canonicalOrAbsolute(selected_path);
  const auto previous = canonicalOrAbsolute(
      previous_stored_path.trimmed().isEmpty() ? configured_default_path
                                                : previous_stored_path);
  const QFileInfo previous_info(previous);
  const auto previous_is_managed =
      isInside(previous, root) &&
      samePath(previous_info.absolutePath(), destination_directory);
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

  const auto suffix_matches =
      !selected.suffix().isEmpty() &&
      previous_info.suffix().compare(selected.suffix(), Qt::CaseInsensitive) ==
          0;
  if (previous_is_managed && suffix_matches &&
      !samePath(previous, destination) && previous_info.isFile()) {
    if (QFile::remove(previous)) {
      result.removed_path = previous;
    } else {
      result.cleanup_warning =
          QStringLiteral("新文件已复制，但同后缀旧文件无法删除：%1")
              .arg(QDir::toNativeSeparators(previous));
    }
  }

  result.success = true;
  result.stored_path = destination;
  return result;
}

} // namespace uds::ui::qt
