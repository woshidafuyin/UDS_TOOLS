#pragma once

#include "core/can_bus.hpp"

#include <QWidget>

#include <atomic>
#include <thread>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace uds::ui::qt {

class BusMonitorPage final : public QWidget {
  Q_OBJECT

public:
  enum class DiagnosticTone {
    Normal,
    Pending,
    Failure,
  };

  struct Row {
    QString time;
    QString direction;
    QString id;
    QString type;
    QString length;
    QString data;
    QString diagnostic;
    DiagnosticTone diagnostic_tone{DiagnosticTone::Normal};
  };

  explicit BusMonitorPage(QWidget* parent = nullptr);
  ~BusMonitorPage() override;

  void setContext(unsigned channel, unsigned nominal_bitrate,
                  unsigned data_bitrate, bool can_fd);
  void setOperationBusy(bool busy);
  // Accepts one already-observed frame for classification and display.  The
  // passive receiver uses the same input boundary as tests and future bus
  // adapters, keeping NRC presentation independent of hardware access.
  void appendObservedFrame(const CanFrame& frame);
  void start();
  void restartForBackendChange();
  void stop();
  [[nodiscard]] bool matchesContext(unsigned channel, unsigned nominal_bitrate,
                                    unsigned data_bitrate, bool can_fd) const noexcept;
  [[nodiscard]] bool isRunning() const noexcept { return running_; }

signals:
  void runningChanged(bool running);
  void monitorMessage(const QString& message);

private:
  void startMonitoring();
  void stopMonitoring();
  void appendFrame(Row row);
  void clearFrames();
  void exportAsc();
  void updateControls();
  [[nodiscard]] bool matchesFilter(const Row& row) const;

  unsigned channel_{1};
  unsigned nominal_bitrate_{500000};
  unsigned data_bitrate_{2000000};
  bool can_fd_{true};
  bool running_{};
  bool operation_busy_{};
  std::atomic_bool stop_requested_{};
  std::jthread worker_;
  std::vector<Row> rows_;

  QLabel* context_label_{};
  QLabel* status_label_{};
  QLabel* count_label_{};
  QLineEdit* id_filter_{};
  QLineEdit* data_filter_{};
  QCheckBox* tx_filter_{};
  QCheckBox* rx_filter_{};
  QCheckBox* standard_filter_{};
  QCheckBox* extended_filter_{};
  QCheckBox* can_filter_{};
  QCheckBox* fd_filter_{};
  QCheckBox* brs_filter_{};
  QPushButton* clear_button_{};
  QPushButton* export_button_{};
  QTableWidget* table_{};
};

} // namespace uds::ui::qt
