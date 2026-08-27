#include "ui/qt/main_window.hpp"
#include "ui/qt/startup_window_presenter.hpp"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QStyleFactory>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  if (QStyleFactory::keys().contains(QStringLiteral("windowsvista"),
                                     Qt::CaseInsensitive)) {
    application.setStyle(QStyleFactory::create(QStringLiteral("windowsvista")));
  }
  application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
  QApplication::setOrganizationName(QStringLiteral("UDSTools"));
  QApplication::setApplicationName(QStringLiteral("uds_tool_qt"));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/uds_flash_tool.ico")));

  uds::ui::qt::MainWindow window;
  uds::ui::qt::presentWindowOnStartup(window);
  window.startDefaultBusMonitoring();

  return application.exec();
}
