#include "ui/qt/project_flash_settings.hpp"
#include <QSettings>
#include <QFileInfo>
#include <array>
#include <tuple>
#include <limits>

namespace uds::ui::qt {
namespace {
QString settingsGroup(const FlashProfile& p) {
  return QStringLiteral("project_flash_parameters/") + QString::fromStdWString(p.id);
}
bool parseAddress(const QString& text, quint32* result = nullptr) {
  bool ok{};
  const auto input = text.trimmed();
  const auto value = input.toULongLong(&ok, input.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
  ok = ok && value <= std::numeric_limits<quint32>::max();
  if (ok && result) *result = static_cast<quint32>(value);
  return ok;
}
QString initialAddress(std::uint32_t value) {
  return value ? QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')) : QString{};
}
}
bool hasProjectFlashSettings(const FlashProfile& p) { return p.flow == L"perodua_p02c"; }
ProjectFlashSettings loadProjectFlashSettings(const FlashProfile& p) {
  QSettings s;
  s.beginGroup(settingsGroup(p));
  return {s.value("crc", QString::fromStdWString(p.programming_crc_variant)).toString(),
          s.value("identity", QString::fromStdWString(p.programming_tester_identity)).toString(),
          s.value("driver_bin", initialAddress(p.driver_start)).toString(),
          s.value("app_bin", initialAddress(p.app_start)).toString(),
          s.value("cal_bin", initialAddress(p.cal_start)).toString()};
}
QStringList validateProjectFlashSettings(const ProjectFlashSettings& v) {
  QStringList errors;
  if (v.crc_variant != "reflected" && v.crc_variant != "non_reflected")
    errors << QStringLiteral("请在“项目刷写参数”中选择已确认的 CRC 方式。");
  bool ascii = !v.tester_identity.trimmed().isEmpty() && v.tester_identity.size() <= 27;
  for (const auto c : v.tester_identity) ascii = ascii && c.unicode() >= 0x20 && c.unicode() <= 0x7e;
  if (!ascii) errors << QStringLiteral("请在“项目刷写参数”中填写测试仪身份（1～27 个可打印 ASCII 字符）。");
  for (const auto& [label, value] : std::array{
       std::pair{QStringLiteral("Driver"), v.driver_bin_address},
       std::pair{QStringLiteral("APP"), v.app_bin_address},
       std::pair{QStringLiteral("CAL"), v.cal_bin_address}}) {
    if (!value.isEmpty() && !parseAddress(value))
      errors << label + QStringLiteral(" BIN 地址必须是 0～0xFFFFFFFF 的十进制或 0x 十六进制数。");
  }
  return errors;
}
bool saveProjectFlashSettings(const FlashProfile& p, const ProjectFlashSettings& v) {
  if (!hasProjectFlashSettings(p) || !validateProjectFlashSettings(v).isEmpty()) return false;
  QSettings s;
  s.beginGroup(settingsGroup(p));
  s.setValue("crc", v.crc_variant); s.setValue("identity", v.tester_identity);
  s.setValue("driver_bin", v.driver_bin_address); s.setValue("app_bin", v.app_bin_address);
  s.setValue("cal_bin", v.cal_bin_address); s.sync();
  return s.status() == QSettings::NoError;
}
QStringList validateProjectFlashInputs(const FlashProfile& p, const ProjectFlashSettings& v,
    const QString& mode, const QString& driver, const QString& app, const QString& cal) {
  if (!hasProjectFlashSettings(p)) return {};
  auto errors = validateProjectFlashSettings(v);
  for (const auto& [label, file, address, used] : std::array{
       std::tuple{QStringLiteral("Driver"), driver, v.driver_bin_address, true},
       std::tuple{QStringLiteral("APP"), app, v.app_bin_address, mode != "cal"},
       std::tuple{QStringLiteral("CAL"), cal, v.cal_bin_address, mode != "app"}}) {
    if (used && QFileInfo(file).suffix().compare("bin", Qt::CaseInsensitive) == 0 && !parseAddress(address))
      errors << label + QStringLiteral(" 使用 BIN：请在“项目刷写参数”填写已确认的起始地址；S19 无需填写。");
  }
  return errors;
}
void applyProjectFlashSettings(FlashProfile& p, const ProjectFlashSettings& v) {
  if (!hasProjectFlashSettings(p)) return;
  p.programming_crc_variant = v.crc_variant.toStdWString();
  p.programming_tester_identity = v.tester_identity.toStdWString();
  parseAddress(v.driver_bin_address, &p.driver_start);
  parseAddress(v.app_bin_address, &p.app_start);
  parseAddress(v.cal_bin_address, &p.cal_start);
}
}
