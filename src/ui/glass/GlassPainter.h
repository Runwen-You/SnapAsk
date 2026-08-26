#pragma once

#include "ui/glass/GlassMaterial.h"

#include <QPointF>
#include <QRectF>

class QPainter;
class QPainterPath;

namespace snapask::ui::glass {

class GlassPainter final {
public:
    GlassPainter() = delete;

    static void paintSurface(
        QPainter& painter,
        const QRectF& rect,
        const GlassMaterial& material,
        qreal hoverProgress = 0.0,
        qreal pressProgress = 0.0,
        QPointF pointerPosition = {});

    [[nodiscard]] static QPainterPath surfacePath(
        const QRectF& rect,
        qreal radius);
};

}  // namespace snapask::ui::glass
