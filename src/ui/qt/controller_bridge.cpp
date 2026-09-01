#include "ui/qt/controller_bridge.hpp"

#include "core/asc_trace.hpp"
#include "core/version_check_plan.hpp"
#include "core/hex.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QStringList>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string_view>
#include <tuple>
#include <utility>

namespace uds::ui::qt {
namespace {

QString fromWide(std::wstring_view text) {
  return QString::fromWCharArray(text.data(), static_cast<int>(text.size()));
}

QString fromUtf8(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

std::string toUtf8(const QString& text) {
  const auto encoded = text.toUtf8();
  return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QString probeAuditKey(int profile_index, const QString& target_id) {
  return QStringLiteral("%1|%2").arg(profile_index).arg(target_id);
}

QString requestText(const std::vector<std::uint8_t>& request) {
  QStringList bytes;
  bytes.reserve(static_cast<int>(request.size()));
  for (const auto byte : request) {
    bytes.push_back(QStringLiteral("%1").arg(byte, 2, 16,
                                             QLatin1Char('0')).toUpper());
  }
  return bytes.join(QLatin1Char(' '));
}

QString didText(const std::vector<std::uint8_t>& request) {
  if (request.size() >= 3U && request[0] == 0x22U) {
    return QStringLiteral("%1%2")
        .arg(request[1], 2, 16, QLatin1Char('0'))
        .arg(request[2], 2, 16, QLatin1Char('0'))
        .toUpper();
  }
  return QStringLiteral("-");
}

VersionReadItems versionItemViews(const VersionCheckPlan& plan) {
  VersionReadItems items;
  items.reserve(plan.items.size());
  for (const auto& item : plan.items) {
    items.push_back({didText(item.request), requestText(item.request),
                     fromWide(item.name), item.required});
  }
  return items;
}

QString pathText(const std::filesystem::path& path) {
  if (path.empty()) return {};
  const auto resolved = path.is_absolute()
                            ? path
                            : std::filesystem::path(
                                  QCoreApplication::applicationDirPath()
                                      .toStdWString()) /
                                  path;
  return QString::fromStdWString(resolved.lexically_normal().wstring());
}

QString targetPathText(const std::filesystem::path& target_path,
                       const std::filesystem::path& profile_path) {
  return pathText(target_path.empty() ? profile_path : target_path);
}

std::filesystem::path toPath(const QString& text) {
  if (text.trimmed().isEmpty()) return {};
  return std::filesystem::path(text.toStdWString()).lexically_normal();
}

struct SelectorLabels {
  QString vendor;
  QString project;
  QString device;
};

SelectorLabels selectorLabels(const FlashProfile& profile) {
  const auto name = fromWide(profile.name);
  const auto vendor = fromWide(profile.vendor_name);
  const auto project = fromWide(profile.project_name);
  const auto device = fromWide(profile.device_name);
  if (!vendor.isEmpty()) {
    return {vendor, project.isEmpty() ? name : project,
            device.isEmpty() ? name : device};
  }
  // Compatibility with profiles created before the three-level selector:
  // project_name was the vendor and device_name was the project/device.
  const auto legacy_vendor = project.isEmpty() ? name : project;
  const auto legacy_project = device.isEmpty() ? name : device;
  return {legacy_vendor, legacy_project, legacy_project};
}

QString versionStatus(app::VersionCheckStatus status) {
  switch (status) {
  case app::VersionCheckStatus::pass:
    return QStringLiteral("成功");
  case app::VersionCheckStatus::fail:
    return QStringLiteral("不一致");
  case app::VersionCheckStatus::warning:
    return QStringLiteral("警告");
  case app::VersionCheckStatus::info:
    return QStringLiteral("信息");
  case app::VersionCheckStatus::error:
  default:
    return QStringLiteral("错误");
  }
}

} // namespace

ControllerBridge::ControllerBridge(QObject* parent)
    : QObject(parent), probe_controller_(operation_state_),
      flash_controller_(operation_state_),
      version_check_controller_(operation_state_),
      diagnostic_request_controller_(operation_state_) {
  loadProfiles();
}

ControllerBridge::ControllerBridge(
    std::vector<FlashProfileRecord> profiles, app::ProbeService service,
    app::FlashController::WorkflowFactory workflow_factory, QObject* parent)
    : QObject(parent), profiles_(std::move(profiles)),
      probe_controller_(operation_state_, std::move(service)),
      flash_controller_(operation_state_, std::move(workflow_factory)),
      version_check_controller_(operation_state_),
      diagnostic_request_controller_(operation_state_) {
  buildProfileOptions();
}

ControllerBridge::~ControllerBridge() {
  shutting_down_.store(true);
  flash_controller_.request_stop();
  probe_controller_.request_stop();
  version_check_controller_.request_stop();
  diagnostic_request_controller_.request_stop();
  flash_controller_.wait();
  probe_controller_.wait();
  version_check_controller_.wait();
  diagnostic_request_controller_.wait();
}

const std::vector<ControllerProfileOption>&
ControllerBridge::profileOptions() const {
  return profile_options_;
}

const QStringList& ControllerBridge::startupMessages() const {
  return startup_messages_;
}

void ControllerBridge::startProbe(int profile_index, const QString& target_id,
                                  const QString& entry_mode,
                                  unsigned channel, quint32 tx_id,
                                  quint32 rx_id) {
  if (shutting_down_.load()) return;
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles_.size()) {
    emit probeFinished(false, false,
                       QStringLiteral("在线探测配置无效：未选择设备。"));
    return;
  }

  const auto& profile =
      profiles_[static_cast<std::size_t>(profile_index)].profile;
  if (profile.placeholder) {
    emit probeFinished(false, false,
                       QStringLiteral("该设备资料尚未补充，不能执行在线探测。"));
    return;
  }
  try {
    std::tie(tx_id, rx_id) =
        resolveDiagnosticEndpoint(profile_index, target_id, tx_id, rx_id);
  } catch (const std::exception& error) {
    emit probeFinished(false, false,
                       QStringLiteral("在线探测目标无效：%1")
                           .arg(fromUtf8(error.what())));
    return;
  }

  app::ProbeRequest request;
  request.profile = profile;
  const auto selected_target = std::find_if(
      profile.targets.cbegin(), profile.targets.cend(),
      [&target_id](const FlashTargetProfile& candidate) {
        return fromWide(candidate.id) == target_id;
      });
  if (selected_target != profile.targets.cend() &&
      selected_target->ft_tx_id != 0 && selected_target->ft_rx_id != 0) {
    request.profile.ft_tx_id = selected_target->ft_tx_id;
    request.profile.ft_rx_id = selected_target->ft_rx_id;
  }
  // CAL and APP+CAL use the normal APP diagnostic endpoint. Probing is an
  // entry concern only and must not know which image set will later download.
  request.entry_mode = entry_mode == QStringLiteral("ft")
                           ? L"ft"
                           : (entry_mode == QStringLiteral("boot") ? L"boot"
                                                                    : L"app");
  request.channel = channel;
  request.tx_id = tx_id;
  request.rx_id = rx_id;
  request.nominal_bitrate = profile.nominal_bitrate;
  request.data_bitrate = profile.data_bitrate;
  request.padding = profile.padding;
  request.trace_file = make_asc_trace_path(
      QCoreApplication::applicationDirPath().toStdWString(), profile.id,
      target_id.toStdWString(), L"probe");

  const auto operation_id = std::make_shared<app::OperationId>();
  app::ProbeControllerCallbacks callbacks;
  callbacks.onLog = [this, operation_id](const std::string& line) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(line);
    QMetaObject::invokeMethod(
        this,
        [this, message, id = *operation_id] {
          if (!shutting_down_.load() && operation_state_.is_latest(id))
            emit logMessage(message);
        },
        Qt::QueuedConnection);
  };
  callbacks.onProgress = [this, operation_id](int percent, const std::string& line) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(line);
    QMetaObject::invokeMethod(
        this,
        [this, percent, message, id = *operation_id] {
          if (!shutting_down_.load() && operation_state_.is_latest(id))
            emit progressChanged(percent, message);
        },
        Qt::QueuedConnection);
  };
  const auto audit_key = probeAuditKey(profile_index, target_id);
  const auto audit_backend =
      fromUtf8(can_vendor_name(default_can_vendor()));
  const auto audit_entry_mode = fromWide(request.entry_mode);
  const auto audit_can_fd = profile.can_fd;
  callbacks.onFinished =
      [this, audit_key, audit_backend, audit_entry_mode, channel, tx_id, rx_id,
       nominal_bitrate = request.nominal_bitrate,
       data_bitrate = request.data_bitrate,
       audit_can_fd, operation_id](app::ProbeResult result) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(result.message);
    QMetaObject::invokeMethod(
        this,
        [this, success = result.success, cancelled = result.cancelled,
         message, audit_key, audit_backend, audit_entry_mode, channel, tx_id,
         rx_id, nominal_bitrate, data_bitrate, audit_can_fd,
         id = *operation_id] {
          if (shutting_down_.load() || !operation_state_.is_latest(id)) return;
          probe_audit_records_.insert(
              audit_key,
              {audit_backend, audit_entry_mode,
               QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
               message, channel, tx_id, rx_id, nominal_bitrate, data_bitrate,
               audit_can_fd, success, cancelled});
          emit probeRunningChanged(false);
          emit probeFinished(success, cancelled, message);
        },
        Qt::QueuedConnection);
  };

  try {
    if (!probe_controller_.start(std::move(request), std::move(callbacks),
                                 operation_id.get())) {
      emit probeFinished(false, false,
                         QStringLiteral("已有操作正在运行，不能启动在线探测。"));
      return;
    }
    emit probeRunningChanged(true);
  } catch (const std::exception& error) {
    emit probeRunningChanged(false);
    emit probeFinished(
        false, false,
        QStringLiteral("无法启动在线探测线程：%1").arg(fromUtf8(error.what())));
  } catch (...) {
    emit probeRunningChanged(false);
    emit probeFinished(false, false,
                       QStringLiteral("无法启动在线探测线程：unknown exception"));
  }
}

