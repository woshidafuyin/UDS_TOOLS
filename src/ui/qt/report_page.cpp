#include "ui/qt/report_page.hpp"

#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace uds::ui::qt {

ReportPage::ReportPage(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("reportWorkspacePage"));

  auto* page_layout = new QVBoxLayout(this);
  page_layout->setContentsMargins(14, 14, 14, 14);
  page_layout->setSpacing(12);

  auto* heading = new QLabel(QStringLiteral("记录与报告"), this);
  heading->setObjectName(QStringLiteral("reportPageHeading"));
  auto heading_font = heading->font();
  heading_font.setPointSize(12);
  heading_font.setBold(true);
  heading->setFont(heading_font);
  page_layout->addWidget(heading);

  auto* notice = new QLabel(
      QStringLiteral("现有“打开最新报告”按钮和报告生成路径保持不变。"
                     "此页面预留统一历史记录入口，不迁移或删除已有报告功能。"),
      this);
  notice->setWordWrap(true);
  page_layout->addWidget(notice);

  auto* history_group = new QGroupBox(QStringLiteral("最近执行记录"), this);
  auto* history_layout = new QVBoxLayout(history_group);
  auto* empty_state = new QLabel(
      QStringLiteral("历史记录索引尚未接入；当前仍通过刷写页打开最新报告。"),
      history_group);
  empty_state->setWordWrap(true);
  empty_state->setAlignment(Qt::AlignCenter);
  empty_state->setMinimumHeight(130);
  history_layout->addWidget(empty_state);

  auto* open_button =
      new QPushButton(QStringLiteral("报告历史（待接入）"), history_group);
  open_button->setEnabled(false);
  history_layout->addWidget(open_button);
  page_layout->addWidget(history_group);
  page_layout->addStretch();
}

} // namespace uds::ui::qt
