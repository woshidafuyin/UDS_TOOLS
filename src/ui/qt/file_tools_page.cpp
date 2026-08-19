#include "ui/qt/file_tools_page.hpp"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace uds::ui::qt {
namespace {

QGroupBox* make_tool_group(const QString& title, const QString& description,
                           const QString& action, QWidget* parent) {
  auto* group = new QGroupBox(title, parent);
  auto* layout = new QVBoxLayout(group);
  layout->setSpacing(10);

  auto* description_label = new QLabel(description, group);
  description_label->setWordWrap(true);
  layout->addWidget(description_label);

  auto* action_button = new QPushButton(action, group);
  action_button->setEnabled(false);
  action_button->setToolTip(
      QStringLiteral("当前阶段仅完成界面分区，文件工具尚未接入"));
  layout->addWidget(action_button);
  return group;
}

} // namespace

FileToolsPage::FileToolsPage(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("fileToolsWorkspacePage"));

  auto* page_layout = new QVBoxLayout(this);
  page_layout->setContentsMargins(14, 14, 14, 14);
  page_layout->setSpacing(12);

  auto* heading = new QLabel(QStringLiteral("文件工具"), this);
  heading->setObjectName(QStringLiteral("fileToolsPageHeading"));
  auto heading_font = heading->font();
  heading_font.setPointSize(12);
  heading_font.setBold(true);
  heading->setFont(heading_font);
  page_layout->addWidget(heading);

  auto* notice = new QLabel(
      QStringLiteral("离线文件能力与刷写Workflow完全隔离，不占用CAN通道，"
                     "也不修改任何项目刷写配置。"),
      this);
  notice->setWordWrap(true);
  page_layout->addWidget(notice);

  auto* tools = new QGridLayout;
  tools->setHorizontalSpacing(12);
  tools->setVerticalSpacing(12);
  tools->addWidget(
      make_tool_group(QStringLiteral("BIN 转 S19"),
                      QStringLiteral("配置起始地址、版本和件号后生成S19文件。"),
                      QStringLiteral("转换（待接入）"), this),
      0, 0);
  tools->addWidget(
      make_tool_group(QStringLiteral("S19检查与合并"),
                      QStringLiteral("地址范围检查、重叠检测和多文件合并。"),
                      QStringLiteral("检查与合并（待接入）"), this),
      0, 1);
  tools->addWidget(
      make_tool_group(QStringLiteral("校验信息"),
                      QStringLiteral("计算CRC、SHA-256并输出可追溯文件信息。"),
                      QStringLiteral("计算校验（待接入）"), this),
      1, 0, 1, 2);
  page_layout->addLayout(tools);
  page_layout->addStretch();
}

} // namespace uds::ui::qt