void ControllerBridge::requestProbeStop() {
  probe_controller_.request_stop();
}

void ControllerBridge::startFlash(
    int profile_index, const QString& target_id, const QString& entry_mode,
    bool update_public_key, unsigned repeat_count, unsigned channel,
    quint32 tx_id, quint32 rx_id,
    const QString& driver_path,
    const QString& app_path, const QString& cal_path,
    const QString& driver_verify_path, const QString& app_verify_path,
    const QString& cal_verify_path, const QString& seed_key_dll_path) {
  if (shutting_down_.load()) return;
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles_.size()) {
    emit flashFinished(false, false,
                       QStringLiteral("刷写配置无效：未选择设备。"), {});
    return;
  }

  const auto& profile =
      profiles_[static_cast<std::size_t>(profile_index)].profile;
  if (profile.placeholder) {
    emit flashFinished(false, false,
                       QStringLiteral("该设备资料尚未补充，不能执行刷写。"), {});
    return;
  }
  try {
    std::tie(tx_id, rx_id) =
        resolveDiagnosticEndpoint(profile_index, target_id, tx_id, rx_id);
  } catch (const std::exception& error) {
    emit flashFinished(false, false,
                       QStringLiteral("刷写目标无效：%1")
                           .arg(fromUtf8(error.what())),
                       {});
    return;
  }

  app::FlashRequest request;
  request.profile = profile;
  request.target_id = target_id.toStdWString();
  request.entry_mode = entry_mode.toStdWString();
  request.update_public_key = update_public_key;
  request.repeat_count = repeat_count;
  request.executable_directory =
      QCoreApplication::applicationDirPath().toStdWString();
  const auto backend = fromUtf8(can_vendor_name(default_can_vendor()));
  request.hardware_backend = toUtf8(backend);
  auto target_display_name = fromWide(profile.device_name);
  if (!target_id.isEmpty()) {
    const auto selected_target = std::find_if(
        profile.targets.cbegin(), profile.targets.cend(),
        [&](const FlashTargetProfile& target) {
          return target.id == target_id.toStdWString();
        });
    if (selected_target != profile.targets.cend() &&
        !selected_target->display_name.empty()) {
      target_display_name = fromWide(selected_target->display_name);
    }
  }
  request.target_description = toUtf8(
      QStringLiteral("%1 / %2 / %3; Profile=%4; Target=%5; Flow=%6; Entry=%7; "
                     "Update_PublicKey=%8; Repetitions=%9")
          .arg(fromWide(profile.vendor_name), fromWide(profile.project_name),
               target_display_name, fromWide(profile.id), target_id,
               fromWide(profile.flow), entry_mode.toUpper(),
               update_public_key ? QStringLiteral("ON")
                                 : QStringLiteral("OFF"))
          .arg(repeat_count));
  request.channel = channel;
  request.tx_id = tx_id;
  request.rx_id = rx_id;
  request.functional_id = profile.functional_id;
  request.nominal_bitrate = profile.nominal_bitrate;
  request.data_bitrate = profile.data_bitrate;
  request.padding = profile.padding;
  const auto audit = probe_audit_records_.constFind(
      probeAuditKey(profile_index, target_id));
  if (audit != probe_audit_records_.cend()) {
    const auto normalized_entry =
        entry_mode == QStringLiteral("ft")
            ? QStringLiteral("ft")
            : (entry_mode == QStringLiteral("boot") ? QStringLiteral("boot")
                                                      : QStringLiteral("app"));
    const auto matches =
        audit->backend == backend && audit->entry_mode == normalized_entry &&
        audit->channel == channel && audit->tx_id == tx_id &&
        audit->rx_id == rx_id &&
        audit->nominal_bitrate == profile.nominal_bitrate &&
        audit->data_bitrate == profile.data_bitrate &&
        audit->can_fd == profile.can_fd;
    if (matches) {
      request.qualification_status =
          audit->cancelled ? "CANCELLED" : (audit->success ? "PASS" : "FAIL");
      request.qualification_detail = toUtf8(audit->message);
    } else {
      request.qualification_status = "STALE_CONFIG";
      request.qualification_detail =
          "The latest pre-flash probe used a different backend, channel, "
          "diagnostic endpoint, bitrate, frame format, or entry mode; last "
          "result: " +
          toUtf8(audit->message);
    }
    request.qualification_completed_at = toUtf8(audit->completed_at);
  }
  request.driver_file = toPath(driver_path);
  request.app_file = toPath(app_path);
  request.cal_file = toPath(cal_path);
  request.driver_verify_file = toPath(driver_verify_path);
  request.app_verify_file = toPath(app_verify_path);
  request.cal_verify_file = toPath(cal_verify_path);
  request.security_dll = toPath(seed_key_dll_path);
  request.trace_file = make_asc_trace_path(
      request.executable_directory, profile.id, request.target_id,
      request.entry_mode);

  const auto operation_id = std::make_shared<app::OperationId>();
  app::OperationCallbacks callbacks;
  callbacks.onLog = [this, operation_id](const std::string& line) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(line);
    QMetaObject::invokeMethod(
        this,
        [this, message, id = *operation_id] {
          if (!shutting_down_.load() && operation_state_.is_latest(id))
            emit logMessage(message);
        },
        Qt::QueuedConnection);
  };
  callbacks.onProgress = [this, operation_id](int percent, const std::string& line) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(line);
    QMetaObject::invokeMethod(
        this,
        [this, percent, message, id = *operation_id] {
          if (!shutting_down_.load() && operation_state_.is_latest(id))
            emit progressChanged(percent, message);
        },
        Qt::QueuedConnection);
  };
  callbacks.onFinished = [this, operation_id](app::OperationResult result) {
    if (shutting_down_.load()) return;
    const auto message = fromWide(result.message);
    const auto report_path =
        QString::fromStdWString(result.report_path.wstring());
    QMetaObject::invokeMethod(
        this,
        [this, success = result.success, cancelled = result.cancelled, message,
         report_path, id = *operation_id] {
          if (shutting_down_.load() || !operation_state_.is_latest(id)) return;
          emit flashRunningChanged(false);
          emit flashFinished(success, cancelled, message, report_path);
        },
        Qt::QueuedConnection);
  };

  try {
    if (!flash_controller_.start(std::move(request), std::move(callbacks),
                                 operation_id.get())) {
      emit flashFinished(false, false,
                         QStringLiteral("已有操作正在运行，不能启动刷写。"), {});
      return;
    }
    emit flashRunningChanged(true);
  } catch (const std::exception& error) {
    emit flashRunningChanged(false);
    emit flashFinished(
        false, false,
        QStringLiteral("无法启动刷写线程：%1").arg(fromUtf8(error.what())), {});
  } catch (...) {
    emit flashRunningChanged(false);
    emit flashFinished(false, false,
                       QStringLiteral("无法启动刷写线程：unknown exception"), {});
  }
}

