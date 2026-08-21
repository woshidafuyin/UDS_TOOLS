#pragma once

#include <QHash>
#include <QList>
#include <QMainWindow>

#include <memory>

class QCloseEvent;
class QEvent;
class QFile;
class QActionGroup;
class QString;

namespace Ui {
class MainWindow;
}

namespace uds::ui::qt {

class ControllerBridge;
class BusMonitorPage;
class VersionConfirmationPage;
class DiagnosticRequestPage;
struct ControllerProfileOption;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  void startDefaultBusMonitoring();

  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

signals:
  void probeRequested(int profile_index, const QString& target_id,
                      const QString& entry_mode,
                      unsigned channel, quint32 tx_id, quint32 rx_id);
  void flashRequested(int profile_index, const QString& target_id,
                      const QString& entry_mode,
                      unsigned repeat_count, unsigned channel, quint32 tx_id,
                      quint32 rx_id,
                      const QString& driver_path, const QString& app_path,
                      const QString& cal_path,
                      const QString& driver_verify_path,
                      const QString& app_verify_path,
                      const QString& cal_verify_path,
                      const QString& seed_key_dll_path);
  void powerRequested(int profile_index, bool enabled);

protected:
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  enum class UiLogTone {
    Normal,
    Success,
    Pending,
    Failure,
  };

  struct UiLogEntry {
    QString text;
    UiLogTone tone{UiLogTone::Normal};
  };

  void configureVisualDesign();
  void installWheelMutationGuards();
  void connectActions();
  void connectControllerActions();
  void populateProfileOptions();
  void populateDeviceOptions(int project_index);
  void populateTargetOptions(int device_index);
  void applySelectedProfile(int device_index);
  void applySelectedRadar(bool log_change);
  [[nodiscard]] int selectedProfileIndex(bool* valid = nullptr) const;
  [[nodiscard]] bool hasRadarSelector() const;
  [[nodiscard]] QString selectedTargetId() const;
  [[nodiscard]] unsigned currentProfileDefaultChannel() const;
  void saveCurrentBackendChannel() const;
  void restoreCurrentBackendChannel(unsigned profile_default_channel);
  void saveComboSelections() const;
  void startProbeFromUi();
  void startFlashFromUi();
  void requestPowerFromUi(bool enabled);
  void syncVersionContext(bool recent_flash = false);
  void syncBusMonitorContext();
  void syncDiagnosticRequestContext();
  void followSelectedBusMonitorContext();
  [[nodiscard]] bool monitorMatchesSelectedHardware(int profile_index) const;
  void updateStatusBar();
  void updateEnabledState();
  void initializeExecutionLog();
  void refreshLatestReportPath();
  void appendUiLog(const QString& message,
                   UiLogTone tone = UiLogTone::Normal);
  void appendUiLogEntryToView(const UiLogEntry& entry);
  void scheduleExecutionLogTailFollow();
  void renderActiveUiLog();
  Q_INVOKABLE void handleFlashFinished(bool success, bool cancelled,
                                       const QString& message,
                                       const QString& report_path);
  Q_INVOKABLE void handleProbeFinished(bool success, bool cancelled,
                                       const QString& message);
  [[nodiscard]] QString selectedLogTargetKey() const;
  void activateSelectedLogTarget();
  void clearActiveUiLog();

  std::unique_ptr<Ui::MainWindow> ui_;
  std::unique_ptr<ControllerBridge> controller_bridge_;
  bool probe_running_{};
  bool flash_running_{};
  bool flash_stop_requested_{};
  bool power_running_{};
  bool version_check_running_{};
  bool diagnostic_request_running_{};
  bool bus_monitor_running_{};
  bool restoring_combo_selections_{};
  int flash_progress_{};
  QString latest_report_path_;
  std::unique_ptr<QFile> execution_log_file_;
  QActionGroup* can_backend_group_{};
  QHash<QString, QList<UiLogEntry>> target_log_entries_;
  QString active_log_target_key_;
  bool execution_log_follow_tail_{true};
  VersionConfirmationPage* version_page_{};
  BusMonitorPage* bus_monitor_page_{};
  DiagnosticRequestPage* diagnostic_request_page_{};
};

} // namespace uds::ui::qt
