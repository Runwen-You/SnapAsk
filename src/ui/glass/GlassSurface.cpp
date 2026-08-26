#include "ui/glass/GlassSurface.h"

#include "ui/common/ThemeTokens.h"
#include "ui/glass/GlassPainter.h"

#include <QEnterEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPropertyAnimation>

#include <algorithm>

namespace snapask::ui::glass {
namespace {

[[nodiscard]] snapask::ui::ThemeMode modeForPalette(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() >= 128
        ? snapask::ui::ThemeMode::Dark
        : snapask::ui::ThemeMode::Light;
}

}  // namespace

GlassSurface::GlassSurface(QWidget* parent, const Qt::WindowFlags flags)
    : QWidget(parent, flags)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_StyledBackground, false);
    setMouseTracking(true);

    hoverAnimation_ = new QPropertyAnimation(this, "hoverProgress", this);
    hoverAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    pressAnimation_ = new QPropertyAnimation(this, "pressProgress", this);
    pressAnimation_->setEasingCurve(QEasingCurve::OutQuart);
    pointerAnimation_ = new QPropertyAnimation(this, "pointerPosition", this);
    pointerAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    refreshMaterial();
}

GlassSurface::~GlassSurface()
{
    hoverAnimation_->stop();
    pressAnimation_->stop();
    pointerAnimation_->stop();
}

GlassMaterial GlassSurface::material() const
{
    return material_;
}

void GlassSurface::setMaterial(const GlassMaterial& material)
{
    material_ = material;
    followsTheme_ = false;
    update();
}

void GlassSurface::resetMaterialFromTheme()
{
    followsTheme_ = true;
    refreshMaterial();
}

GlassMaterialRole GlassSurface::materialRole() const noexcept
{
    return materialRole_;
}

void GlassSurface::setMaterialRole(const GlassMaterialRole role)
{
    if (materialRole_ == role) {
        return;
    }
    materialRole_ = role;
    followsTheme_ = true;
    refreshMaterial();
}

GlassBackdropMode GlassSurface::backdropMode() const noexcept
{
    return backdropMode_;
}

void GlassSurface::setBackdropMode(const GlassBackdropMode mode)
{
    if (backdropMode_ == mode) {
        return;
    }
    backdropMode_ = mode;
    update();
}

void GlassSurface::setBackdropImage(
    const QImage& source,
    const QRect& sourceRect,
    const quint64 sourceRevision)
{
    const QRect effectiveRect = sourceRect.isValid() && !sourceRect.isEmpty()
        ? sourceRect
        : source.rect();
    if (backdropSource_.cacheKey() == source.cacheKey()
        && backdropSourceRect_ == effectiveRect
        && backdropSourceRevision_ == sourceRevision) {
        return;
    }
    backdropSource_ = source;
    backdropSourceRect_ = effectiveRect;
    backdropSourceRevision_ = sourceRevision;
    backdropCache_.invalidate();
    update();
}

void GlassSurface::clearBackdropImage()
{
    backdropSource_ = {};
    backdropSourceRect_ = {};
    backdropSourceRevision_ = 0;
    backdropCache_.invalidate();
    update();
}

bool GlassSurface::hasBackdropImage() const noexcept
{
    return !backdropSource_.isNull();
}

quint64 GlassSurface::backdropGenerationCount() const noexcept
{
    return backdropCache_.generationCount();
}

void GlassSurface::setFallbackEnabled(const bool enabled)
{
    if (fallbackEnabled_ == enabled) {
        return;
    }
    fallbackEnabled_ = enabled;
    update();
}

bool GlassSurface::fallbackEnabled() const noexcept
{
    return fallbackEnabled_;
}

qreal GlassSurface::hoverProgress() const noexcept
{
    return hoverProgress_;
}

void GlassSurface::setHoverProgress(const qreal progress)
{
    const qreal bounded = std::clamp(progress, 0.0, 1.0);
    if (qFuzzyCompare(hoverProgress_, bounded)) {
        return;
    }
    hoverProgress_ = bounded;
    update();
}

qreal GlassSurface::pressProgress() const noexcept
{
    return pressProgress_;
}

void GlassSurface::setPressProgress(const qreal progress)
{
    const qreal bounded = std::clamp(progress, 0.0, 1.0);
    if (qFuzzyCompare(pressProgress_, bounded)) {
        return;
    }
    pressProgress_ = bounded;
    update();
}

QPointF GlassSurface::pointerPosition() const noexcept
{
    return pointerPosition_;
}

