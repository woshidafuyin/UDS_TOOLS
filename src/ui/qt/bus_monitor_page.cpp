#include "ui/qt/bus_monitor_page.hpp"

#include "core/hex.hpp"
#include "core/uds_nrc.hpp"
#include "drivers/can/can_bus_provider.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>

namespace uds::ui::qt {
namespace {
constexpr int kMaximumRows = 10000;

void insertRow(QTableWidget* table, int index,
               const BusMonitorPage::Row& row) {
  table->insertRow(index);
  const std::array<QString, 7> values{
      row.time, row.direction, row.id, row.type, row.length, row.data,
      row.diagnostic};
  for (int column = 0; column < static_cast<int>(values.size()); ++column) {
    auto* item =
        new QTableWidgetItem(values[static_cast<std::size_t>(column)]);
    switch (row.diagnostic_tone) {
    case BusMonitorPage::DiagnosticTone::Failure:
      item->setForeground(QColor(QStringLiteral("#B71C1C")));
      item->setBackground(QColor(QStringLiteral("#FDECEC")));
      item->setData(Qt::UserRole, QStringLiteral("failure"));
      item->setToolTip(row.diagnostic);
      {
        auto font = item->font();
        font.setBold(true);
        item->setFont(font);
      }
      break;
    case BusMonitorPage::DiagnosticTone::Pending:
      item->setForeground(QColor(QStringLiteral("#A85D00")));
      item->setBackground(QColor(QStringLiteral("#FFF4DD")));
      item->setData(Qt::UserRole, QStringLiteral("pending"));
      item->setToolTip(row.diagnostic);
      break;
    case BusMonitorPage::DiagnosticTone::Normal:
      item->setData(Qt::UserRole, QStringLiteral("normal"));
      break;
    }
    table->setItem(index, column, item);
  }
}
} // namespace

BusMonitorPage::BusMonitorPage(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("busMonitorWorkspacePage"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  auto* configuration = new QGroupBox(QStringLiteral("总线监听配置（被动模式）"), this);
  auto* config_layout = new QGridLayout(configuration);
  config_layout->addWidget(new QLabel(QStringLiteral("当前通道："), configuration), 0, 0);
  context_label_ = new QLabel(configuration);
  config_layout->addWidget(context_label_, 0, 1, 1, 5);
  config_layout->addWidget(new QLabel(QStringLiteral("状态："), configuration), 1, 0);
  status_label_ = new QLabel(
      QStringLiteral("工具启动后自动监听刷写作业当前通道；不会发送任何 CAN/UDS 报文。"),
      configuration);
  config_layout->addWidget(status_label_, 1, 1, 1, 5);
  clear_button_ = new QPushButton(QStringLiteral("清空列表"), configuration);
  clear_button_->setObjectName(QStringLiteral("busMonitorClearButton"));
  export_button_ = new QPushButton(QStringLiteral("导出 ASC"), configuration);
  export_button_->setObjectName(QStringLiteral("busMonitorExportButton"));
  config_layout->addWidget(clear_button_, 2, 1);
  config_layout->addWidget(export_button_, 2, 2);
  layout->addWidget(configuration);

  auto* filter = new QGroupBox(QStringLiteral("显示过滤"), this);
  auto* filter_layout = new QGridLayout(filter);
  filter_layout->addWidget(new QLabel(QStringLiteral("ID："), filter), 0, 0);
  id_filter_ = new QLineEdit(filter);
  id_filter_->setPlaceholderText(QStringLiteral("如 772、7DF、18DAF1B6；空=全部"));
  filter_layout->addWidget(id_filter_, 0, 1);
  filter_layout->addWidget(new QLabel(QStringLiteral("数据："), filter), 0, 2);
  data_filter_ = new QLineEdit(filter);
  data_filter_->setPlaceholderText(QStringLiteral("如 10 02；空=全部"));
  filter_layout->addWidget(data_filter_, 0, 3);
  diagnostic_only_filter_ =
      new QCheckBox(QStringLiteral("仅显示诊断 ID"), filter);
  diagnostic_only_filter_->setObjectName(
      QStringLiteral("busMonitorDiagnosticOnlyFilter"));
  diagnostic_only_filter_->setChecked(true);
  diagnostic_only_filter_->setToolTip(
      QStringLiteral("只影响表格显示；后台仍接收、缓存并导出全部帧。"));
  tx_filter_ = new QCheckBox(QStringLiteral("TX"), filter);
  tx_filter_->setObjectName(QStringLiteral("busMonitorTxFilter"));
  rx_filter_ = new QCheckBox(QStringLiteral("RX"), filter);
  rx_filter_->setObjectName(QStringLiteral("busMonitorRxFilter"));
  standard_filter_ = new QCheckBox(QStringLiteral("标准帧"), filter);
  extended_filter_ = new QCheckBox(QStringLiteral("扩展帧"), filter);
  can_filter_ = new QCheckBox(QStringLiteral("CAN"), filter);
  fd_filter_ = new QCheckBox(QStringLiteral("CAN FD"), filter);
  brs_filter_ = new QCheckBox(QStringLiteral("仅 BRS"), filter);
  standard_filter_->setChecked(true);
  extended_filter_->setChecked(true);
  can_filter_->setChecked(true);
  fd_filter_->setChecked(true);
  tx_filter_->setChecked(true);
  rx_filter_->setChecked(true);
  filter_layout->addWidget(diagnostic_only_filter_, 1, 0, 1, 2);
  filter_layout->addWidget(tx_filter_, 1, 2);
  filter_layout->addWidget(rx_filter_, 1, 3);
  filter_layout->addWidget(standard_filter_, 1, 4);
  filter_layout->addWidget(extended_filter_, 1, 5);
  filter_layout->addWidget(can_filter_, 1, 6);
  filter_layout->addWidget(fd_filter_, 1, 7);
  filter_layout->addWidget(brs_filter_, 1, 8);
  count_label_ = new QLabel(filter);
  filter_layout->addWidget(count_label_, 0, 5);
  layout->addWidget(filter);

  table_ = new QTableWidget(0, 7, this);
  table_->setObjectName(QStringLiteral("busMonitorTable"));
  table_->setHorizontalHeaderLabels(
      {QStringLiteral("时间"), QStringLiteral("方向"), QStringLiteral("ID"),
       QStringLiteral("类型"), QStringLiteral("长度"), QStringLiteral("数据"),
       QStringLiteral("诊断提示")});
  table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  layout->addWidget(table_, 1);

  connect(clear_button_, &QPushButton::clicked, this, &BusMonitorPage::clearFrames);
  connect(export_button_, &QPushButton::clicked, this, &BusMonitorPage::exportAsc);
  const auto refresh = [this] {
    rebuildTable();
  };
  connect(id_filter_, &QLineEdit::textChanged, this, refresh);
  connect(data_filter_, &QLineEdit::textChanged, this, refresh);
  for (auto* check : {diagnostic_only_filter_, tx_filter_, rx_filter_,
                      standard_filter_, extended_filter_, can_filter_,
                      fd_filter_, brs_filter_}) {
    connect(check, &QCheckBox::toggled, this, refresh);
  }
  setContext(channel_, nominal_bitrate_, data_bitrate_, can_fd_);
  clearFrames();
  updateControls();
}

BusMonitorPage::~BusMonitorPage() { stopMonitoring(); }

void BusMonitorPage::stop() { stopMonitoring(); }

void BusMonitorPage::start() { startMonitoring(); }

void BusMonitorPage::restartForBackendChange() {
  if (!running_) return;
  stopMonitoring();
  startMonitoring();
}

void BusMonitorPage::setContext(unsigned channel, unsigned nominal_bitrate, unsigned data_bitrate, bool can_fd) {
  channel = std::max(1U, channel);
  const auto unchanged =
      channel_ == channel && nominal_bitrate_ == nominal_bitrate &&
      data_bitrate_ == data_bitrate && can_fd_ == can_fd;
  context_label_->setText(QStringLiteral("CH%1；%2 kbit/s；%3")
                              .arg(channel)
                              .arg(nominal_bitrate / 1000)
                              .arg(can_fd ? QStringLiteral("CAN FD %1 Mbit/s")
                                                .arg(data_bitrate / 1000000)
                                          : QStringLiteral("经典 CAN")));
  if (unchanged) return;
  const auto restart = running_;
  if (restart) stopMonitoring();
  channel_ = channel;
  nominal_bitrate_ = nominal_bitrate;
  data_bitrate_ = data_bitrate;
  can_fd_ = can_fd;
  if (restart) startMonitoring();
}

void BusMonitorPage::setOperationBusy(bool busy) {
  if (operation_busy_ == busy) return;
  operation_busy_ = busy;
  // During flashing, retain frames in the bounded model but do not mutate the
  // QTableWidget thousands of times. Rebuild once when the operation ends.
  if (!operation_busy_) rebuildTable();
  updateControls();
}

void BusMonitorPage::setDiagnosticIds(
    std::vector<std::uint32_t> diagnostic_ids) {
  std::sort(diagnostic_ids.begin(), diagnostic_ids.end());
  diagnostic_ids.erase(
      std::remove(diagnostic_ids.begin(), diagnostic_ids.end(), 0U),
      diagnostic_ids.end());
  diagnostic_ids.erase(
      std::unique(diagnostic_ids.begin(), diagnostic_ids.end()),
      diagnostic_ids.end());
  if (diagnostic_ids_ == diagnostic_ids) return;
  diagnostic_ids_ = std::move(diagnostic_ids);

  QString id_summary;
  for (const auto id : diagnostic_ids_) {
    if (!id_summary.isEmpty()) id_summary += QStringLiteral("、");
    id_summary += QStringLiteral("0x%1").arg(id, 0, 16).toUpper();
  }
  diagnostic_only_filter_->setToolTip(
      diagnostic_ids_.empty()
          ? QStringLiteral(
                "当前未配置诊断 ID，因此不限制表格显示；后台始终接收、缓存并导出全部帧。")
          : QStringLiteral(
                "当前诊断 ID：%1。只影响表格显示；后台仍接收、缓存并导出全部帧。")
                .arg(id_summary));
  rebuildTable();
}

bool BusMonitorPage::matchesContext(unsigned channel, unsigned nominal_bitrate,
                                    unsigned data_bitrate, bool can_fd) const noexcept {
  return channel_ == channel && nominal_bitrate_ == nominal_bitrate &&
         data_bitrate_ == data_bitrate && can_fd_ == can_fd;
}

void BusMonitorPage::startMonitoring() {
  if (running_) return;
  stop_requested_.store(false);
  running_ = true;
  status_label_->setText(QStringLiteral("正在被动监听 CH%1；不发送任何帧。").arg(channel_));
  updateControls();
  emit runningChanged(true);
  emit monitorMessage(QStringLiteral("总线监听已启动：CH%1，被动接收，零发送。").arg(channel_));
  const auto provider = default_can_bus_provider();
  const auto channel = channel_; const auto nominal = nominal_bitrate_; const auto data_bitrate = data_bitrate_; const auto can_fd = can_fd_;
  worker_ = std::jthread([this, provider, channel, nominal, data_bitrate, can_fd](std::stop_token stop) {
    try {
      auto bus = provider->create({"", channel, nominal, data_bitrate, can_fd, L"UDSToolBusMonitor"});
      bus->open();
      std::vector<CanFrame> batch;
      batch.reserve(128);
      auto last_flush = std::chrono::steady_clock::now();
      while (!stop.stop_requested() && !stop_requested_.load()) {
        const auto frame = bus->receive(std::chrono::milliseconds(50));
        if (frame) batch.push_back(*frame);
        const auto now = std::chrono::steady_clock::now();
        if (!batch.empty() &&
            (batch.size() >= 128U ||
             now - last_flush >= std::chrono::milliseconds(50))) {
          QMetaObject::invokeMethod(
              this,
              [this, observed = std::move(batch)]() mutable {
                appendObservedFrames(std::move(observed));
              },
              Qt::QueuedConnection);
          batch.clear();
          batch.reserve(128);
          last_flush = now;
        }
      }
      if (!batch.empty()) {
        QMetaObject::invokeMethod(
            this,
            [this, observed = std::move(batch)]() mutable {
              appendObservedFrames(std::move(observed));
            },
            Qt::QueuedConnection);
      }
      bus->close();
      QMetaObject::invokeMethod(this, [this] { if (running_) stopMonitoring(); }, Qt::QueuedConnection);
    } catch (const std::exception& error) {
      const auto message = QString::fromLocal8Bit(error.what());
      QMetaObject::invokeMethod(this, [this, message] { status_label_->setText(QStringLiteral("监听失败：%1").arg(message)); emit monitorMessage(QStringLiteral("总线监听失败：%1").arg(message)); if (running_) stopMonitoring(); }, Qt::QueuedConnection);
    }
  });
}

void BusMonitorPage::stopMonitoring() {
  stop_requested_.store(true);
  if (worker_.joinable()) worker_.request_stop();
  if (worker_.joinable() && std::this_thread::get_id() != worker_.get_id()) worker_.join();
  if (!running_) return;
  running_ = false;
  status_label_->setText(QStringLiteral("已停止；监听期间未发送任何 CAN/UDS 报文。"));
  updateControls();
  emit runningChanged(false);
  emit monitorMessage(QStringLiteral("总线监听已停止。"));
}

bool BusMonitorPage::matchesFilter(const Row& row) const {
  if (diagnostic_only_filter_->isChecked() && !diagnostic_ids_.empty() &&
      !std::binary_search(diagnostic_ids_.cbegin(), diagnostic_ids_.cend(),
                          row.can_id)) {
    return false;
  }
  if (row.direction == QStringLiteral("TX") && !tx_filter_->isChecked()) return false;
  if (row.direction == QStringLiteral("RX") && !rx_filter_->isChecked()) return false;
  const auto id = id_filter_->text().trimmed().remove(QStringLiteral("0x"), Qt::CaseInsensitive).toUpper();
  if (!id.isEmpty() && !row.id.contains(id, Qt::CaseInsensitive)) return false;
  const auto data_text = data_filter_->text().simplified().toUpper();
  if (!data_text.isEmpty() && !row.data.contains(data_text, Qt::CaseInsensitive)) return false;
  if (row.type.startsWith(QStringLiteral("STD")) && !standard_filter_->isChecked()) return false;
  if (row.type.startsWith(QStringLiteral("EXT")) && !extended_filter_->isChecked()) return false;
  if (row.type.contains(QStringLiteral("CAN FD")) && !fd_filter_->isChecked()) return false;
  if (!row.type.contains(QStringLiteral("CAN FD")) && !can_filter_->isChecked()) return false;
  return !brs_filter_->isChecked() || row.type.endsWith(QStringLiteral("BRS"));
}

void BusMonitorPage::appendObservedFrame(const CanFrame& frame) {
  Row row{frame.id,
          QDateTime::currentDateTime().toString(
              QStringLiteral("HH:mm:ss.zzz")),
          frame.transmitted ? QStringLiteral("TX") : QStringLiteral("RX"),
          QStringLiteral("0x%1").arg(frame.id, 0, 16).toUpper(),
          QStringLiteral("%1 %2%3")
              .arg(frame.extended ? QStringLiteral("EXT")
                                  : QStringLiteral("STD"),
                   frame.fd ? QStringLiteral("CAN FD")
                            : QStringLiteral("CAN"),
                   frame.brs ? QStringLiteral(" BRS") : QString{}),
          QString::number(frame.data.size()),
          QString::fromStdString(to_hex(frame.data))};
  if (!frame.transmitted) {
    if (const auto negative =
            parse_isotp_single_frame_negative_response(frame.data)) {
      // NRC 0x78 is the normal asynchronous wait state used for every
      // TransferData block on ARC331. Keep its raw CAN frame visible without
      // an alarm-like diagnostic/color; only a final NRC is emphasized.
      if (negative->kind == UdsNegativeResponseKind::failure) {
        const auto detail = format_uds_nrc(negative->nrc);
        row.diagnostic = QString::fromUtf8(
            detail.data(), static_cast<int>(detail.size()));
        row.diagnostic_tone = DiagnosticTone::Failure;
      }
    } else if (const auto routine =
                   parse_isotp_single_frame_routine_result(frame.data);
               routine && routine->failure) {
      const auto detail = format_uds_routine_result(*routine);
      row.diagnostic = QString::fromUtf8(
          detail.data(), static_cast<int>(detail.size()));
      row.diagnostic_tone = DiagnosticTone::Failure;
    }
  }
  appendFrame(std::move(row));
}

void BusMonitorPage::appendObservedFrames(std::vector<CanFrame> frames) {
  batch_appending_ = true;
  if (!operation_busy_) table_->setUpdatesEnabled(false);
  for (const auto& frame : frames) appendObservedFrame(frame);
  batch_appending_ = false;
  if (!operation_busy_) {
    table_->setUpdatesEnabled(true);
    table_->scrollToBottom();
  }
  count_label_->setText(
      QStringLiteral("已记录：%1 帧（最多保留 %2 帧）")
          .arg(rows_.size())
          .arg(kMaximumRows));
  updateControls();
}

void BusMonitorPage::appendFrame(Row row) {
  bool evicted_visible{};
  if (static_cast<int>(rows_.size()) >= kMaximumRows) {
    evicted_visible = matchesFilter(rows_.front());
    rows_.pop_front();
  }
  rows_.push_back(row);
  if (!operation_busy_ && evicted_visible && table_->rowCount() > 0) {
    table_->removeRow(0);
  }
  if (!operation_busy_ && matchesFilter(row)) {
    const auto index = table_->rowCount();
    insertRow(table_, index, row);
  }
  if (!batch_appending_) {
    if (!operation_busy_) table_->scrollToBottom();
    count_label_->setText(
        QStringLiteral("已记录：%1 帧（最多保留 %2 帧）")
            .arg(rows_.size())
            .arg(kMaximumRows));
    updateControls();
  }
}

void BusMonitorPage::rebuildTable() {
  table_->setUpdatesEnabled(false);
  table_->setRowCount(0);
  for (const auto& row : rows_) {
    if (!matchesFilter(row)) continue;
    insertRow(table_, table_->rowCount(), row);
  }
  table_->setUpdatesEnabled(true);
  table_->scrollToBottom();
}

void BusMonitorPage::clearFrames() { rows_.clear(); table_->setRowCount(0); count_label_->setText(QStringLiteral("已接收：0 帧")); }

void BusMonitorPage::exportAsc() {
  const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出监听报文"), QStringLiteral("bus_monitor_CH%1.asc").arg(channel_), QStringLiteral("ASC 文件 (*.asc)"));
  if (path.isEmpty()) return;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { status_label_->setText(QStringLiteral("导出失败：无法写入 %1").arg(path)); return; }
  file.write("date " + QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8() + "\nbase hex  timestamps absolute\n\n");
  for (const auto& row : rows_) file.write(QStringLiteral("%1 %2 %3 d %4 %5\n").arg(row.time, QString::number(channel_), row.direction, row.length, row.data).toUtf8());
  if (!file.commit()) { status_label_->setText(QStringLiteral("导出失败：无法保存 %1").arg(path)); return; }
  status_label_->setText(QStringLiteral("已导出 %1 帧到 %2").arg(rows_.size()).arg(path));
}

void BusMonitorPage::updateControls() {
  clear_button_->setEnabled(!rows_.empty());
  export_button_->setEnabled(!rows_.empty());
}

} // namespace uds::ui::qt
