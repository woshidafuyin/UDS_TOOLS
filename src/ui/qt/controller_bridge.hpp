#pragma once

#include "app/flash_controller.hpp"
#include "app/operation_state.hpp"
#include "app/probe_controller.hpp"
#include "app/version_check_controller.hpp"
#include "core/profile.hpp"
#include "ui/qt/version_read_view_model.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace uds::ui::qt {

struct ControllerTargetOption {
  QString target_id;
  QString display_name;
  quint32 tx_id{};
  quint32 rx_id{};
  bool pending_validation{};
  QString driver_path;
  QString app_path;
  QString cal_path;
  QString driver_verify_path;
  QString app_verify_path;
  QString cal_verify_path;
  QString seed_key_dll_path;
  bool version_check_available{};
  VersionReadItems version_items;
};

struct ControllerProfileOption {
  QString profile_id;
  QString flow_id;
  QString vendor_name;
  QString project_name;
  QString device_name;
  bool placeholder{};
  bool power_control{};
  bool supports_ft_entry{};
  bool supports_cal_download{};
  bool lock_diagnostic_ids{};
  QString default_entry_mode;
  QString app_entry_label;
  QString ft_entry_label;
  unsigned channel{};
  quint32 tx_id{};
  quint32 rx_id{};
  quint32 functional_id{};
  unsigned nominal_bitrate{};
  unsigned data_bitrate{};
  bool can_fd{};
  quint8 padding{};
  QString driver_path;
  QString app_path;
  QString cal_path;
  QString driver_verify_path;
  QString app_verify_path;
  QString app_verify_label;
  QString cal_verify_path;
  QString seed_key_dll_path;
  std::vector<ControllerTargetOption> target_options;
  bool version_check_available{};
  VersionReadItems version_items;
};

// Owns the application-layer controllers and adapts their standard C++ worker
// callbacks to queued Qt signals. It never accesses QWidget objects.
class ControllerBridge final : public QObject {
  Q_OBJECT

public:
  explicit ControllerBridge(QObject* parent = nullptr);
  ControllerBridge(
      std::vector<FlashProfileRecord> profiles, app::ProbeService service,
      app::FlashController::WorkflowFactory workflow_factory = {},
      QObject* parent = nullptr);
  ~ControllerBridge() override;

  ControllerBridge(const ControllerBridge&) = delete;
  ControllerBridge& operator=(const ControllerBridge&) = delete;

  [[nodiscard]] const std::vector<ControllerProfileOption>& profileOptions() const;
  [[nodiscard]] const QStringList& startupMessages() const;

public slots:
  void startProbe(int profile_index, const QString& target_id,
                  const QString& entry_mode,
                  unsigned channel, quint32 tx_id, quint32 rx_id);
  void requestProbeStop();
  void startFlash(int profile_index, const QString& target_id,
                  const QString& entry_mode,
                  unsigned repeat_count, unsigned channel, quint32 tx_id,
                  quint32 rx_id,
                  const QString& driver_path, const QString& app_path,
                  const QString& cal_path,
                  const QString& driver_verify_path,
                  const QString& app_verify_path,
                  const QString& cal_verify_path,
                  const QString& seed_key_dll_path);
  bool requestFlashStop();
  void startVersionCheck(int profile_index, const QString& target_id,
                         unsigned channel, quint32 tx_id, quint32 rx_id);
  void requestVersionCheckStop();
  void setPower(int profile_index, bool enabled);

signals:
  void logMessage(const QString& message);
  void progressChanged(int percent, const QString& message);
  void probeRunningChanged(bool running);
  void probeFinished(bool success, bool cancelled, const QString& message);
  void flashRunningChanged(bool running);
  void flashFinished(bool success, bool cancelled, const QString& message,
                     const QString& report_path);
  void versionCheckRunningChanged(bool running);
  void versionCheckRow(const QString& status, const QString& request,
                       const QString& name, const QString& actual,
                       const QString& raw_response);
  void versionCheckFinished(bool success, bool cancelled,
                            const QString& message);
  void powerRunningChanged(bool running);
  void powerFinished(bool success, const QString& message);

private:
  [[nodiscard]] std::pair<quint32, quint32> resolveDiagnosticEndpoint(
      int profile_index, const QString& target_id, quint32 displayed_tx_id,
      quint32 displayed_rx_id) const;
  void loadProfiles();
  void buildProfileOptions();

  std::vector<FlashProfileRecord> profiles_;
  std::vector<ControllerProfileOption> profile_options_;
  QStringList startup_messages_;
  app::OperationState operation_state_;
  app::ProbeController probe_controller_;
  app::FlashController flash_controller_;
  app::VersionCheckController version_check_controller_;
  std::mutex power_mutex_;
  std::jthread power_worker_;
  std::atomic_bool power_running_{};
  std::atomic_bool shutting_down_{};
};

} // namespace uds::ui::qt
