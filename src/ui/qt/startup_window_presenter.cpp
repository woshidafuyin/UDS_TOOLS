#include "ui/qt/startup_window_presenter.hpp"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace uds::ui::qt {
namespace {

void requestOneTimeForeground(QWidget& window) {
  if (!window.isVisible()) {
    return;
  }

  if (window.isMinimized()) {
    window.showNormal();
  }
  window.raise();
  window.activateWindow();

#ifdef Q_OS_WIN
  const auto handle = reinterpret_cast<HWND>(window.winId());
  if (handle == nullptr) {
    return;
  }

  constexpr UINT flags =
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
  // A short TOPMOST -> NOTOPMOST transition makes a user-launched process
  // visible above existing windows without leaving an always-on-top window.
  SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, flags);
  SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
  SetForegroundWindow(handle);
#endif
}

}  // namespace

void presentWindowOnStartup(QWidget& window) {
  window.show();

  const QPointer<QWidget> guarded_window(&window);
  QTimer::singleShot(0, &window, [guarded_window]() {
    if (guarded_window != nullptr) {
      requestOneTimeForeground(*guarded_window);
    }
  });
}

}  // namespace uds::ui::qt
