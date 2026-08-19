#pragma once

#include "ui/qt/version_read_view_model.hpp"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;
class QPlainTextEdit;

namespace uds::ui::qt {

class VersionConfirmationPage final : public QWidget {
  Q_OBJECT

public:
  explicit VersionConfirmationPage(QWidget* parent = nullptr);

  void setContext(int profile_index, const QString& source,
                  const QString& vendor, const QString& project,
                  const QString& target_id, const QString& target_name,
                  unsigned channel, quint32 tx_id, quint32 rx_id,
                  const VersionReadItems& items);
  void setRunning(bool running);
  void setOperationBusy(bool busy);
  void clearResults();
  void appendResult(const QString& status, const QString& request,
                    const QString& name, const QString& actual,
                    const QString& raw_response);
  void finish(bool success, bool cancelled, const QString& message);
  void setReportAvailable(bool available);

signals:
  void checkRequested(int profile_index, const QString& target_id,
                      unsigned channel, quint32 tx_id, quint32 rx_id);
  void stopRequested();
  void openReportRequested();

private:
  void requestCheck();
  void updateControls();
  void showPlannedItems();
  [[nodiscard]] QString contextKey() const;

  int profile_index_{-1};
  QString target_id_;
  unsigned channel_{};
  quint32 tx_id_{};
  quint32 rx_id_{};
  QString vendor_;
  QString project_;
  QString target_name_;
  QString source_;
  VersionReadItems planned_items_;
  QString rendered_context_key_;
  bool configured_{};
  bool running_{};
  bool operation_busy_{};
  bool report_available_{};

  QLabel* selection_value_{};
  QLabel* address_value_{};
  QLabel* summary_{};
  QPushButton* check_button_{};
  QPushButton* stop_button_{};
  QPushButton* report_button_{};
  QTableWidget* table_{};
  QPlainTextEdit* raw_view_{};
};

} // namespace uds::ui::qt