bool ControllerBridge::requestFlashStop() {
  return flash_controller_.request_stop();
}

void ControllerBridge::startVersionCheck(int profile_index,
                                         const QString& target_id,
                                         unsigned channel, quint32 tx_id,
                                         quint32 rx_id) {
  if (shutting_down_.load()) return;
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles_.size()) {
    emit versionCheckFinished(false, false,
                              QStringLiteral("版本读取配置无效：未选择设备。"));
    return;
  }
  const auto& record = profiles_[static_cast<std::size_t>(profile_index)];
  try {
    std::tie(tx_id, rx_id) =
        resolveDiagnosticEndpoint(profile_index, target_id, tx_id, rx_id);
    if (load_version_check_plan(record.source, target_id.toStdWString())
            .items.empty()) {
      emit versionCheckFinished(
          false, false, QStringLiteral("当前Profile未配置版本读取项目。"));
      return;
    }
  } catch (const std::exception& error) {
    emit versionCheckFinished(
        false, false,
        QStringLiteral("版本读取目标无效：%1").arg(fromUtf8(error.what())));
    return;
  }

  app::VersionCheckRequest request;
  request.profile = record.profile;
  request.profile_path = record.source;
  request.target_id = target_id.toStdWString();
  request.channel = channel;
  request.tx_id = tx_id;
  request.rx_id = rx_id;
  request.trace_file = make_asc_trace_path(
      QCoreApplication::applicationDirPath().toStdWString(), record.profile.id,
      request.target_id, L"version");

  const auto operation_id = std::make_shared<app::OperationId>();
  app::VersionCheckControllerCallbacks callbacks;
  callbacks.onLog = [this, operation_id](const std::string& line) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(line);
    QMetaObject::invokeMethod(
        this,
        [this, message, id = *operation_id] {
          if (!shutting_down_.load() && operation_state_.is_latest(id))
            emit logMessage(message);
        },
        Qt::QueuedConnection);
  };
  callbacks.onProgress = [this, operation_id](int percent, const std::string& line) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(line);
    QMetaObject::invokeMethod(
        this,
        [this, percent, message, id = *operation_id] {
          if (!shutting_down_.load() && operation_state_.is_latest(id))
            emit progressChanged(percent, message);
        },
        Qt::QueuedConnection);
  };
  callbacks.onFinished = [this, operation_id](app::VersionCheckResult result) {
    if (shutting_down_.load()) return;
    const auto message = fromUtf8(result.message);
    QMetaObject::invokeMethod(
        this,
        [this, result = std::move(result), message, id = *operation_id] {
          if (shutting_down_.load() || !operation_state_.is_latest(id)) return;
          for (const auto& item : result.items) {
            emit versionCheckRow(
                versionStatus(item.status), fromUtf8(item.request_hex),
                fromWide(item.name), fromWide(item.actual),
                fromUtf8(item.response_hex));
          }
          emit versionCheckRunningChanged(false);
          emit versionCheckFinished(result.success, result.cancelled, message);
        },
        Qt::QueuedConnection);
  };

  try {
    if (!version_check_controller_.start(std::move(request),
                                         std::move(callbacks),
                                         operation_id.get())) {
      emit versionCheckFinished(
          false, false,
          QStringLiteral("已有操作正在运行，不能启动版本读取。"));
      return;
    }
    emit versionCheckRunningChanged(true);
  } catch (const std::exception& error) {
    emit versionCheckRunningChanged(false);
    emit versionCheckFinished(
        false, false,
        QStringLiteral("无法启动版本读取线程：%1")
            .arg(fromUtf8(error.what())));
  }
}

