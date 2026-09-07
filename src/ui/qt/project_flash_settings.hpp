#pragma once
#include "core/profile.hpp"
#include <QString>
#include <QStringList>

namespace uds::ui::qt {
// Operator choices, kept separate from the customer's protocol defaults.
struct ProjectFlashSettings {
  QString crc_variant;
  QString tester_identity;
  QString driver_bin_address;
  QString app_bin_address;
  QString cal_bin_address;
};
bool hasProjectFlashSettings(const FlashProfile& profile);
ProjectFlashSettings loadProjectFlashSettings(const FlashProfile& profile);
QStringList validateProjectFlashSettings(const ProjectFlashSettings& settings);
bool saveProjectFlashSettings(const FlashProfile& profile,
                              const ProjectFlashSettings& settings);
QStringList validateProjectFlashInputs(const FlashProfile& profile,
    const ProjectFlashSettings& settings, const QString& mode,
    const QString& driver, const QString& app, const QString& cal);
void applyProjectFlashSettings(FlashProfile& profile,
                               const ProjectFlashSettings& settings);
}
