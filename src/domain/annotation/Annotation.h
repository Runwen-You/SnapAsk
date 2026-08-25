#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QUuid>
#include <QVector>

#include <variant>

namespace snapask {

// Annotation geometry is always expressed in source-image physical pixels.
enum class AnnotationType {
    Rectangle,
    Arrow,
    Text,
    Mosaic,
};

struct AnnotationStyle {
    QColor strokeColor{255, 59, 48};
    QColor fillColor{Qt::transparent};
    qreal strokeWidth{3.0};
    QFont font;
    int mosaicBlockSize{12};

    AnnotationStyle();

    friend bool operator==(const AnnotationStyle& lhs,
                           const AnnotationStyle& rhs) = default;
};

struct RectangleGeometry {
    QRectF rect;

    friend bool operator==(const RectangleGeometry& lhs,
                           const RectangleGeometry& rhs) = default;
};

struct ArrowGeometry {
    QPointF start;
    QPointF end;

    friend bool operator==(const ArrowGeometry& lhs,
                           const ArrowGeometry& rhs) = default;
};

struct TextGeometry {
    QRectF rect;
    QString text;

    friend bool operator==(const TextGeometry& lhs,
                           const TextGeometry& rhs) = default;
};

struct MosaicGeometry {
    QVector<QPointF> points;
    qreal brushWidth{28.0};

    friend bool operator==(const MosaicGeometry& lhs,
                           const MosaicGeometry& rhs) = default;
};

using AnnotationGeometry =
    std::variant<RectangleGeometry, ArrowGeometry, TextGeometry, MosaicGeometry>;

class Annotation final {
public:
    QUuid id;
    AnnotationType type{AnnotationType::Rectangle};
    int zOrder{0};
    AnnotationStyle style;
    AnnotationGeometry geometry{RectangleGeometry{}};

    static Annotation makeRectangle(const QRectF& rect,
                                    const AnnotationStyle& style = {},
                                    int zOrder = 0,
                                    const QUuid& id = QUuid::createUuid());
    static Annotation makeArrow(const QPointF& start,
                                const QPointF& end,
                                const AnnotationStyle& style = {},
                                int zOrder = 0,
                                const QUuid& id = QUuid::createUuid());
    static Annotation makeText(const QRectF& rect,
                               QString text,
                               const AnnotationStyle& style = {},
                               int zOrder = 0,
                               const QUuid& id = QUuid::createUuid());
    static Annotation makeMosaic(QVector<QPointF> points,
                                 qreal brushWidth,
                                 const AnnotationStyle& style = {},
                                 int zOrder = 0,
                                 const QUuid& id = QUuid::createUuid());

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QRectF bounds() const;
    [[nodiscard]] bool hitTest(const QPointF& imagePoint,
                               qreal tolerance = 4.0) const;

    friend bool operator==(const Annotation& lhs,
                           const Annotation& rhs) = default;
};

[[nodiscard]] bool geometryMatchesType(AnnotationType type,
                                       const AnnotationGeometry& geometry);
[[nodiscard]] AnnotationGeometry translatedGeometry(
    const AnnotationGeometry& geometry,
    const QPointF& delta);

}  // namespace snapask
