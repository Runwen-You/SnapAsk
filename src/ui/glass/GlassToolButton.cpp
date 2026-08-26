#include "ui/glass/GlassToolButton.h"

#include "ui/common/ThemeTokens.h"
#include "ui/glass/GlassMaterial.h"
#include "ui/glass/GlassPainter.h"

#include <QEnterEvent>
#include <QEvent>
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPropertyAnimation>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QStyleOptionToolButton>

#include <algorithm>

namespace snapask::ui::glass {
namespace {

[[nodiscard]] snapask::ui::ThemeMode modeForPalette(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() >= 128
        ? snapask::ui::ThemeMode::Dark
        : snapask::ui::ThemeMode::Light;
}

[[nodiscard]] QColor mixColor(
    const QColor& first,
    const QColor& second,
    const qreal secondAmount)
{
    const qreal amount = std::clamp(secondAmount, 0.0, 1.0);
    return QColor::fromRgbF(
        (first.redF() * (1.0 - amount)) + (second.redF() * amount),
        (first.greenF() * (1.0 - amount)) + (second.greenF() * amount),
        (first.blueF() * (1.0 - amount)) + (second.blueF() * amount),
        (first.alphaF() * (1.0 - amount)) + (second.alphaF() * amount));
}

}  // namespace

GlassToolButton::GlassToolButton(QWidget* parent)
    : QToolButton(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setIconSize(QSize(19, 19));
    setMinimumSize(36, 36);

    hoverAnimation_ = new QPropertyAnimation(this, "hoverProgress", this);
    hoverAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    pressAnimation_ = new QPropertyAnimation(this, "pressProgress", this);
    pressAnimation_->setEasingCurve(QEasingCurve::OutQuart);
}

qreal GlassToolButton::hoverProgress() const noexcept
{
    return hoverProgress_;
}

void GlassToolButton::setHoverProgress(const qreal progress)
{
    const qreal bounded = std::clamp(progress, 0.0, 1.0);
    if (qFuzzyCompare(hoverProgress_, bounded)) {
        return;
    }
    hoverProgress_ = bounded;
    update();
}

qreal GlassToolButton::pressProgress() const noexcept
{
    return pressProgress_;
}

void GlassToolButton::setPressProgress(const qreal progress)
{
    const qreal bounded = std::clamp(progress, 0.0, 1.0);
    if (qFuzzyCompare(pressProgress_, bounded)) {
        return;
    }
    pressProgress_ = bounded;
    update();
}

QSize GlassToolButton::sizeHint() const
{
    return QToolButton::sizeHint().expandedTo(QSize(36, 36));
}

void GlassToolButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QStyleOptionToolButton option;
    initStyleOption(&option);
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    GlassMaterial material = materialFor(tokens, GlassMaterialRole::Control);
    const bool selected = isCheckable() && isChecked();
    const qreal enabledStrength = isEnabled() ? 1.0 : 0.3;
    const qreal interaction = std::max(hoverProgress_, pressProgress_);

    material.radius = std::max(0.0, (height() - 5.0) * 0.5);
    material.opacity = enabledStrength
        * (selected ? 0.92 : 0.08 + (0.5 * interaction));
    material.highlightStrength =
        enabledStrength * (selected ? 1.14 : 0.6 + (0.36 * hoverProgress_));
    material.shadowStrength =
        enabledStrength * (selected ? 0.22 : 0.04 + (0.08 * hoverProgress_));
    if (selected) {
        material.tint = mixColor(
            tokens.glassControlFill,
            tokens.accent,
            tokens.dark ? 0.2 : 0.14);
        material.hover = mixColor(tokens.glassHover, tokens.accent, 0.14);
    }

    QPainter painter(this);
    const QRectF buttonRect = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);
    GlassPainter::paintSurface(
        painter,
        buttonRect,
        material,
        hoverProgress_,
        pressProgress_,
        mapFromGlobal(QCursor::pos()));

    painter.save();
    if (pressProgress_ > 0.35) {
        painter.translate(0.0, std::min(1.0, pressProgress_));
    }
    option.state &= ~QStyle::State_MouseOver;
    option.state &= ~QStyle::State_Sunken;
    style()->drawControl(QStyle::CE_ToolButtonLabel, &option, &painter, this);
    painter.restore();

    if (hasFocus()) {
        QStyleOptionFocusRect focus;
        focus.initFrom(this);
        focus.rect = rect().adjusted(4, 4, -4, -4);
        focus.backgroundColor = Qt::transparent;
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
}

void GlassToolButton::enterEvent(QEnterEvent* event)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    animateHover(1.0, tokens.animationFastMs);
    QToolButton::enterEvent(event);
}

void GlassToolButton::leaveEvent(QEvent* event)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    animateHover(0.0, tokens.animationNormalMs);
    animatePress(0.0, tokens.animationFastMs);
    QToolButton::leaveEvent(event);
}

void GlassToolButton::mousePressEvent(QMouseEvent* event)
{
    if (isEnabled() && event->button() == Qt::LeftButton) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        animatePress(1.0, tokens.animationFastMs);
    }
    QToolButton::mousePressEvent(event);
}

void GlassToolButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        animatePress(0.0, tokens.animationNormalMs);
    }
    QToolButton::mouseReleaseEvent(event);
}

void GlassToolButton::changeEvent(QEvent* event)
{
    QToolButton::changeEvent(event);
    update();
}

void GlassToolButton::animateHover(const qreal target, const int durationMs)
{
    hoverAnimation_->stop();
    hoverAnimation_->setStartValue(hoverProgress_);
    hoverAnimation_->setEndValue(target);
    hoverAnimation_->setDuration(durationMs);
    hoverAnimation_->start();
}

void GlassToolButton::animatePress(const qreal target, const int durationMs)
{
    pressAnimation_->stop();
    pressAnimation_->setStartValue(pressProgress_);
    pressAnimation_->setEndValue(target);
    pressAnimation_->setDuration(durationMs);
    pressAnimation_->start();
}

}  // namespace snapask::ui::glass
