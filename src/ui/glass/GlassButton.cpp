#include "ui/glass/GlassButton.h"

#include "ui/common/ThemeTokens.h"
#include "ui/glass/GlassMaterial.h"
#include "ui/glass/GlassPainter.h"

#include <QCursor>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPropertyAnimation>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionFocusRect>

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

GlassButton::GlassButton(QWidget* parent)
    : QPushButton(parent)
{
    initialize();
}

GlassButton::GlassButton(const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    initialize();
}

void GlassButton::initialize()
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    hoverAnimation_ = new QPropertyAnimation(this, "hoverProgress", this);
    hoverAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    pressAnimation_ = new QPropertyAnimation(this, "pressProgress", this);
    pressAnimation_->setEasingCurve(QEasingCurve::OutQuart);
}

void GlassButton::setAccent(const bool accent)
{
    if (accent_ == accent) {
        return;
    }
    accent_ = accent;
    update();
}

bool GlassButton::isAccent() const noexcept
{
    return accent_;
}

void GlassButton::setDanger(const bool danger)
{
    if (danger_ == danger) {
        return;
    }
    danger_ = danger;
    update();
}

bool GlassButton::isDanger() const noexcept
{
    return danger_;
}

qreal GlassButton::hoverProgress() const noexcept
{
    return hoverProgress_;
}

void GlassButton::setHoverProgress(const qreal progress)
{
    const qreal bounded = std::clamp(progress, 0.0, 1.0);
    if (qFuzzyCompare(hoverProgress_, bounded)) {
        return;
    }
    hoverProgress_ = bounded;
    update();
}

qreal GlassButton::pressProgress() const noexcept
{
    return pressProgress_;
}

void GlassButton::setPressProgress(const qreal progress)
{
    const qreal bounded = std::clamp(progress, 0.0, 1.0);
    if (qFuzzyCompare(pressProgress_, bounded)) {
        return;
    }
    pressProgress_ = bounded;
    update();
}

void GlassButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QStyleOptionButton option;
    initStyleOption(&option);
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    GlassMaterial material = materialFor(tokens, GlassMaterialRole::Control);
    const qreal enabledStrength = isEnabled() ? 1.0 : 0.3;
    material.radius = std::max(0.0, (height() - 4.0) * 0.5);
    material.opacity = enabledStrength
        * (0.28 + (0.32 * hoverProgress_) + (0.12 * pressProgress_));
    material.highlightStrength =
        enabledStrength * (0.68 + (0.34 * hoverProgress_));
    material.shadowStrength =
        enabledStrength * (0.08 + (0.12 * hoverProgress_));
    if (accent_ || isDefault()) {
        material.tint = mixColor(
            tokens.glassControlFill,
            tokens.accent,
            tokens.dark ? 0.34 : 0.26);
        material.opacity = enabledStrength
            * (0.82 + (0.12 * hoverProgress_));
    } else if (danger_) {
        material.tint = mixColor(
            tokens.glassControlFill,
            tokens.danger,
            tokens.dark ? 0.28 : 0.2);
    }

    QPainter painter(this);
    GlassPainter::paintSurface(
        painter,
        QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
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
    style()->drawControl(QStyle::CE_PushButtonLabel, &option, &painter, this);
    painter.restore();

    if (hasFocus()) {
        QStyleOptionFocusRect focus;
        focus.initFrom(this);
        focus.rect = rect().adjusted(4, 4, -4, -4);
        focus.backgroundColor = Qt::transparent;
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
}

void GlassButton::enterEvent(QEnterEvent* event)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    animateHover(1.0, tokens.animationFastMs);
    QPushButton::enterEvent(event);
}

void GlassButton::leaveEvent(QEvent* event)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    animateHover(0.0, tokens.animationNormalMs);
    animatePress(0.0, tokens.animationFastMs);
    QPushButton::leaveEvent(event);
}

void GlassButton::mousePressEvent(QMouseEvent* event)
{
    if (isEnabled() && event->button() == Qt::LeftButton) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        animatePress(1.0, tokens.animationFastMs);
    }
    QPushButton::mousePressEvent(event);
}

void GlassButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        animatePress(0.0, tokens.animationNormalMs);
    }
    QPushButton::mouseReleaseEvent(event);
}

void GlassButton::animateHover(const qreal target, const int durationMs)
{
    hoverAnimation_->stop();
    hoverAnimation_->setStartValue(hoverProgress_);
    hoverAnimation_->setEndValue(target);
    hoverAnimation_->setDuration(durationMs);
    hoverAnimation_->start();
}

void GlassButton::animatePress(const qreal target, const int durationMs)
{
    pressAnimation_->stop();
    pressAnimation_->setStartValue(pressProgress_);
    pressAnimation_->setEndValue(target);
    pressAnimation_->setDuration(durationMs);
    pressAnimation_->start();
}

}  // namespace snapask::ui::glass
