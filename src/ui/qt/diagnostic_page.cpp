#include "ui/qt/diagnostic_page.hpp"

#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace uds::ui::qt {
namespace {

QGroupBox* make_feature_group(const QString& title, const QString& description,
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
      QStringLiteral("当前阶段仅完成界面分区，诊断服务尚未接入"));
  layout->addWidget(action_button);
  return group;
}

} // namespace

DiagnosticPage::DiagnosticPage(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("diagnosticWorkspacePage"));

  auto* page_layout = new QVBoxLayout(this);
  page_layout->setContentsMargins(14, 14, 14, 14);
  page_layout->setSpacing(12);

  auto* heading = new QLabel(QStringLiteral("UDS 诊断"), this);
  heading->setObjectName(QStringLiteral("diagnosticPageHeading"));
  auto heading_font = heading->font();
  heading_font.setPointSize(12);
  heading_font.setBold(true);
  heading->setFont(heading_font);
  page_layout->addWidget(heading);

  auto* notice = new QLabel(
      QStringLiteral("已与刷写作业分区。当前只建立独立页面边界，未接入任何"
                     "CAN、UDS或安全访问操作，因此不会影响现有刷写流程。"),
      this);
  notice->setWordWrap(true);
  page_layout->addWidget(notice);

  auto* features = new QGridLayout;
  features->setHorizontalSpacing(12);
  features->setVerticalSpacing(12);
  features->addWidget(
      make_feature_group(
          QStringLiteral("DID / 版本"),
          QStringLiteral("项目化DID字典、版本组读取、原始响应与解析结果。"),
          QStringLiteral("读取版本组（待接入）"), this),
      0, 0);
  features->addWidget(
      make_feature_group(
          QStringLiteral("DTC"),
          QStringLiteral("读取、解析和清除DTC，后续使用独立诊断控制器。"),
          QStringLiteral("读取DTC（待接入）"), this),
      0, 1);
  features->addWidget(
      make_feature_group(
          QStringLiteral("会话与安全访问"),
          QStringLiteral("默认/扩展/编程会话及按项目配置的安全访问。"),
          QStringLiteral("进入会话（待接入）"), this),
      1, 0);
  features->addWidget(
      make_feature_group(
          QStringLiteral("高级诊断"),
          QStringLiteral("例程控制、通信控制和受约束的原始UDS请求。"),
          QStringLiteral("高级诊断（待接入）"), this),
      1, 1);
  page_layout->addLayout(features);
  page_layout->addStretch();
}

} // namespace uds::ui::qt
