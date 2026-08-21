#include "ui/qt/diagnostic_request_page.hpp"

#include "core/hex.hpp"
#include "core/uds_nrc.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace uds::ui::qt {

DiagnosticRequestPage::DiagnosticRequestPage(QWidget* parent)
    : QWidget(parent) {
  setObjectName(QStringLiteral("diagnosticRequestPage"));
  auto* layout = new QVBoxLayout(this);
  auto* context_group = new QGroupBox(QStringLiteral("当前刷写作业配置（自动跟随）"), this);
  auto* context_layout = new QVBoxLayout(context_group);
  context_ = new QLabel(QStringLiteral("尚未选择有效项目"), context_group);
  context_->setObjectName(QStringLiteral("diagnosticRequestContext"));
  context_->setWordWrap(true);
  context_layout->addWidget(context_);
  layout->addWidget(context_group);

  auto* send_group = new QGroupBox(QStringLiteral("单次 UDS 请求（ISO-TP）"), this);
  auto* form = new QFormLayout(send_group);
  addressing_ = new QComboBox(send_group);
  addressing_->setObjectName(QStringLiteral("diagnosticAddressingComboBox"));
  addressing_->addItem(QStringLiteral("物理寻址"), false);
  addressing_->addItem(QStringLiteral("功能寻址（响应仍限定当前ECU）"), true);
  payload_ = new QLineEdit(send_group);
  payload_->setObjectName(QStringLiteral("diagnosticPayloadLineEdit"));
  payload_->setPlaceholderText(QStringLiteral("例如：22 F1 87、10 03、31 01 02 03"));
  timeout_ = new QSpinBox(send_group);
  timeout_->setObjectName(QStringLiteral("diagnosticTimeoutSpinBox"));
  timeout_->setRange(100, 30000);
  timeout_->setSingleStep(100);
  timeout_->setValue(2000);
  timeout_->setSuffix(QStringLiteral(" ms"));
  form->addRow(QStringLiteral("寻址方式"), addressing_);
  form->addRow(QStringLiteral("UDS 数据"), payload_);
  form->addRow(QStringLiteral("响应超时"), timeout_);
  layout->addWidget(send_group);

  auto* buttons = new QHBoxLayout;
  send_ = new QPushButton(QStringLiteral("发送并等待响应"), this);
  send_->setObjectName(QStringLiteral("diagnosticSendButton"));
  stop_ = new QPushButton(QStringLiteral("停止等待"), this);
  stop_->setObjectName(QStringLiteral("diagnosticStopButton"));
  buttons->addWidget(send_);
  buttons->addWidget(stop_);
  buttons->addStretch();
  layout->addLayout(buttons);
  summary_ = new QLabel(QStringLiteral("请输入一条 UDS 请求。"), this);
  summary_->setObjectName(QStringLiteral("diagnosticRequestSummary"));
  summary_->setWordWrap(true);
  layout->addWidget(summary_);
  communication_ = new QPlainTextEdit(this);
  communication_->setObjectName(QStringLiteral("diagnosticRawCommunication"));
  communication_->setReadOnly(true);
  layout->addWidget(communication_, 1);
  connect(send_, &QPushButton::clicked, this, &DiagnosticRequestPage::requestSend);
  connect(stop_, &QPushButton::clicked, this, &DiagnosticRequestPage::stopRequested);
  connect(payload_, &QLineEdit::returnPressed, this, &DiagnosticRequestPage::requestSend);
  updateControls();
}