void GlassSurface::setPointerPosition(const QPointF& position)
{
    if (pointerPosition_ == position) {
        return;
    }
    pointerPosition_ = position;
    update();
}

void GlassSurface::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    const QRectF surface = glassRect();
    const GlassMaterial material = effectiveMaterial();
    if (backdropMode_ == GlassBackdropMode::Image
        && !fallbackEnabled_
        && !backdropSource_.isNull()) {
        const bool dark = modeForPalette(palette())
            == snapask::ui::ThemeMode::Dark;
        const QImage& backdrop = backdropCache_.imageFor(
            backdropSource_,
            backdropSourceRect_,
            surface.size().toSize(),
            devicePixelRatioF(),
            dark,
            12,
            backdropSourceRevision_);
        if (!backdrop.isNull()) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setClipPath(
                GlassPainter::surfacePath(surface, material.radius));
            painter.drawImage(surface, backdrop);
            painter.restore();
        }
    }
    GlassPainter::paintSurface(
        painter,
        surface,
        material,
        hoverProgress_,
        pressProgress_,
        pointerPosition_);
}

void GlassSurface::enterEvent(QEnterEvent* event)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    animateHover(1.0, tokens.animationFastMs);
    setPointerPosition(event->position());
    QWidget::enterEvent(event);
}

void GlassSurface::leaveEvent(QEvent* event)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    animateHover(0.0, tokens.animationNormalMs);
    animatePress(0.0, tokens.animationFastMs);
    animatePointerHome();
    QWidget::leaveEvent(event);
}

void GlassSurface::mouseMoveEvent(QMouseEvent* event)
{
    pointerAnimation_->stop();
    setPointerPosition(event->position());
    QWidget::mouseMoveEvent(event);
}

void GlassSurface::mousePressEvent(QMouseEvent* event)
{
    if (isEnabled() && event->button() == Qt::LeftButton) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        animatePress(1.0, tokens.animationFastMs);
        setPointerPosition(event->position());
    }
    QWidget::mousePressEvent(event);
}

void GlassSurface::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        animatePress(0.0, tokens.animationNormalMs);
        setPointerPosition(event->position());
    }
    QWidget::mouseReleaseEvent(event);
}

void GlassSurface::changeEvent(QEvent* event)
{
    if (event != nullptr
        && (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::ApplicationPaletteChange
            || event->type() == QEvent::ThemeChange)) {
        refreshMaterial();
    }
    QWidget::changeEvent(event);
}

QRectF GlassSurface::glassRect() const
{
    return QRectF(rect()).adjusted(6.0, 5.0, -6.0, -7.0);
}

GlassMaterial GlassSurface::effectiveMaterial() const
{
    GlassMaterial result = material_;
    if (backdropMode_ == GlassBackdropMode::SolidFallback
        || fallbackEnabled_) {
        const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
        result.tint = tokens.elevatedSurface;
        result.tint.setAlpha(255);
        result.opacity = 1.0;
        result.highlightStrength *= 0.32;
        result.shadowStrength *= 0.45;
    }
    return result;
}

void GlassSurface::refreshMaterial()
{
    if (!followsTheme_) {
        update();
        return;
    }
    material_ = materialFor(
        snapask::ui::ThemeTokens::resolve(modeForPalette(palette())),
        materialRole_);
    update();
}

void GlassSurface::animateHover(const qreal target, const int durationMs)
{
    hoverAnimation_->stop();
    hoverAnimation_->setStartValue(hoverProgress_);
    hoverAnimation_->setEndValue(target);
    hoverAnimation_->setDuration(durationMs);
    hoverAnimation_->start();
}

void GlassSurface::animatePress(const qreal target, const int durationMs)
{
    pressAnimation_->stop();
    pressAnimation_->setStartValue(pressProgress_);
    pressAnimation_->setEndValue(target);
    pressAnimation_->setDuration(durationMs);
    pressAnimation_->start();
}

void GlassSurface::animatePointerHome()
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(modeForPalette(palette()));
    const QRectF surface = glassRect();
    const QPointF home(
        surface.left() + (surface.width() * 0.28),
        surface.top() + (surface.height() * 0.12));
    pointerAnimation_->stop();
    pointerAnimation_->setStartValue(pointerPosition_);
    pointerAnimation_->setEndValue(home);
    pointerAnimation_->setDuration(tokens.animationSlowMs);
    pointerAnimation_->start();
}

}  // namespace snapask::ui::glass