void ControllerBridge::requestVersionCheckStop() {
  version_check_controller_.request_stop();
}

void ControllerBridge::startDiagnosticRequest(
    int profile_index, const QString& target_id, unsigned channel,
    quint32 tx_id, quint32 rx_id, const QString& payload,
    unsigned timeout_ms) {
  if (shutting_down_.load()) return;
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profiles_.size()) {
    emit diagnosticFinished(false, false, payload, {},
                            QStringLiteral("诊断配置无效：未选择设备。"), 0, 0);
    return;
  }
  try {
    const auto request_bytes = from_hex(payload.toStdString());
    if (request_bytes.empty()) throw std::runtime_error("UDS request is empty");
    app::DiagnosticRequest request;
    request.profile = profiles_[static_cast<std::size_t>(profile_index)].profile;
    request.channel = channel;
    request.tx_id = tx_id;
    request.rx_id = rx_id;
    request.payload = request_bytes;
    request.timeout_ms = timeout_ms;
    request.trace_file = make_asc_trace_path(
        QCoreApplication::applicationDirPath().toStdWString(),
        request.profile.id, target_id.toStdWString(), L"diagnostic");
    const auto operation_id = std::make_shared<app::OperationId>();
    if (!diagnostic_request_controller_.start(
            std::move(request), [this, operation_id](app::DiagnosticRequestResult result) {
              if (shutting_down_.load()) return;
              QMetaObject::invokeMethod(
                  this,
                  [this, result = std::move(result), id = *operation_id] {
                    if (shutting_down_.load() ||
                        !operation_state_.is_latest(id))
                      return;
                    emit diagnosticRunningChanged(false);
                    emit diagnosticFinished(
                        result.success, result.cancelled,
                        fromUtf8(result.request_hex), fromUtf8(result.response_hex),
                        fromUtf8(result.message), result.elapsed_ms, result.nrc);
                  },
                  Qt::QueuedConnection);
            }, operation_id.get())) {
      emit diagnosticFinished(false, false, payload, {},
                              QStringLiteral("已有操作正在运行，不能发送诊断请求。"), 0, 0);
      return;
    }
    emit diagnosticRunningChanged(true);
  } catch (const std::exception& error) {
    emit diagnosticFinished(false, false, payload, {}, fromUtf8(error.what()),
                            0, 0);
  }
}

