#include "ui/settings/SettingsDialog.h"

#include "SnapAskVersion.h"
#include "ui/common/GlyphIcon.h"
#include "ui/settings/ProviderSettingsWidget.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace snapask::ui {
namespace {

constexpr auto kThemeSettingsKey = "appearance/theme";
constexpr auto kCaptureHotkeySettingsKey = "capture/hotkey";
constexpr auto kDefaultCaptureHotkey = "Ctrl+Shift+Space";

QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPointSize(font.pointSize() + 1);
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QLabel* makeDescription(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SecondaryLabel"));
    label->setWordWrap(true);
    return label;
}

QFrame* makeGlassCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("SettingsCard"));
    return card;
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("SettingsDialog"));
    setWindowTitle(tr("SnapAsk 设置"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(860, 600);
    resize(980, 700);

    auto* shell = new QHBoxLayout(this);
    shell->setContentsMargins(18, 18, 18, 18);
    shell->setSpacing(14);

    auto* sidebarSurface = new QFrame(this);
    sidebarSurface->setObjectName(QStringLiteral("GlassSurface"));
    sidebarSurface->setFixedWidth(190);
    auto* sidebarLayout = new QVBoxLayout(sidebarSurface);
    sidebarLayout->setContentsMargins(12, 16, 12, 12);
    sidebarLayout->setSpacing(12);

    auto* brand = new QLabel(tr("SnapAsk"), sidebarSurface);
    QFont brandFont = brand->font();
    brandFont.setPointSize(17);
    brandFont.setWeight(QFont::DemiBold);
    brand->setFont(brandFont);
    sidebarLayout->addWidget(brand);

    sidebar_ = new QListWidget(sidebarSurface);
    sidebar_->setObjectName(QStringLiteral("settingsSidebar"));
    sidebar_->setIconSize(QSize(19, 19));
    const QColor iconColor = palette().color(QPalette::WindowText);
    auto* generalItem = new QListWidgetItem(
        glyphIcon(Glyph::General, iconColor), tr("通用"), sidebar_);
    generalItem->setData(Qt::UserRole, tr("通用设置"));
    auto* serviceItem = new QListWidgetItem(
        glyphIcon(Glyph::Service, iconColor), tr("AI 服务"), sidebar_);
    serviceItem->setData(Qt::UserRole, tr("AI 服务"));
    auto* privacyItem = new QListWidgetItem(
        glyphIcon(Glyph::Privacy, iconColor), tr("隐私与关于"), sidebar_);
    privacyItem->setData(Qt::UserRole, tr("隐私与关于"));
    sidebarLayout->addWidget(sidebar_, 1);

    auto* versionLabel = new QLabel(
        tr("版本 %1").arg(QStringLiteral(SNAPASK_VERSION_STRING)),
        sidebarSurface);
    versionLabel->setObjectName(QStringLiteral("SecondaryLabel"));
    sidebarLayout->addWidget(versionLabel);
    shell->addWidget(sidebarSurface);

    auto* contentSurface = new QFrame(this);
    contentSurface->setObjectName(QStringLiteral("GlassSurface"));
    auto* contentLayout = new QVBoxLayout(contentSurface);
    contentLayout->setContentsMargins(22, 18, 22, 20);
    contentLayout->setSpacing(14);

    auto* titleRow = new QHBoxLayout();
    pageTitleLabel_ = new QLabel(tr("通用设置"), contentSurface);
    QFont titleFont = pageTitleLabel_->font();
    titleFont.setPointSize(18);
    titleFont.setWeight(QFont::DemiBold);
    pageTitleLabel_->setFont(titleFont);
    titleRow->addWidget(pageTitleLabel_);
    titleRow->addStretch(1);
    auto* closeButton = new QToolButton(contentSurface);
    closeButton->setObjectName(QStringLiteral("settingsCloseButton"));
    closeButton->setIcon(glyphIcon(Glyph::Close, iconColor));
    closeButton->setToolTip(tr("关闭设置"));
    closeButton->setAccessibleName(tr("关闭设置"));
    titleRow->addWidget(closeButton);
    contentLayout->addLayout(titleRow);

    pages_ = new QStackedWidget(contentSurface);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    contentLayout->addWidget(pages_, 1);
    shell->addWidget(contentSurface, 1);

    auto* generalScroll = new QScrollArea(pages_);
    generalScroll->setWidgetResizable(true);
    generalScroll->setFrameShape(QFrame::NoFrame);
    auto* generalPage = new QWidget(generalScroll);
    auto* generalLayout = new QVBoxLayout(generalPage);
    generalLayout->setContentsMargins(2, 2, 8, 2);
    generalLayout->setSpacing(14);

    auto* appearanceCard = makeGlassCard(generalPage);
    auto* cardLayout = new QVBoxLayout(appearanceCard);
    cardLayout->setContentsMargins(18, 16, 18, 16);
    cardLayout->setSpacing(8);
    cardLayout->addWidget(makeSectionTitle(tr("外观"), appearanceCard));
    cardLayout->addWidget(makeDescription(
        tr("所有窗口共用系统毛玻璃材质；不支持时自动使用可读的半透明背景。"),
        appearanceCard));

    auto* themeRow = new QHBoxLayout();
    auto* themeLabel = new QLabel(tr("主题"), appearanceCard);
    themeModeCombo_ = new QComboBox(appearanceCard);
    themeModeCombo_->setObjectName(QStringLiteral("themeModeCombo"));
    themeModeCombo_->setAccessibleName(tr("界面主题"));
    themeModeCombo_->addItem(tr("跟随系统"), ThemeTokens::toStorage(ThemeMode::System));
    themeModeCombo_->addItem(tr("浅色"), ThemeTokens::toStorage(ThemeMode::Light));
    themeModeCombo_->addItem(tr("深色"), ThemeTokens::toStorage(ThemeMode::Dark));
    themeRow->addWidget(themeLabel);
    themeRow->addStretch();
    themeRow->addWidget(themeModeCombo_);
    cardLayout->addLayout(themeRow);
    generalLayout->addWidget(appearanceCard);

    auto* captureCard = makeGlassCard(generalPage);
    auto* captureLayout = new QVBoxLayout(captureCard);
    captureLayout->setContentsMargins(18, 16, 18, 16);
    captureLayout->setSpacing(8);
    captureLayout->addWidget(makeSectionTitle(tr("截图"), captureCard));
    captureLayout->addWidget(makeDescription(
        tr("选区完成后显示极简浮动工具条；只有点击提问并明确发送才会上传图片。"),
        captureCard));
    auto* hotkeyRow = new QHBoxLayout();
    hotkeyRow->addWidget(new QLabel(tr("全局快捷键"), captureCard));
    hotkeyRow->addStretch();
    captureHotkeyEdit_ = new QKeySequenceEdit(captureCard);
    captureHotkeyEdit_->setMaximumSequenceLength(1);
    captureHotkeyEdit_->setAccessibleName(tr("截图全局快捷键"));
    hotkeyRow->addWidget(captureHotkeyEdit_);
    captureLayout->addLayout(hotkeyRow);
    hotkeyStatusLabel_ = new QLabel(captureCard);
    hotkeyStatusLabel_->setWordWrap(true);
    hotkeyStatusLabel_->setObjectName(QStringLiteral("SecondaryLabel"));
    captureLayout->addWidget(hotkeyStatusLabel_);
    auto* captureNowButton = new QPushButton(tr("立即截图"), captureCard);
    captureNowButton->setIcon(glyphIcon(Glyph::Capture, iconColor));
    captureNowButton->setAccessibleName(tr("立即开始截图"));
    captureLayout->addWidget(captureNowButton, 0, Qt::AlignRight);
    generalLayout->addWidget(captureCard);
    generalLayout->addStretch(1);
    generalScroll->setWidget(generalPage);
    pages_->addWidget(generalScroll);

    auto* servicePage = new QWidget(pages_);
    auto* serviceLayout = new QVBoxLayout(servicePage);
    serviceLayout->setContentsMargins(2, 2, 2, 2);
    providerSettingsWidget_ = new ProviderSettingsWidget(servicePage);
    serviceLayout->addWidget(providerSettingsWidget_);
    pages_->addWidget(servicePage);

    auto* privacyPage = new QWidget(pages_);
    auto* privacyLayout = new QVBoxLayout(privacyPage);
    privacyLayout->setContentsMargins(2, 2, 8, 2);
    privacyLayout->setSpacing(14);
    auto* privacyCard = makeGlassCard(privacyPage);
    auto* privacyCardLayout = new QVBoxLayout(privacyCard);
    privacyCardLayout->setContentsMargins(18, 16, 18, 16);
    privacyCardLayout->setSpacing(9);
    privacyCardLayout->addWidget(makeSectionTitle(tr("隐私边界"), privacyCard));
    privacyCardLayout->addWidget(makeDescription(
        tr("截图在本机内存中编辑；未明确发送前不会上传。保存、复制、贴图和 AI 发送共用同一份快照渲染结果。API Key 仅保存在 Windows 凭据管理器。"),
        privacyCard));
    privacyLayout->addWidget(privacyCard);

    auto* aboutCard = makeGlassCard(privacyPage);
    auto* aboutLayout = new QVBoxLayout(aboutCard);
    aboutLayout->setContentsMargins(18, 16, 18, 16);
    aboutLayout->setSpacing(8);
    aboutLayout->addWidget(makeSectionTitle(tr("关于 SnapAsk"), aboutCard));
    aboutLayout->addWidget(makeDescription(
        tr("版本 %1 · Windows 10/11 x64").arg(
            QStringLiteral(SNAPASK_VERSION_STRING)),
        aboutCard));
    privacyLayout->addWidget(aboutCard);
    privacyLayout->addStretch(1);
    pages_->addWidget(privacyPage);

    loadSettings();

    sidebar_->setCurrentRow(0);
    connect(closeButton, &QToolButton::clicked, this, &QDialog::close);
    connect(sidebar_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= pages_->count()) {
            return;
        }
        pages_->setCurrentIndex(row);
        pageTitleLabel_->setText(sidebar_->item(row)->data(Qt::UserRole).toString());
    });
    connect(themeModeCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const ThemeMode mode = ThemeTokens::fromStorage(themeModeCombo_->itemData(index).toString());
        saveThemeMode(mode);
        emit themeModeChanged(mode);
    });
    connect(captureHotkeyEdit_, &QKeySequenceEdit::editingFinished,
            this, &SettingsDialog::saveCaptureHotkey);
    connect(captureNowButton, &QPushButton::clicked,
            this, &SettingsDialog::captureNowRequested);
}

