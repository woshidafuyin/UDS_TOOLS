#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace uds::ui::qt {

class DiagnosticRequestPage final : public QWidget {
  Q_OBJECT
public:
  explicit DiagnosticRequestPage(QWidget* parent = nullptr);
  void setContext(int profile_index, const QString& target_id,
                  const QString& hardware, const QString& vendor,
                  const QString& project, const QString& target_name,
                  unsigned channel, unsigned nominal_bitrate,
                  unsigned data_bitrate, bool can_fd, quint32 tx_id,
                  quint32 rx_id, quint32 functional_id);
  void setRunning(bool running);
  void setOperationBusy(bool busy);
  void finish(bool success, bool cancelled, const QString& request,
              const QString& response, const QString& detail,
              unsigned elapsed_ms, quint8 nrc);

signals:
  void sendRequested(int profile_index, const QString& target_id,
                     unsigned channel, quint32 tx_id, quint32 rx_id,
                     const QString& payload, unsigned timeout_ms);
  void stopRequested();

private:
  void requestSend();
  void updateControls();
  [[nodiscard]] bool isStateChangingService(quint8 sid) const;

  int profile_index_{-1};
  QString target_id_;
  unsigned channel_{};
  quint32 physical_tx_id_{};
  quint32 physical_rx_id_{};
  quint32 functional_id_{};
  bool configured_{};
  bool running_{};
  bool operation_busy_{};
  QLabel* context_{};
  QComboBox* addressing_{};
  QLineEdit* payload_{};
  QSpinBox* timeout_{};
  QPushButton* send_{};
  QPushButton* stop_{};
  QLabel* summary_{};
  QPlainTextEdit* communication_{};
};

} // namespace uds::ui::qt
