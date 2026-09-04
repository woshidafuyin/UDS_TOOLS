#include "ui/qt/version_confirmation_page.hpp"

#include <QAbstractItemView>
#include <QColor>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace uds::ui::qt {
namespace {

QLabel* valueLabel(QWidget* parent) {
  auto* label = new QLabel(QStringLiteral("-"), parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QTableWidgetItem* cell(const QString& text, bool mono = false) {
  auto* item = new QTableWidgetItem(text);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  if (mono) item->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  return item;
}

QString didFromRequest(const QString& request) {
  const auto bytes = request.simplified().split(QLatin1Char(' '));
  if (bytes.size() >= 3 &&
      bytes[0].compare(QStringLiteral("22"), Qt::CaseInsensitive) == 0) {
    return (bytes[1] + bytes[2]).toUpper();
  }
  return QStringLiteral("-");
}

} // namespace

VersionConfirmationPage::VersionConfirmationPage(QWidget* parent)
    : QWidget(parent) {
  setObjectName(QStringLiteral("versionConfirmationPage"));
  auto* page = new QVBoxLayout(this);
  page->setContentsMargins(8, 8, 8, 8);
  page->setSpacing(6);

  auto* context =
      new QGroupBox(QStringLiteral("读取对象（自动跟随刷写作业）"), this);
  context->setObjectName(QStringLiteral("versionContextGroup"));
  auto* context_layout = new QGridLayout(context);
  context_layout->setContentsMargins(10, 8, 10, 8);
  context_layout->setHorizontalSpacing(10);
  context_layout->setVerticalSpacing(3);
  selection_value_ = valueLabel(context);
  selection_value_->setObjectName(QStringLiteral("versionSelectionSummary"));
  auto selection_font = selection_value_->font();
  selection_font.setBold(true);
  selection_value_->setFont(selection_font);
  address_value_ = valueLabel(context);
  address_value_->setObjectName(QStringLiteral("versionAddressSummary"));
  address_value_->setStyleSheet(QStringLiteral("color: #4b5563;"));
  context_layout->addWidget(selection_value_, 0, 0);
  context_layout->addWidget(address_value_, 1, 0);
  context_layout->setColumnStretch(0, 1);
  page->addWidget(context);

  auto* actions = new QHBoxLayout;
  check_button_ = new QPushButton(QStringLiteral("一键读取"), this);
  check_button_->setObjectName(QStringLiteral("versionCheckButton"));
  stop_button_ = new QPushButton(QStringLiteral("停止"), this);
  stop_button_->setObjectName(QStringLiteral("versionStopButton"));
  report_button_ = new QPushButton(QStringLiteral("打开报告"), this);
  report_button_->setObjectName(QStringLiteral("versionReportButton"));
  check_button_->setMinimumWidth(160);
  stop_button_->setMinimumWidth(100);
  report_button_->setMinimumWidth(120);
  actions->addWidget(check_button_);
  actions->addWidget(stop_button_);
  actions->addWidget(report_button_);
  actions->addStretch();
  page->addLayout(actions);

  table_ = new QTableWidget(0, 5, this);
  table_->setObjectName(QStringLiteral("versionResultTable"));
  table_->setHorizontalHeaderLabels(
      {QStringLiteral("状态"), QStringLiteral("DID"),
       QStringLiteral("请求"), QStringLiteral("DID含义"),
       QStringLiteral("ECU读取值（ASCII / 解析值）")});
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setAlternatingRowColors(true);
  table_->verticalHeader()->setVisible(false);
  table_->verticalHeader()->setDefaultSectionSize(24);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  page->addWidget(table_, 1);

  auto* raw_group = new QGroupBox(QStringLiteral("原始UDS通信"), this);
  auto* raw_layout = new QVBoxLayout(raw_group);
  raw_layout->setContentsMargins(6, 8, 6, 6);
  raw_view_ = new QPlainTextEdit(raw_group);
  raw_view_->setObjectName(QStringLiteral("versionRawCommunication"));
  raw_view_->setReadOnly(true);
  raw_view_->setMaximumHeight(100);
  raw_view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  raw_layout->addWidget(raw_view_);
  page->addWidget(raw_group);

  connect(check_button_, &QPushButton::clicked, this,
          &VersionConfirmationPage::requestCheck);
  connect(stop_button_, &QPushButton::clicked, this,
          &VersionConfirmationPage::stopRequested);
  connect(report_button_, &QPushButton::clicked, this,
          &VersionConfirmationPage::openReportRequested);
  updateControls();
}

void VersionConfirmationPage::setContext(
    int profile_index, const QString&,
    const QString& hardware_backend, const QString& vendor,
    const QString& project,
    const QString& target_id,
    const QString& target_name, unsigned channel, quint32 tx_id, quint32 rx_id,
    const VersionReadItems& items) {
  const auto previous_key = contextKey();
  profile_index_ = profile_index;
  target_id_ = target_id;
  channel_ = channel;
  tx_id_ = tx_id;
  rx_id_ = rx_id;
  vendor_ = vendor;
  project_ = project;
  target_name_ = target_name.isEmpty() ? QStringLiteral("默认设备")
                                        : target_name;
  hardware_backend_ = hardware_backend;
  configured_ = !items.empty();
  planned_items_ = items;
  selection_value_->setText(
      QStringLiteral("%1  /  %2  /  %3").arg(vendor_, project_, target_name_));
  address_value_->setText(
      QStringLiteral("%1    ·    CH%2    TX 0x%3  →  RX 0x%4")
          .arg(hardware_backend_)
          .arg(channel_)
          .arg(tx_id_, 0, 16)
          .arg(rx_id_, 0, 16)
          .toUpper());

  const auto new_key = contextKey();
  if (!running_ &&
      (new_key != previous_key || new_key != rendered_context_key_)) {
    showPlannedItems();
  }
  updateControls();
}

void VersionConfirmationPage::setRunning(bool running) {
  running_ = running;
  if (running) {
    showPlannedItems();
    raw_view_->clear();
  }
  updateControls();
}

void VersionConfirmationPage::setOperationBusy(bool busy) {
  operation_busy_ = busy;
  updateControls();
}

void VersionConfirmationPage::clearResults() {
  table_->setRowCount(0);
  raw_view_->clear();
}

void VersionConfirmationPage::appendResult(
    const QString& status, const QString& request, const QString& name,
    const QString& actual, const QString& raw_response, const QString& detail) {
  auto row = -1;
  for (int candidate = 0; candidate < table_->rowCount(); ++candidate) {
    const auto* request_item = table_->item(candidate, 2);
    const auto* status_item = table_->item(candidate, 0);
    if (request_item && status_item && request_item->text() == request &&
        status_item->text().startsWith(QStringLiteral("待读取"))) {
      row = candidate;
      break;
    }
  }
  if (row < 0) {
    row = table_->rowCount();
    table_->insertRow(row);
  }
  table_->setItem(row, 0, cell(status, true));
  table_->setItem(row, 1, cell(didFromRequest(request), true));
  table_->setItem(row, 2, cell(request, true));
  table_->setItem(row, 3, cell(name.isEmpty() ? QStringLiteral("含义未配置")
                                              : name));
  table_->setItem(row, 4,
                  cell(actual.isEmpty() ? QStringLiteral("-") : actual, true));
  if (status == QStringLiteral("成功")) {
    table_->item(row, 0)->setForeground(Qt::darkGreen);
  } else if (status == QStringLiteral("不一致") ||
             status == QStringLiteral("错误")) {
    table_->item(row, 0)->setForeground(Qt::red);
  } else if (status == QStringLiteral("警告")) {
    table_->item(row, 0)->setForeground(QColor(QStringLiteral("#b85c00")));
  } else if (status == QStringLiteral("信息")) {
    table_->item(row, 0)->setForeground(QColor(QStringLiteral("#526d82")));
  }
  raw_view_->appendPlainText(QStringLiteral("TX  %1").arg(request));
  if (!raw_response.isEmpty())
    raw_view_->appendPlainText(QStringLiteral("RX  %1").arg(raw_response));
  if (!detail.isEmpty()) {
    raw_view_->appendPlainText(
        QStringLiteral("%1  %2")
            .arg(status == QStringLiteral("警告") ? QStringLiteral("WARN")
                                                   : QStringLiteral("FAIL"),
                 detail));
  }
}

void VersionConfirmationPage::showPlannedItems() {
  table_->setRowCount(0);
  raw_view_->clear();
  for (const auto& item : planned_items_) {
    const auto row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, 0,
                    cell(item.required ? QStringLiteral("待读取")
                                       : QStringLiteral("待读取（可选）")));
    table_->setItem(row, 1, cell(item.did, true));
    table_->setItem(row, 2, cell(item.request, true));
    table_->setItem(
        row, 3,
        cell(item.meaning.isEmpty() ? QStringLiteral("含义未配置")
                                    : item.meaning));
    table_->setItem(row, 4, cell(QStringLiteral("-"), true));
  }
  rendered_context_key_ = contextKey();
}

QString VersionConfirmationPage::contextKey() const {
  return QStringLiteral("%1|%2|%3|%4|%5|%6")
      .arg(profile_index_)
      .arg(target_id_)
      .arg(hardware_backend_)
      .arg(channel_)
      .arg(tx_id_)
      .arg(rx_id_);
}

void VersionConfirmationPage::markPendingItemsNotExecuted() {
  for (int row = 0; row < table_->rowCount(); ++row) {
    auto* status = table_->item(row, 0);
    if (!status || !status->text().startsWith(QStringLiteral("待读取")))
      continue;
    status->setText(QStringLiteral("未执行"));
    status->setForeground(Qt::gray);
  }
}

void VersionConfirmationPage::finish(bool success, bool cancelled,
                                     const QString& message) {
  running_ = false;
  if (cancelled) {
    markPendingItemsNotExecuted();
    raw_view_->appendPlainText(
        QStringLiteral("INFO  %1")
            .arg(message.isEmpty() ? QStringLiteral("版本读取已停止") : message));
  } else if (!success) {
    markPendingItemsNotExecuted();
    raw_view_->appendPlainText(
        QStringLiteral("FAIL  %1")
            .arg(message.isEmpty() ? QStringLiteral("版本读取失败") : message));
  }
  updateControls();
}

void VersionConfirmationPage::setReportAvailable(bool available) {
  report_available_ = available;
  updateControls();
}

void VersionConfirmationPage::requestCheck() {
  emit checkRequested(profile_index_, target_id_, channel_, tx_id_, rx_id_);
}

void VersionConfirmationPage::updateControls() {
  check_button_->setText(running_ ? QStringLiteral("读取中")
                                  : QStringLiteral("一键读取"));
  check_button_->setEnabled(configured_ && profile_index_ >= 0 && !running_ &&
                            !operation_busy_);
  stop_button_->setEnabled(running_);
  report_button_->setEnabled(report_available_ && !running_);
}

} // namespace uds::ui::qt