void DiagnosticRequestPage::setContext(
    int profile_index, const QString& target_id, const QString& hardware,
    const QString& vendor, const QString& project, const QString& target_name,
    unsigned channel, unsigned nominal_bitrate, unsigned data_bitrate,
    bool can_fd, quint32 tx_id, quint32 rx_id, quint32 functional_id) {
  profile_index_ = profile_index;
  target_id_ = target_id;
  channel_ = channel;
  physical_tx_id_ = tx_id;
  physical_rx_id_ = rx_id;
  functional_id_ = functional_id;
  configured_ = profile_index >= 0 && tx_id != 0 && rx_id != 0;
  context_->setText(QStringLiteral("%1 / %2 / %3 · %4 · CH%5 · %6 kbit/s · %7 · "
                                   "物理 0x%8 → 0x%9 · 功能 0x%10")
                        .arg(vendor, project, target_name, hardware)
                        .arg(channel)
                        .arg(nominal_bitrate / 1000)
                        .arg(can_fd ? QStringLiteral("CAN FD %1 kbit/s").arg(data_bitrate / 1000)
                                    : QStringLiteral("CAN"))
                        .arg(tx_id, 0, 16).arg(rx_id, 0, 16)
                        .arg(functional_id, 0, 16).toUpper());
  addressing_->setItemData(1, functional_id != 0 ? QVariant{} : QVariant(0),
                           Qt::UserRole - 1);
  updateControls();
}

bool DiagnosticRequestPage::isStateChangingService(quint8 sid) const {
  switch (sid) {
  case 0x10: case 0x11: case 0x27: case 0x28: case 0x2E:
  case 0x31: case 0x34: case 0x36: case 0x37: case 0x3D: case 0x85:
    return true;
  default:
    return false;
  }
}

void DiagnosticRequestPage::requestSend() {
  std::vector<std::uint8_t> bytes;
  try { bytes = from_hex(payload_->text().toStdString()); }
  catch (const std::exception& error) {
    QMessageBox::warning(this, QStringLiteral("UDS 数据格式错误"),
                         QString::fromUtf8(error.what()));
    return;
  }
  if (bytes.empty()) return;
  if (isStateChangingService(bytes.front()) &&
      QMessageBox::warning(
          this, QStringLiteral("诊断服务可能改变 ECU 状态"),
          QStringLiteral("请求 %1 可能改变会话、安全状态、存储内容或刷写状态。\n"
                         "当前页面不会执行项目 Workflow 的前置条件和恢复步骤。\n\n确认发送？")
              .arg(QString::fromStdString(to_hex(bytes))),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
  const bool functional = addressing_->currentData().toBool();
  emit sendRequested(profile_index_, target_id_, channel_,
                     functional ? functional_id_ : physical_tx_id_,
                     physical_rx_id_, payload_->text(),
                     static_cast<unsigned>(timeout_->value()));
}

void DiagnosticRequestPage::setRunning(bool running) {
  running_ = running;
  if (running) {
    summary_->setText(QStringLiteral("正在发送并等待当前 ECU 响应……"));
    communication_->clear();
  }
  updateControls();
}

void DiagnosticRequestPage::setOperationBusy(bool busy) {
  operation_busy_ = busy;
  updateControls();
}

void DiagnosticRequestPage::finish(bool success, bool cancelled,
                                   const QString& request,
                                   const QString& response,
                                   const QString& detail, unsigned elapsed_ms,
                                   quint8 nrc) {
  running_ = false;
  summary_->setText(cancelled ? QStringLiteral("已停止等待响应。")
                              : QStringLiteral("%1（%2 ms）%3")
                                    .arg(success ? QStringLiteral("请求成功")
                                                 : QStringLiteral("请求失败"))
                                    .arg(elapsed_ms)
                                    .arg(nrc ? QStringLiteral("，NRC 0x%1").arg(nrc, 2, 16, QLatin1Char('0')).toUpper()
                                             : QString{}));
  communication_->setPlainText(QStringLiteral("TX  %1\nRX  %2\n\n%3")
                                   .arg(request, response.isEmpty() ? QStringLiteral("<无响应>") : response, detail));
  updateControls();
}

void DiagnosticRequestPage::updateControls() {
  const bool available = configured_ && !operation_busy_;
  addressing_->setEnabled(available && !running_);
  payload_->setEnabled(available && !running_);
  timeout_->setEnabled(available && !running_);
  send_->setEnabled(available && !running_);
  stop_->setEnabled(running_);
}

} // namespace uds::ui::qt
