#include "domain/annotation/Annotation.h"

#include <QLineF>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace snapask {
namespace {

qreal distanceToSegment(const QPointF& point,
                        const QPointF& start,
                        const QPointF& end) {
    const QPointF segment = end - start;
    const qreal lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 0.0) {
        return QLineF(point, start).length();
    }

    const qreal projection = std::clamp(
        QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
    const QPointF closest = start + segment * projection;
    return QLineF(point, closest).length();
}

QRectF normalizedRect(const QRectF& rect) {
    return rect.normalized();
}

bool styleIsValid(const AnnotationStyle& style) {
    return style.strokeColor.isValid() && style.fillColor.isValid() &&
           std::isfinite(style.strokeWidth) && style.strokeWidth > 0.0 &&
           style.mosaicBlockSize >= 2;
}

}  // namespace

AnnotationStyle::AnnotationStyle() {
    font.setFamily(QStringLiteral("Segoe UI"));
    font.setPixelSize(18);
}

Annotation Annotation::makeRectangle(const QRectF& rect,
                                     const AnnotationStyle& style,
                                     int zOrder,
                                     const QUuid& id) {
    Annotation annotation;
    annotation.id = id;
    annotation.type = AnnotationType::Rectangle;
    annotation.zOrder = zOrder;
    annotation.style = style;
    annotation.geometry = RectangleGeometry{normalizedRect(rect)};
    return annotation;
}

Annotation Annotation::makeArrow(const QPointF& start,
                                 const QPointF& end,
                                 const AnnotationStyle& style,
                                 int zOrder,
                                 const QUuid& id) {
    Annotation annotation;
    annotation.id = id;
    annotation.type = AnnotationType::Arrow;
    annotation.zOrder = zOrder;
    annotation.style = style;
    annotation.geometry = ArrowGeometry{start, end};
    return annotation;
}

Annotation Annotation::makeText(const QRectF& rect,
                                QString text,
                                const AnnotationStyle& style,
                                int zOrder,
                                const QUuid& id) {
    Annotation annotation;
    annotation.id = id;
    annotation.type = AnnotationType::Text;
    annotation.zOrder = zOrder;
    annotation.style = style;
    annotation.geometry = TextGeometry{normalizedRect(rect), std::move(text)};
    return annotation;
}

Annotation Annotation::makeMosaic(QVector<QPointF> points,
                                  qreal brushWidth,
                                  const AnnotationStyle& style,
                                  int zOrder,
                                  const QUuid& id) {
    Annotation annotation;
    annotation.id = id;
    annotation.type = AnnotationType::Mosaic;
    annotation.zOrder = zOrder;
    annotation.style = style;
    annotation.geometry = MosaicGeometry{std::move(points), brushWidth};
    return annotation;
}

bool Annotation::isValid() const {
    if (id.isNull() || !styleIsValid(style) ||
        !geometryMatchesType(type, geometry)) {
        return false;
    }

    return std::visit(
        [](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, RectangleGeometry>) {
                return value.rect.isValid() && !value.rect.isEmpty();
            } else if constexpr (std::is_same_v<Value, ArrowGeometry>) {
                return value.start != value.end;
            } else if constexpr (std::is_same_v<Value, TextGeometry>) {
                return value.rect.isValid() && !value.rect.isEmpty();
            } else {
                return !value.points.isEmpty() &&
                       std::isfinite(value.brushWidth) && value.brushWidth > 0.0;
            }
        },
        geometry);
}

QRectF Annotation::bounds() const {
    return std::visit(
        [this](const auto& value) -> QRectF {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, RectangleGeometry> ||
                          std::is_same_v<Value, TextGeometry>) {
                const qreal margin = style.strokeWidth * 0.5;
                return value.rect.normalized().adjusted(-margin, -margin, margin,
                                                        margin);
            } else if constexpr (std::is_same_v<Value, ArrowGeometry>) {
                QRectF result(value.start, value.end);
                const qreal arrowMargin =
                    std::max<qreal>(8.0, style.strokeWidth * 4.0);
                return result.normalized().adjusted(-arrowMargin, -arrowMargin,
                                                    arrowMargin, arrowMargin);
            } else {
                if (value.points.isEmpty()) {
                    return {};
                }
                qreal left = value.points.front().x();
                qreal right = left;
                qreal top = value.points.front().y();
                qreal bottom = top;
                for (const QPointF& point : value.points) {
                    left = std::min(left, point.x());
                    right = std::max(right, point.x());
                    top = std::min(top, point.y());
                    bottom = std::max(bottom, point.y());
                }
                const qreal radius = value.brushWidth * 0.5;
                return QRectF(QPointF(left, top), QPointF(right, bottom))
                    .normalized()
                    .adjusted(-radius, -radius, radius, radius);
            }
        },
        geometry);
}

bool Annotation::hitTest(const QPointF& imagePoint, qreal tolerance) const {
    tolerance = std::max<qreal>(0.0, tolerance);
    return std::visit(
        [this, imagePoint, tolerance](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, RectangleGeometry>) {
                const QRectF outer = value.rect.normalized().adjusted(
                    -tolerance, -tolerance, tolerance, tolerance);
                return outer.contains(imagePoint);
            } else if constexpr (std::is_same_v<Value, TextGeometry>) {
                return value.rect.normalized().adjusted(-tolerance, -tolerance,
                                                        tolerance, tolerance)
                    .contains(imagePoint);
            } else if constexpr (std::is_same_v<Value, ArrowGeometry>) {
                return distanceToSegment(imagePoint, value.start, value.end) <=
                       tolerance + style.strokeWidth * 0.5;
            } else {
                if (value.points.size() == 1) {
                    return QLineF(imagePoint, value.points.front()).length() <=
                           value.brushWidth * 0.5 + tolerance;
                }
                for (qsizetype index = 1; index < value.points.size(); ++index) {
                    if (distanceToSegment(imagePoint, value.points[index - 1],
                                          value.points[index]) <=
                        value.brushWidth * 0.5 + tolerance) {
                        return true;
                    }
                }
                return false;
            }
        },
        geometry);
}

bool geometryMatchesType(AnnotationType type,
                         const AnnotationGeometry& geometry) {
    switch (type) {
        case AnnotationType::Rectangle:
            return std::holds_alternative<RectangleGeometry>(geometry);
        case AnnotationType::Arrow:
            return std::holds_alternative<ArrowGeometry>(geometry);
        case AnnotationType::Text:
            return std::holds_alternative<TextGeometry>(geometry);
        case AnnotationType::Mosaic:
            return std::holds_alternative<MosaicGeometry>(geometry);
    }
    return false;
}

AnnotationGeometry translatedGeometry(const AnnotationGeometry& geometry,
                                      const QPointF& delta) {
    return std::visit(
        [delta](const auto& value) -> AnnotationGeometry {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, RectangleGeometry>) {
                return RectangleGeometry{value.rect.translated(delta)};
            } else if constexpr (std::is_same_v<Value, ArrowGeometry>) {
                return ArrowGeometry{value.start + delta, value.end + delta};
            } else if constexpr (std::is_same_v<Value, TextGeometry>) {
                return TextGeometry{value.rect.translated(delta), value.text};
            } else {
                MosaicGeometry translated = value;
                for (QPointF& point : translated.points) {
                    point += delta;
                }
                return translated;
            }
        },
        geometry);
}

}  // namespace snapask