void ControllerBridge::requestDiagnosticStop() {
  diagnostic_request_controller_.request_stop();
}

void ControllerBridge::loadProfiles() {
  const auto directory =
      std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
      L"profiles";
  const auto catalog = discover_flash_profiles(directory);
  profiles_ = catalog.profiles;
  for (const auto& error : catalog.errors) {
    startup_messages_.push_back(
        QStringLiteral("Profile加载失败：%1；%2")
            .arg(QString::fromStdWString(error.source.wstring()),
                 fromUtf8(error.message)));
  }
  if (profiles_.empty()) {
    startup_messages_.push_back(
        QStringLiteral("未发现Profile：%1").arg(
            QString::fromStdWString(directory.wstring())));
  }
  buildProfileOptions();
}

std::pair<quint32, quint32> ControllerBridge::resolveDiagnosticEndpoint(
    int profile_index, const QString& target_id, quint32 displayed_tx_id,
    quint32 displayed_rx_id) const {
  if (profile_index < 0 ||
      static_cast<std::size_t>(profile_index) >= profile_options_.size()) {
    throw std::runtime_error("profile index is out of range");
  }
  const auto& profile = profile_options_[static_cast<std::size_t>(profile_index)];
  if (profile.target_options.empty()) {
    return {displayed_tx_id, displayed_rx_id};
  }

  const auto target = std::find_if(
      profile.target_options.cbegin(), profile.target_options.cend(),
      [&target_id](const ControllerTargetOption& option) {
        return option.target_id == target_id;
      });
  if (target == profile.target_options.cend()) {
    throw std::runtime_error("target role is missing or unsupported");
  }
  return {displayed_tx_id, displayed_rx_id};
}

