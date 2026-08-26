#include "ui/glass/GlassPainter.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>

namespace snapask::ui::glass {
namespace {

[[nodiscard]] QColor scaledAlpha(QColor color, const qreal scale)
{
    color.setAlphaF(static_cast<float>(
        std::clamp(color.alphaF() * scale, 0.0, 1.0)));
    return color;
}

[[nodiscard]] QPointF effectivePointer(
    const QRectF& rect,
    const QPointF& pointerPosition)
{
    if (rect.contains(pointerPosition)) {
        return pointerPosition;
    }
    return {
        rect.left() + (rect.width() * 0.28),
        rect.top() + (rect.height() * 0.12),
    };
}

void paintSoftShadow(
    QPainter& painter,
    const QRectF& rect,
    const GlassMaterial& material)
{
    if (material.shadowStrength <= 0.0 || material.shadow.alpha() <= 0) {
        return;
    }

    constexpr qreal spread[] = {5.0, 3.0, 1.5};
    constexpr qreal alpha[] = {0.16, 0.22, 0.3};
    for (int index = 0; index < 3; ++index) {
        const qreal amount = spread[index];
        QRectF shadowRect = rect.adjusted(-amount, -amount, amount, amount);
        shadowRect.translate(0.0, 1.5 + (amount * 0.32));
        const QPainterPath shadowPath = GlassPainter::surfacePath(
            shadowRect,
            material.radius + amount);
        painter.fillPath(
            shadowPath,
            scaledAlpha(
                material.shadow,
                material.shadowStrength * alpha[index]));
    }
}

}  // namespace

QPainterPath GlassPainter::surfacePath(const QRectF& rect, const qreal radius)
{
    QPainterPath path;
    const qreal boundedRadius = std::clamp(
        radius,
        0.0,
        std::max(0.0, std::min(rect.width(), rect.height()) * 0.5));
    path.addRoundedRect(rect, boundedRadius, boundedRadius);
    return path;
}

void GlassPainter::paintSurface(
    QPainter& painter,
    const QRectF& rect,
    const GlassMaterial& material,
    const qreal hoverProgress,
    const qreal pressProgress,
    const QPointF pointerPosition)
{
    if (!rect.isValid() || rect.isEmpty()) {
        return;
    }

    const qreal hover = std::clamp(hoverProgress, 0.0, 1.0);
    const qreal press = std::clamp(pressProgress, 0.0, 1.0);
    const qreal density = std::max(0.0, material.materialDensity);
    const QPainterPath path = surfacePath(rect, material.radius);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintSoftShadow(painter, rect, material);

    painter.setClipPath(path);
    painter.fillPath(
        path,
        scaledAlpha(material.tint, material.opacity * density));

    QLinearGradient materialGradient(rect.topLeft(), rect.bottomLeft());
    QColor materialTop = material.innerHighlight;
    materialTop.setAlphaF(static_cast<float>(
        0.22 * material.highlightStrength));
    QColor materialMiddle = material.tint;
    materialMiddle.setAlphaF(0.02F);
    QColor materialBottom = material.edgeDim;
    materialBottom.setAlphaF(static_cast<float>(0.12 * density));
    materialGradient.setColorAt(0.0, materialTop);
    materialGradient.setColorAt(0.46, materialMiddle);
    materialGradient.setColorAt(1.0, materialBottom);
    painter.fillPath(path, materialGradient);

    const QPointF pointer = effectivePointer(rect, pointerPosition);
    const qreal specularRadius = std::max(rect.width(), rect.height()) * 0.72;
    QRadialGradient specular(pointer, std::max(1.0, specularRadius));
    specular.setColorAt(
        0.0,
        scaledAlpha(
            material.specular,
            (0.2 + (0.24 * hover)) * material.highlightStrength));
    specular.setColorAt(
        0.28,
        scaledAlpha(
            material.specular,
            (0.08 + (0.1 * hover)) * material.highlightStrength));
    specular.setColorAt(1.0, scaledAlpha(material.specular, 0.0));
    painter.fillPath(path, specular);

    if (hover > 0.0) {
        QRadialGradient hoverGlow(pointer, std::max(1.0, specularRadius * 0.7));
        hoverGlow.setColorAt(0.0, scaledAlpha(material.hover, 0.52 * hover));
        hoverGlow.setColorAt(1.0, scaledAlpha(material.hover, 0.0));
        painter.fillPath(path, hoverGlow);
    }
    if (press > 0.0) {
        QLinearGradient pressShade(rect.topLeft(), rect.bottomLeft());
        pressShade.setColorAt(0.0, scaledAlpha(material.pressed, 0.18 * press));
        pressShade.setColorAt(1.0, scaledAlpha(material.pressed, 0.64 * press));
        painter.fillPath(path, pressShade);
    }

    painter.setClipping(false);

    const QRectF innerRect = rect.adjusted(1.1, 1.1, -1.1, -1.1);
    if (innerRect.isValid()) {
        QLinearGradient innerGradient(innerRect.topLeft(), innerRect.bottomRight());
        innerGradient.setColorAt(
            0.0,
            scaledAlpha(
                material.innerHighlight,
                (0.62 + (0.18 * hover) - (0.22 * press))
                    * material.highlightStrength));
        innerGradient.setColorAt(
            0.48,
            scaledAlpha(material.innerHighlight, 0.06));
        innerGradient.setColorAt(1.0, scaledAlpha(material.edgeDim, 0.08));
        QPen innerPen(innerGradient, 1.0);
        innerPen.setCosmetic(true);
        painter.setPen(innerPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(surfacePath(
            innerRect,
            std::max(0.0, material.radius - 1.1)));
    }

    QLinearGradient edgeGradient(rect.topLeft(), rect.bottomRight());
    edgeGradient.setColorAt(
        0.0,
        scaledAlpha(
            material.edgeBright,
            0.82 + (0.16 * hover) - (0.1 * press)));
    edgeGradient.setColorAt(0.42, scaledAlpha(material.edgeBright, 0.32));
    edgeGradient.setColorAt(0.64, scaledAlpha(material.edgeDim, 0.34));
    edgeGradient.setColorAt(1.0, scaledAlpha(material.edgeDim, 0.86));
    QPen edgePen(edgeGradient, 1.0);
    edgePen.setCosmetic(true);
    painter.setPen(edgePen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);

    painter.restore();
}

}  // namespace snapask::ui::glass
