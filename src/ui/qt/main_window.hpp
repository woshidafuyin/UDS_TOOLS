#pragma once

#include <QHash>
#include <QList>
#include <QMainWindow>

#include "ui/qt/ui_log_message_parser.hpp"

#include <memory>

class QCloseEvent;
class QEvent;
class QFile;
class QActionGroup;
class QLineEdit;
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
                      bool update_public_key, unsigned repeat_count,
                      unsigned channel, quint32 tx_id, quint32 rx_id,
                      const QString& driver_path, const QString& app_path,
                      const QString& cal_path,
                      const QString& driver_verify_path,
                      const QString& app_verify_path,
                      const QString& cal_verify_path,
                      const QString& seed_key_dll_path);
protected:
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  enum class FlashFileField {
    Driver,
    DriverVerify,
    App,
    AppVerify,
    Cal,
    CalVerify,
    SeedKeyDll,
  };

  enum class UiLogTone {
    Normal,
    Success,
    Pending,
    Failure,
  };

  enum class UiLogDestination {
    ViewAndFile,
    ViewOnly,
    FileOnly,
  };

  struct UiLogEntry {
    QString timestamp;
    QString message;
    UiLogTone tone{UiLogTone::Normal};
    ParsedUiLogMessage parsed;
    QString local_file_link;
  };

  struct RuntimeFileSelection {
    QString driver_path;
    QString app_path;
    QString cal_path;
    QString driver_verify_path;
    QString app_verify_path;
    QString cal_verify_path;
    QString seed_key_dll_path;
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
  void saveActiveProfileState() const;
  void restoreCurrentProfileState();
  void saveRuntimeFileSelection();
  void restoreRuntimeFileSelection();
  [[nodiscard]] QString configuredDefaultFlashFile(FlashFileField field) const;
  bool storeSelectedFlashFile(FlashFileField field, const QString& selected,
                              QLineEdit* editor, const QString& log_name);
  void restoreDefaultFlashFile(FlashFileField field);
  void restoreDefaultDiagnosticId(bool restore_tx);
  void updateAppPackagePresentation(bool report_error = false);
  [[nodiscard]] bool selectedProfileSupportsAppTmpPackage() const;
  [[nodiscard]] int selectedProfileIndex(bool* valid = nullptr) const;
  [[nodiscard]] bool hasRadarSelector() const;
  [[nodiscard]] QString selectedTargetId() const;
  [[nodiscard]] unsigned currentProfileDefaultChannel() const;
  void saveCurrentBackendChannel() const;
  void restoreCurrentBackendChannel(unsigned profile_default_channel);
  void saveComboSelections() const;
  void startProbeFromUi();
  void startFlashFromUi();
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
                   UiLogTone tone = UiLogTone::Normal,
                   UiLogDestination destination =
                       UiLogDestination::ViewAndFile,
                   const QString& local_file_link = {});
  void appendProbeLogMessage(const QString& message);
  void appendFlashLogMessage(const QString& message);
  void flushPendingFlashPreparationSummary();
  Q_INVOKABLE void beginFlashUiLog();
  void appendUiLogEntryToView(const UiLogEntry& entry);
  void scheduleExecutionLogTailFollow();
  void renderActiveUiLog();
  Q_INVOKABLE void handleFlashFinished(bool success, bool cancelled,
                                       const QString& message,
                                       const QString& report_path);
  Q_INVOKABLE void handleProbeFinished(bool success, bool cancelled,
                                       const QString& message);
  Q_INVOKABLE void handleProgressChanged(int percent, const QString& message);
  Q_INVOKABLE void handleVersionCheckRunningChanged(bool running);
  Q_INVOKABLE void handleVersionCheckFinished(bool success, bool cancelled,
                                              const QString& message);
  [[nodiscard]] QString selectedLogTargetKey() const;
  void activateSelectedLogTarget();
  void clearActiveUiLog();

  std::unique_ptr<Ui::MainWindow> ui_;
  std::unique_ptr<ControllerBridge> controller_bridge_;
  bool probe_running_{};
  bool probe_ui_log_active_{};
  bool probe_refresh_entry_checked_{};
  bool flash_running_{};
  bool flash_ui_log_active_{};
  bool flash_preparation_ui_active_{};
  bool flash_stop_requested_{};
  bool version_check_running_{};
  bool diagnostic_request_running_{};
  bool bus_monitor_running_{};
  bool restoring_combo_selections_{};
  int flash_progress_{};
  QString latest_report_path_;
  std::unique_ptr<QFile> execution_log_file_;
  QActionGroup* can_backend_group_{};
  QHash<QString, QList<UiLogEntry>> target_log_entries_;
  QHash<QString, RuntimeFileSelection> runtime_file_selections_;
  QString active_profile_state_key_;
  QString active_file_selection_key_;
  QString active_log_target_key_;
  QString probe_can_open_summary_;
  QString pending_flash_trace_summary_;
  QString pending_flash_cycle_summary_;
  bool execution_log_follow_tail_{true};
  VersionConfirmationPage* version_page_{};
  BusMonitorPage* bus_monitor_page_{};
  DiagnosticRequestPage* diagnostic_request_page_{};
};

} // namespace uds::ui::qt
