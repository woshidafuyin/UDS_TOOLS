#pragma once

#include <QWidget>

namespace uds::ui::qt {

class DiagnosticPage final : public QWidget {
public:
  explicit DiagnosticPage(QWidget* parent = nullptr);
};

} // namespace uds::ui::qt