void ControllerBridge::buildProfileOptions() {
  profile_options_.clear();
  profile_options_.reserve(profiles_.size());
  for (const auto& record : profiles_) {
    const auto labels = selectorLabels(record.profile);
    auto device_name = labels.device;
    if (record.profile.placeholder) {
      device_name += QStringLiteral(" [资料待补充]");
    }
    profile_options_.push_back(
        {fromWide(record.profile.id),
         fromWide(record.profile.flow),
         labels.vendor,
         labels.project,
         std::move(device_name),
         record.profile.placeholder,
          record.profile.power_control,
          record.profile.supports_ft_entry,
           record.profile.supports_cal_download,
           record.profile.lock_diagnostic_ids,
           fromWide(record.profile.default_entry_mode),
           fromWide(record.profile.app_entry_label),
           fromWide(record.profile.ft_entry_label),
          record.profile.channel,
         record.profile.tx_id,
         record.profile.rx_id,
         record.profile.functional_id,
         record.profile.nominal_bitrate,
         record.profile.data_bitrate,
         record.profile.can_fd,
         record.profile.padding,
         pathText(record.profile.driver_file),
         pathText(record.profile.app_file),
         pathText(record.profile.cal_file),
          pathText(record.profile.driver_verify_file),
          pathText(record.profile.app_verify_file),
          fromWide(record.profile.app_verify_label),
          pathText(record.profile.cal_verify_file),
         pathText(record.profile.security_dll)});
    auto& option = profile_options_.back();
    option.ft_tx_id = record.profile.ft_tx_id;
    option.ft_rx_id = record.profile.ft_rx_id;
    option.supports_app_tmp_package =
        record.profile.supports_app_tmp_package;
    try {
      const auto plan = load_version_check_plan(record.source, {});
      option.version_check_available = !plan.items.empty();
      option.version_items = versionItemViews(plan);
    } catch (const std::exception& error) {
      startup_messages_.push_back(
          QStringLiteral("版本读取配置失败：%1；%2")
              .arg(QString::fromStdWString(record.source.wstring()),
                   fromUtf8(error.what())));
    }
    option.target_options.reserve(record.profile.targets.size());
    for (const auto& target : record.profile.targets) {
      option.target_options.push_back(
          {fromWide(target.id),
           fromWide(target.display_name),
           target.tx_id,
           target.rx_id,
           target.pending_validation,
           targetPathText(target.driver_file, record.profile.driver_file),
           targetPathText(target.app_file, record.profile.app_file),
           targetPathText(target.cal_file, record.profile.cal_file),
           targetPathText(target.driver_verify_file,
                          record.profile.driver_verify_file),
           targetPathText(target.app_verify_file,
                          record.profile.app_verify_file),
           targetPathText(target.cal_verify_file,
                          record.profile.cal_verify_file),
            targetPathText(target.security_dll, record.profile.security_dll)});
      option.target_options.back().ft_tx_id = target.ft_tx_id;
      option.target_options.back().ft_rx_id = target.ft_rx_id;
      try {
        const auto plan = load_version_check_plan(record.source, target.id);
        option.target_options.back().version_check_available =
            !plan.items.empty();
        option.target_options.back().version_items = versionItemViews(plan);
      } catch (const std::exception& error) {
        startup_messages_.push_back(
            QStringLiteral("版本读取配置失败：%1 [%2]；%3")
                .arg(QString::fromStdWString(record.source.wstring()),
                     fromWide(target.id), fromUtf8(error.what())));
      }
    }
  }
}

} // namespace uds::ui::qt
