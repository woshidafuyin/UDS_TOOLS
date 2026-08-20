#pragma once

#include "core/bus_monitor_trace.hpp"
#include "core/can_id_filter.hpp"
#include "core/can_bus.hpp"

#include <QWidget>

#include <atomic>
#include <deque>
#include <memory>
#include <optional>
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
    std::uint32_t can_id{};
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
  void setDiagnosticIds(std::vector<std::uint32_t> diagnostic_ids);
  void setDiagnosticAddressing(std::vector<std::uint32_t> physical_ids,
                               std::vector<std::uint32_t> functional_ids);
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
  void appendObservedFrames(std::vector<CanFrame> frames);
  void appendFrameToView(const CanFrame& frame);
  void appendFrame(Row row);
  void rebuildTable();
  void clearFrames();
  void exportAsc();
  void updateControls();
  void updateCounters();
  void updateTraceStatus();
  void resetViewForNewCapture();
  void applyIdFilterText(const QString& text);
  void setShortcutFilter(const std::vector<std::uint32_t>& included,
                         bool exclude);
  void updateFilterShortcuts();
  [[nodiscard]] bool matchesFilter(const Row& row) const;

  unsigned channel_{1};
  unsigned nominal_bitrate_{500000};
  unsigned data_bitrate_{2000000};
  bool can_fd_{true};
  bool running_{};
  bool operation_busy_{};
  std::atomic_bool stop_requested_{};
  std::jthread worker_;
  std::deque<Row> rows_;
  std::vector<std::uint32_t> diagnostic_ids_;
  std::vector<std::uint32_t> physical_ids_;
  std::vector<std::uint32_t> functional_ids_;
  std::optional<CanIdFilter> active_id_filter_;
  bool batch_appending_{};
  std::size_t total_frame_count_{};
  std::size_t evicted_frame_count_{};
  std::unique_ptr<BusMonitorTraceSession> trace_session_;
  BusMonitorTraceRecovery recovery_{};

  QLabel* context_label_{};
  QLabel* status_label_{};
  QLabel* trace_status_label_{};
  QLabel* total_count_label_{};
  QLabel* displayed_count_label_{};
  QLabel* evicted_count_label_{};
  QLineEdit* id_filter_{};
  QLabel* id_filter_error_{};
  QLineEdit* data_filter_{};
  QCheckBox* diagnostic_only_filter_{};
  QCheckBox* tx_filter_{};
  QCheckBox* rx_filter_{};
  QCheckBox* standard_filter_{};
  QCheckBox* extended_filter_{};
  QCheckBox* can_filter_{};
  QCheckBox* fd_filter_{};
  QCheckBox* brs_filter_{};
  QPushButton* project_diagnostic_filter_button_{};
  QPushButton* functional_filter_button_{};
  QPushButton* physical_filter_button_{};
  QPushButton* periodic_filter_button_{};
  QPushButton* clear_button_{};
  QPushButton* export_button_{};
  QTableWidget* table_{};
};

} // namespace uds::ui::qt
