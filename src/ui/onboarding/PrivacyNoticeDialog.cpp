#include "ui/onboarding/PrivacyNoticeDialog.h"

#include "ui/glass/GlassButton.h"
#include "ui/glass/GlassSurface.h"

#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace snapask::ui::onboarding {
namespace {

QLabel* noticeItem(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("\u2022 ") + text, parent);
    label->setObjectName(QStringLiteral("privacyNoticeItem"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return label;
}

}  // namespace

PrivacyNoticeDialog::PrivacyNoticeDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("PrivacyNoticeDialog"));
    setWindowTitle(tr("开始使用 SnapAsk"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    setMinimumSize(420, 320);
    QSize initialSize(680, 620);
    QScreen* targetScreen = parent != nullptr ? parent->screen()
                                               : QGuiApplication::primaryScreen();
    if (targetScreen != nullptr) {
        const QSize available = targetScreen->availableGeometry().size()
                                    - QSize(40, 40);
        if (available.isValid()) {
            initialSize = initialSize.boundedTo(available)
                              .expandedTo(minimumSize());
        }
    }
    resize(initialSize);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    auto* shell = new snapask::ui::glass::GlassSurface(this);
    shell->setObjectName(QStringLiteral("privacyGlassShell"));
    shell->setMaterialRole(
        snapask::ui::glass::GlassMaterialRole::Elevated);
    outerLayout->addWidget(shell);

    auto* pageLayout = new QVBoxLayout(shell);
    pageLayout->setContentsMargins(24, 22, 24, 20);
    pageLayout->setSpacing(14);

    auto* title = new QLabel(tr("在继续前，请了解截图如何被处理"), shell);
    title->setObjectName(QStringLiteral("privacyNoticeTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(17);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    title->setWordWrap(true);
    pageLayout->addWidget(title);

    auto* introduction = new QLabel(
        tr("SnapAsk 以本地优先和最少披露为原则。你可以拒绝本说明并退出，不会因此发送任何截图或问题。"),
        shell);
    introduction->setObjectName(QStringLiteral("privacyNoticeIntroduction"));
    introduction->setTextFormat(Qt::PlainText);
    introduction->setWordWrap(true);
    pageLayout->addWidget(introduction);

    auto* scrollArea = new QScrollArea(shell);
    scrollArea->setObjectName(QStringLiteral("privacyNoticeScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* details = new snapask::ui::glass::GlassSurface(scrollArea);
    details->setObjectName(QStringLiteral("privacyNoticeDetails"));
    details->setMaterialRole(
        snapask::ui::glass::GlassMaterialRole::ReadableContent);
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(18, 14, 18, 14);
    detailsLayout->setSpacing(11);
    detailsLayout->addWidget(noticeItem(
        tr("默认情况下，截图和会话只驻留在内存中；SnapAsk 不会自动把它们写入磁盘或临时目录。"),
        details));
    detailsLayout->addWidget(noticeItem(
        tr("只有你点击发送或按 Ctrl+Enter 明确发送时，才会上传当时的已扁平化快照和你输入的问题；仅打开窗口、预览、编辑、保存、复制或贴图都不会触发上传。"),
        details));
    detailsLayout->addWidget(noticeItem(
        tr("发送前会显示目标 AI 服务的域名，便于你确认数据将交给谁处理。"),
        details));
    detailsLayout->addWidget(noticeItem(
        tr("自定义服务端点在首次使用前会单独请求确认；拒绝确认时不会连接该端点。"),
        details));
    detailsLayout->addWidget(noticeItem(
        tr("API Key 只保存在 Windows 凭据管理器中，不写入普通配置、导出文件或日志。"),
        details));
    detailsLayout->addWidget(noticeItem(
        tr("像素化马赛克只是一种视觉遮挡，并非强安全脱敏。发送前请自行确认敏感内容已得到充分处理。"),
        details));
    detailsLayout->addStretch();
    scrollArea->setWidget(details);
    pageLayout->addWidget(scrollArea, 1);

    auto* decisionHint = new QLabel(
        tr("选择“同意并继续”表示你已阅读以上说明。选择“拒绝并退出”将关闭首次使用流程。"),
        shell);
    decisionHint->setObjectName(QStringLiteral("privacyNoticeDecisionHint"));
    decisionHint->setTextFormat(Qt::PlainText);
    decisionHint->setWordWrap(true);
    pageLayout->addWidget(decisionHint);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    auto* rejectButton = new snapask::ui::glass::GlassButton(
        tr("拒绝并退出"),
        shell);
    rejectButton->setObjectName(QStringLiteral("privacyDeclineButton"));
    rejectButton->setAutoDefault(false);
    auto* acceptButton = new snapask::ui::glass::GlassButton(
        tr("同意并继续"),
        shell);
    acceptButton->setAccent(true);
    acceptButton->setObjectName(QStringLiteral("privacyAcceptButton"));
    acceptButton->setDefault(true);
    buttonRow->addWidget(rejectButton);
    buttonRow->addStretch();
    buttonRow->addWidget(acceptButton);
    pageLayout->addLayout(buttonRow);

    connect(rejectButton, &QPushButton::clicked,
            this, &PrivacyNoticeDialog::reject);
    connect(acceptButton, &QPushButton::clicked,
            this, &PrivacyNoticeDialog::accept);
}

void PrivacyNoticeDialog::accept()
{
    if (decisionFinalized_) {
        return;
    }
    decisionFinalized_ = true;
    emit privacyAccepted();
    QDialog::accept();
}

void PrivacyNoticeDialog::reject()
{
    if (decisionFinalized_) {
        return;
    }
    decisionFinalized_ = true;
    emit privacyRejected();
    QDialog::reject();
}

}  // namespace snapask::ui::onboarding
