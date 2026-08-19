#pragma once

#include <QString>

#include <vector>

namespace uds::ui::qt {

// Read-only presentation data adapted from the core version-check plan.
// The page does not parse Profiles and does not own selector state.
struct VersionReadItemView {
  QString did;
  QString request;
  QString meaning;
  bool required{};
};

using VersionReadItems = std::vector<VersionReadItemView>;

} // namespace uds::ui::qt