ProviderSettingsWidget* SettingsDialog::providerSettingsWidget() const noexcept {
    return providerSettingsWidget_;
}

void SettingsDialog::setHotkeyStatus(bool registered, const QString& message) {
    if (hotkeyStatusLabel_ == nullptr) {
        return;
    }
    if (registered) {
        hotkeyStatusLabel_->setText(tr("快捷键已注册，可在任意应用中使用。"));
        hotkeyStatusLabel_->setStyleSheet(QStringLiteral("color: #2aa745;"));
    } else {
        hotkeyStatusLabel_->setText(message.isEmpty()
            ? tr("快捷键注册失败，可能与其他应用冲突。") : message);
        hotkeyStatusLabel_->setStyleSheet(QStringLiteral("color: #d95050;"));
    }
}

void SettingsDialog::loadSettings() {
    QSettings settings;
    const ThemeMode savedMode = ThemeTokens::fromStorage(
        settings.value(QString::fromLatin1(kThemeSettingsKey), QStringLiteral("system")).toString());
    const QString storageValue = ThemeTokens::toStorage(savedMode);
    const int index = themeModeCombo_->findData(storageValue);
    themeModeCombo_->setCurrentIndex(index >= 0 ? index : 0);
    const auto hotkey = settings.value(
        QString::fromLatin1(kCaptureHotkeySettingsKey),
        QString::fromLatin1(kDefaultCaptureHotkey)).toString();
    captureHotkeyEdit_->setKeySequence(
        QKeySequence::fromString(hotkey, QKeySequence::PortableText));
}

void SettingsDialog::saveThemeMode(ThemeMode mode) {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kThemeSettingsKey), ThemeTokens::toStorage(mode));
    settings.sync();
}

void SettingsDialog::saveCaptureHotkey() {
    const auto sequence = captureHotkeyEdit_->keySequence();
    QSettings settings;
    settings.setValue(
        QString::fromLatin1(kCaptureHotkeySettingsKey),
        sequence.toString(QKeySequence::PortableText));
    settings.sync();
    emit captureHotkeyChanged(sequence);
}

}  // namespace snapask::ui
