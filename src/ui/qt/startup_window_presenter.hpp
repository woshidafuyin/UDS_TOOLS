#pragma once

class QWidget;

namespace uds::ui::qt {

// Shows and foregrounds the main window once during application startup.
// The window is returned to the normal (non-always-on-top) z-order before
// this operation completes.
void presentWindowOnStartup(QWidget& window);

}  // namespace uds::ui::qt
