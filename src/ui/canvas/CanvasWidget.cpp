#include "ui/canvas/CanvasWidget.h"

#include "domain/annotation/AnnotationDocument.h"
#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"

#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QPalette>
#include <QSet>
#include <QUndoCommand>
#include <QUndoStack>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace snapask::ui::canvas {
namespace {

constexpr qreal kArrowHeadAngleDegrees = 28.0;
constexpr qreal kHandleSize = 9.0;
constexpr qreal kHitTolerance = 6.0;
constexpr qreal kMinimumWidgetDrag = 3.0;
constexpr qreal kMinimumControlExtent = 14.0;
constexpr qreal kNudgeStep = 1.0;
constexpr qreal kAcceleratedNudgeStep = 10.0;
constexpr qint64 kNudgeMergeIntervalMs = 500;

enum class ResizeHandle {
    None,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
};

enum class InteractionMode {
    None,
    CreateRectangle,
    CreateArrow,
    CreateText,
    CreateMosaic,
    Move,
    Resize,
};

QRect alignedMosaicBounds(const MosaicGeometry& geometry,
                          const QPoint& cropOrigin,
                          const QRect& imageRect) {
    if (geometry.points.isEmpty()) {
        return {};
    }

    qreal left = geometry.points.front().x();
    qreal right = left;
    qreal top = geometry.points.front().y();
    qreal bottom = top;
    for (const QPointF& point : geometry.points) {
        left = std::min(left, point.x());
        right = std::max(right, point.x());
        top = std::min(top, point.y());
        bottom = std::max(bottom, point.y());
    }
    const qreal radius = geometry.brushWidth * 0.5 + 1.0;
    const QRectF bounds = QRectF(QPointF(left, top), QPointF(right, bottom))
                              .normalized()
                              .adjusted(-radius, -radius, radius, radius);
    return bounds.translated(-cropOrigin).toAlignedRect().intersected(imageRect);
}

QColor averageBlockColor(const QImage& image, const QRect& block) {
    quint64 alphaSum = 0;
    quint64 redSum = 0;
    quint64 greenSum = 0;
    quint64 blueSum = 0;
    quint64 count = 0;
    for (int y = block.top(); y <= block.bottom(); ++y) {
        for (int x = block.left(); x <= block.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const quint64 alpha = static_cast<quint64>(color.alpha());
            alphaSum += alpha;
            redSum += static_cast<quint64>(color.red()) * alpha;
            greenSum += static_cast<quint64>(color.green()) * alpha;
            blueSum += static_cast<quint64>(color.blue()) * alpha;
            ++count;
        }
    }
    if (count == 0 || alphaSum == 0) {
        return Qt::transparent;
    }
    return QColor(static_cast<int>(redSum / alphaSum),
                  static_cast<int>(greenSum / alphaSum),
                  static_cast<int>(blueSum / alphaSum),
                  static_cast<int>(alphaSum / count));
}

QImage pixelatedRegion(const QImage& image, const QRect& target, int blockSize) {
    QImage result = image.copy(target);
    blockSize = std::max(2, blockSize);
    const int firstX = (target.left() / blockSize) * blockSize;
    const int firstY = (target.top() / blockSize) * blockSize;
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (int y = firstY; y <= target.bottom(); y += blockSize) {
        for (int x = firstX; x <= target.right(); x += blockSize) {
            const QRect block(x, y, blockSize, blockSize);
            const QRect sourceBlock = block.intersected(image.rect());
            const QRect targetBlock = block.intersected(target);
            if (!sourceBlock.isEmpty() && !targetBlock.isEmpty()) {
                painter.fillRect(targetBlock.translated(-target.topLeft()),
                                 averageBlockColor(image, sourceBlock));
            }
        }
    }
    return result;
}

void rasterizeMosaic(QImage* canvas,
                     const MosaicGeometry& geometry,
                     const AnnotationStyle& style,
                     const QPoint& cropOrigin) {
    if (canvas == nullptr || canvas->isNull() || geometry.points.isEmpty()
        || geometry.brushWidth <= 0.0) {
        return;
    }
    const QRect target = alignedMosaicBounds(geometry, cropOrigin, canvas->rect());
    if (target.isEmpty()) {
        return;
    }

    QImage masked = pixelatedRegion(*canvas, target, style.mosaicBlockSize);
    QImage mask(target.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QPainter maskPainter(&mask);
    maskPainter.setRenderHint(QPainter::Antialiasing, true);
    maskPainter.setPen(QPen(Qt::white, geometry.brushWidth, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
    const auto localPoint = [&target, &cropOrigin](const QPointF& point) {
        return point - QPointF(cropOrigin) - QPointF(target.topLeft());
    };
    if (geometry.points.size() == 1) {
        maskPainter.setPen(Qt::NoPen);
        maskPainter.setBrush(Qt::white);
        const qreal radius = geometry.brushWidth * 0.5;
        maskPainter.drawEllipse(localPoint(geometry.points.front()), radius, radius);
    } else {
        QPainterPath path(localPoint(geometry.points.front()));
        for (qsizetype index = 1; index < geometry.points.size(); ++index) {
            path.lineTo(localPoint(geometry.points[index]));
        }
        maskPainter.drawPath(path);
    }
    maskPainter.end();

    QPainter maskedPainter(&masked);
    maskedPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    maskedPainter.drawImage(QPoint(), mask);
    maskedPainter.end();
    QPainter canvasPainter(canvas);
    canvasPainter.drawImage(target.topLeft(), masked);
}

void drawArrow(QPainter* painter,
               const ArrowGeometry& arrow,
               const AnnotationStyle& style) {
    const QLineF shaft(arrow.start, arrow.end);
    if (shaft.length() <= 0.0) {
        return;
    }
    painter->setPen(QPen(style.strokeColor, style.strokeWidth, Qt::SolidLine,
                         Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(style.strokeColor);
    painter->drawLine(shaft);
    const qreal headLength = std::max<qreal>(8.0, style.strokeWidth * 4.0);
    const qreal angle = std::atan2(arrow.start.y() - arrow.end.y(),
                                   arrow.start.x() - arrow.end.x());
    const qreal spread = qDegreesToRadians(kArrowHeadAngleDegrees);
    const QPointF first = arrow.end
        + QPointF(std::cos(angle + spread) * headLength,
                  std::sin(angle + spread) * headLength);
    const QPointF second = arrow.end
        + QPointF(std::cos(angle - spread) * headLength,
                  std::sin(angle - spread) * headLength);
    painter->drawPolygon(QPolygonF{arrow.end, first, second});
}

void drawVectorAnnotation(QImage* canvas,
                          const Annotation& annotation,
                          const QPoint& cropOrigin) {
    QPainter painter(canvas);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.translate(-cropOrigin);
    std::visit(
        [&painter, &annotation](const auto& geometry) {
            using Geometry = std::decay_t<decltype(geometry)>;
            if constexpr (std::is_same_v<Geometry, RectangleGeometry>) {
                painter.setPen(QPen(annotation.style.strokeColor,
                                    annotation.style.strokeWidth, Qt::SolidLine,
                                    Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(geometry.rect.normalized());
            } else if constexpr (std::is_same_v<Geometry, ArrowGeometry>) {
                drawArrow(&painter, geometry, annotation.style);
            } else if constexpr (std::is_same_v<Geometry, TextGeometry>) {
                painter.setPen(annotation.style.strokeColor);
                painter.setFont(annotation.style.font);
                painter.setBrush(Qt::NoBrush);
                painter.drawText(geometry.rect.normalized(),
                                 Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                 geometry.text);
            }
        },
        annotation.geometry);
}

void drawInteractiveAnnotation(
    QPainter* painter,
    const Annotation& annotation,
    const QRectF& displayRect,
    const QRect& cropRect)
{
    if (painter == nullptr || !annotation.isValid()
        || displayRect.isEmpty() || cropRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setClipRect(displayRect);
    painter->translate(displayRect.topLeft());
    painter->scale(
        displayRect.width() / static_cast<qreal>(cropRect.width()),
        displayRect.height() / static_cast<qreal>(cropRect.height()));
    painter->translate(-cropRect.topLeft());
    painter->setRenderHints(
        QPainter::Antialiasing | QPainter::TextAntialiasing);

    if (annotation.type == AnnotationType::Mosaic) {
        const auto& mosaic = std::get<MosaicGeometry>(annotation.geometry);
        if (!mosaic.points.isEmpty()) {
            QPen outline(QColor(32, 32, 32, 190), mosaic.brushWidth,
                         Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(outline);
            if (mosaic.points.size() == 1) {
                const qreal radius = mosaic.brushWidth * 0.5;
                painter->setBrush(QColor(128, 128, 128, 80));
                painter->drawEllipse(mosaic.points.front(), radius, radius);
            } else {
                QPainterPath path(mosaic.points.front());
                for (qsizetype index = 1; index < mosaic.points.size(); ++index) {
                    path.lineTo(mosaic.points.at(index));
                }
                painter->drawPath(path);
            }
        }
    } else {
        std::visit(
            [painter, &annotation](const auto& geometry) {
                using Geometry = std::decay_t<decltype(geometry)>;
                if constexpr (std::is_same_v<Geometry, RectangleGeometry>) {
                    painter->setPen(QPen(
                        annotation.style.strokeColor,
                        annotation.style.strokeWidth,
                        Qt::SolidLine,
                        Qt::RoundCap,
                        Qt::RoundJoin));
                    painter->setBrush(Qt::NoBrush);
                    painter->drawRect(geometry.rect.normalized());
                } else if constexpr (std::is_same_v<Geometry, ArrowGeometry>) {
                    drawArrow(painter, geometry, annotation.style);
                } else if constexpr (std::is_same_v<Geometry, TextGeometry>) {
                    painter->setPen(annotation.style.strokeColor);
                    painter->setFont(annotation.style.font);
                    painter->setBrush(Qt::NoBrush);
                    painter->drawText(
                        geometry.rect.normalized(),
                        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                        geometry.text);
                }
            },
            annotation.geometry);
    }
    painter->restore();
}

QImage renderPresentation(const ScreenshotSession& session,
                          const QUuid& overrideId,
                          const std::optional<AnnotationGeometry>& overrideGeometry,
                          const std::optional<Annotation>& transientAnnotation) {
    if (!session.hasSourceImage() || session.cropRect().isEmpty()) {
        return {};
    }
    QImage canvas = session.sourceImage().copy(session.cropRect());
    if (canvas.format() != QImage::Format_ARGB32_Premultiplied) {
        canvas = canvas.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    canvas.setDevicePixelRatio(1.0);
    const QPoint cropOrigin = session.cropRect().topLeft();

    for (Annotation annotation : session.annotations().annotationsInPaintOrder()) {
        if (annotation.id == overrideId) {
            if (overrideGeometry.has_value()) {
                annotation.geometry = *overrideGeometry;
            } else {
                continue;
            }
        }
        if (!annotation.isValid()
            || !annotation.bounds().intersects(QRectF(session.cropRect()))) {
            continue;
        }
        if (annotation.type == AnnotationType::Mosaic) {
            rasterizeMosaic(&canvas, std::get<MosaicGeometry>(annotation.geometry),
                            annotation.style, cropOrigin);
        } else {
            drawVectorAnnotation(&canvas, annotation, cropOrigin);
        }
    }

    if (transientAnnotation.has_value() && transientAnnotation->isValid()) {
        if (transientAnnotation->type == AnnotationType::Mosaic) {
            rasterizeMosaic(&canvas,
                            std::get<MosaicGeometry>(transientAnnotation->geometry),
                            transientAnnotation->style, cropOrigin);
        } else {
            drawVectorAnnotation(&canvas, *transientAnnotation, cropOrigin);
        }
    }
    return canvas;
}

int nextZOrder(const AnnotationDocument& document) {
    int result = -1;
    for (const Annotation& annotation : document.annotations()) {
        result = std::max(result, annotation.zOrder);
    }
    return result == std::numeric_limits<int>::max() ? result : result + 1;
}

QRectF geometryControlRect(const AnnotationGeometry& geometry,
                           qreal minimumExtent) {
    QRectF result = std::visit(
        [](const auto& value) -> QRectF {
            using Geometry = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Geometry, RectangleGeometry>
                          || std::is_same_v<Geometry, TextGeometry>) {
                return value.rect.normalized();
            } else if constexpr (std::is_same_v<Geometry, ArrowGeometry>) {
                return QRectF(value.start, value.end).normalized();
            } else {
                if (value.points.isEmpty()) {
                    return {};
                }
                QRectF bounds(value.points.front(), QSizeF());
                for (const QPointF& point : value.points) {
                    bounds |= QRectF(point, QSizeF());
                }
                const qreal radius = value.brushWidth * 0.5;
                return bounds.normalized().adjusted(-radius, -radius, radius, radius);
            }
        },
        geometry);
    if (result.width() < minimumExtent) {
        const qreal difference = minimumExtent - result.width();
        result.adjust(-difference * 0.5, 0.0, difference * 0.5, 0.0);
    }
    if (result.height() < minimumExtent) {
        const qreal difference = minimumExtent - result.height();
        result.adjust(0.0, -difference * 0.5, 0.0, difference * 0.5);
    }
    return result.normalized();
}

QPointF mapPointBetweenRects(const QPointF& point,
                             const QRectF& from,
                             const QRectF& to) {
    const qreal xRatio = from.width() > 0.0
        ? (point.x() - from.left()) / from.width()
        : 0.5;
    const qreal yRatio = from.height() > 0.0
        ? (point.y() - from.top()) / from.height()
        : 0.5;
    return QPointF(to.left() + xRatio * to.width(),
                   to.top() + yRatio * to.height());
}

AnnotationGeometry resizedGeometry(const AnnotationGeometry& geometry,
                                   const QRectF& from,
                                   const QRectF& to) {
    return std::visit(
        [&from, &to](const auto& value) -> AnnotationGeometry {
            using Geometry = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Geometry, RectangleGeometry>) {
                return RectangleGeometry{to.normalized()};
            } else if constexpr (std::is_same_v<Geometry, TextGeometry>) {
                return TextGeometry{to.normalized(), value.text};
            } else if constexpr (std::is_same_v<Geometry, ArrowGeometry>) {
                return ArrowGeometry{mapPointBetweenRects(value.start, from, to),
                                     mapPointBetweenRects(value.end, from, to)};
            } else {
                MosaicGeometry result = value;
                for (QPointF& point : result.points) {
                    point = mapPointBetweenRects(point, from, to);
                }
                const qreal xScale = from.width() > 0.0 ? to.width() / from.width() : 1.0;
                const qreal yScale = from.height() > 0.0 ? to.height() / from.height() : 1.0;
                result.brushWidth = std::max<qreal>(1.0, result.brushWidth
                    * std::sqrt(std::abs(xScale * yScale)));
                return result;
            }
        },
        geometry);
}

QRectF adjustedResizeRect(const QRectF& original,
                          ResizeHandle handle,
                          const QPointF& point,
                          const QRectF& limits,
                          qreal minimumExtent) {
    QRectF result = original;
    const qreal x = std::clamp(point.x(), limits.left(), limits.right());
    const qreal y = std::clamp(point.y(), limits.top(), limits.bottom());
    const bool changesLeft = handle == ResizeHandle::TopLeft
        || handle == ResizeHandle::Left || handle == ResizeHandle::BottomLeft;
    const bool changesRight = handle == ResizeHandle::TopRight
        || handle == ResizeHandle::Right || handle == ResizeHandle::BottomRight;
    const bool changesTop = handle == ResizeHandle::TopLeft
        || handle == ResizeHandle::Top || handle == ResizeHandle::TopRight;
    const bool changesBottom = handle == ResizeHandle::BottomLeft
        || handle == ResizeHandle::Bottom || handle == ResizeHandle::BottomRight;

    if (changesLeft) {
        result.setLeft(std::min(x, result.right() - minimumExtent));
    } else if (changesRight) {
        result.setRight(std::max(x, result.left() + minimumExtent));
    }
    if (changesTop) {
        result.setTop(std::min(y, result.bottom() - minimumExtent));
    } else if (changesBottom) {
        result.setBottom(std::max(y, result.top() + minimumExtent));
    }
    return result.normalized().intersected(limits);
}

QPointF constrainedTranslation(const Annotation& annotation,
                               const QPointF& requested,
                               const QRectF& limits) {
    QRectF moved = annotation.bounds().translated(requested);
    QPointF result = requested;
    if (moved.left() < limits.left()) {
        result.rx() += limits.left() - moved.left();
    }
    if (moved.right() > limits.right()) {
        result.rx() -= moved.right() - limits.right();
    }
    if (moved.top() < limits.top()) {
        result.ry() += limits.top() - moved.top();
    }
    if (moved.bottom() > limits.bottom()) {
        result.ry() -= moved.bottom() - limits.bottom();
    }
    return result;
}

}  // namespace

struct CanvasWidget::InteractionState {
    InteractionMode mode = InteractionMode::None;
    ResizeHandle handle = ResizeHandle::None;
    QUuid annotationId;
    QPointF startImage;
    QPointF currentImage;
    AnnotationGeometry before{RectangleGeometry{}};
    AnnotationGeometry preview{RectangleGeometry{}};
    QRectF originalControlRect;
    QVector<QPointF> mosaicPoints;
    QImage previewImage;
    bool previewDirty = true;
};

CanvasWidget::CanvasWidget(QWidget* parent) : QWidget(parent) {
    initialize();
}

CanvasWidget::CanvasWidget(ScreenshotSession* session, QWidget* parent)
    : QWidget(parent) {
    initialize();
    setSession(session);
}

CanvasWidget::~CanvasWidget() = default;

void CanvasWidget::initialize() {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    interaction_ = std::make_unique<InteractionState>();
}

void CanvasWidget::setSession(ScreenshotSession* session) {
    if (session_ == session) {
        return;
    }
    disconnect(undoIndexConnection_);
    cancelInteraction();
    resetNudgeMergeGroup();
    session_ = session;
    if (session_ != nullptr) {
        undoIndexConnection_ = connect(
            &session_->undoStack(), &QUndoStack::indexChanged,
            this, &CanvasWidget::handleUndoIndexChanged);
    }
    invalidatePresentation();
    updateGeometry();
    update();
    emit selectionChanged();
    emit contentChanged();
}

ScreenshotSession* CanvasWidget::session() noexcept { return session_; }
const ScreenshotSession* CanvasWidget::session() const noexcept { return session_; }

void CanvasWidget::setTool(CanvasTool tool) {
    if (tool_ == tool) {
        return;
    }
    cancelInteraction();
    resetNudgeMergeGroup();
    tool_ = tool;
    unsetCursor();
    update();
    emit toolChanged(tool_);
}

CanvasTool CanvasWidget::tool() const noexcept { return tool_; }

void CanvasWidget::setCurrentStyle(const AnnotationStyle& style) {
    if (!style.strokeColor.isValid() || !style.fillColor.isValid()
        || !std::isfinite(style.strokeWidth) || style.strokeWidth <= 0.0
        || style.mosaicBlockSize < 2 || currentStyle_ == style) {
        return;
    }
    currentStyle_ = style;
    applyCurrentStyleToSelection();
    emit currentStyleChanged(currentStyle_);
    update();
}

const AnnotationStyle& CanvasWidget::currentStyle() const noexcept {
    return currentStyle_;
}

void CanvasWidget::setStrokeColor(const QColor& color) {
    if (!color.isValid()) return;
    AnnotationStyle style = currentStyle_;
    style.strokeColor = color;
    setCurrentStyle(style);
}

void CanvasWidget::setStrokeWidth(qreal width) {
    if (!std::isfinite(width) || width <= 0.0) return;
    AnnotationStyle style = currentStyle_;
    style.strokeWidth = width;
    setCurrentStyle(style);
}

void CanvasWidget::setTextFont(const QFont& font) {
    AnnotationStyle style = currentStyle_;
    style.font = font;
    setCurrentStyle(style);
}

void CanvasWidget::setMosaicBlockSize(int blockSize) {
    if (blockSize < 2) return;
    AnnotationStyle style = currentStyle_;
    style.mosaicBlockSize = blockSize;
    setCurrentStyle(style);
}

void CanvasWidget::setMosaicBrushWidth(qreal width) {
    if (!std::isfinite(width) || width <= 0.0 || qFuzzyCompare(width, mosaicBrushWidth_)) {
        return;
    }
    mosaicBrushWidth_ = width;
}

qreal CanvasWidget::mosaicBrushWidth() const noexcept { return mosaicBrushWidth_; }

quint64 CanvasWidget::rebuildableCacheByteSize() const noexcept
{
    quint64 bytes = cachedPresentation_.isNull()
        ? 0
        : static_cast<quint64>(cachedPresentation_.sizeInBytes());
    if (interaction_ != nullptr && !interaction_->previewImage.isNull()) {
        const quint64 previewBytes =
            static_cast<quint64>(interaction_->previewImage.sizeInBytes());
        if (previewBytes <= std::numeric_limits<quint64>::max() - bytes) {
            bytes += previewBytes;
        } else {
            bytes = std::numeric_limits<quint64>::max();
        }
    }
    return bytes;
}

void CanvasWidget::releaseRebuildableCaches()
{
    invalidatePresentation();
    update();
}

QRectF CanvasWidget::imageDisplayRect() const {
    if (session_ == nullptr || session_->cropRect().isEmpty() || width() <= 0 || height() <= 0) {
        return {};
    }
    const QSize imageSize = session_->cropRect().size();
    const qreal scale = std::min(width() / static_cast<qreal>(imageSize.width()),
                                 height() / static_cast<qreal>(imageSize.height()));
    const QSizeF displaySize(imageSize.width() * scale, imageSize.height() * scale);
    return QRectF(QPointF((width() - displaySize.width()) * 0.5,
                          (height() - displaySize.height()) * 0.5),
                  displaySize);
}

std::optional<QPointF> CanvasWidget::mapWidgetToImage(const QPointF& widgetPoint) const {
    const QRectF display = imageDisplayRect();
    if (session_ == nullptr || display.isEmpty() || !display.contains(widgetPoint)) {
        return std::nullopt;
    }
    const QRectF crop(session_->cropRect());
    const qreal x = crop.left()
        + (widgetPoint.x() - display.left()) * crop.width() / display.width();
    const qreal y = crop.top()
        + (widgetPoint.y() - display.top()) * crop.height() / display.height();
    return QPointF(std::clamp(x, crop.left(), crop.right()),
                   std::clamp(y, crop.top(), crop.bottom()));
}

QPointF CanvasWidget::mapImageToWidget(const QPointF& imagePoint) const {
    const QRectF display = imageDisplayRect();
    if (session_ == nullptr || display.isEmpty()) {
        return {};
    }
    const QRectF crop(session_->cropRect());
    return QPointF(display.left() + (imagePoint.x() - crop.left())
                                    * display.width() / crop.width(),
                   display.top() + (imagePoint.y() - crop.top())
                                   * display.height() / crop.height());
}

QSize CanvasWidget::sizeHint() const {
    if (session_ == nullptr || session_->cropRect().isEmpty()) {
        return QSize(720, 480);
    }
    return session_->cropRect().size().scaled(QSize(960, 720), Qt::KeepAspectRatio)
        .expandedTo(minimumSizeHint());
}

QSize CanvasWidget::minimumSizeHint() const { return QSize(320, 200); }

void CanvasWidget::deleteSelection() {
    if (session_ == nullptr) return;
    const QSet<QUuid> ids = session_->annotations().selectedAnnotationIds();
    if (ids.isEmpty()) return;
    if (ids.size() == 1) {
        session_->undoStack().push(
            new RemoveAnnotationCommand(&session_->annotations(), *ids.cbegin()));
        return;
    }
    auto* group = new QUndoCommand(tr("删除标注"));
    for (const QUuid& id : ids) {
        new RemoveAnnotationCommand(&session_->annotations(), id, group);
    }
    session_->undoStack().push(group);
}

void CanvasWidget::clearAnnotations() {
    if (session_ != nullptr && !session_->annotations().isEmpty()) {
        session_->undoStack().push(new ClearAnnotationsCommand(&session_->annotations()));
    }
}

void CanvasWidget::editSelectedText() {
    if (session_ == nullptr) return;
    const Annotation* annotation = primarySelection();
    if (annotation == nullptr || annotation->type != AnnotationType::Text) return;
    const auto* geometry = std::get_if<TextGeometry>(&annotation->geometry);
    if (geometry == nullptr) return;
    bool accepted = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("编辑文字"), tr("文字内容"), geometry->text, &accepted,
        Qt::Dialog, Qt::ImhMultiLine);
    if (accepted && text != geometry->text) {
        session_->undoStack().push(
            new EditTextCommand(&session_->annotations(), annotation->id, text));
    }
}

void CanvasWidget::refreshFromSession() {
    invalidatePresentation();
    updateGeometry();
    update();
    emit contentChanged();
}

void CanvasWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const QRectF display = imageDisplayRect();
    if (display.isEmpty()) return;

    const QImage image = presentationImage();
    if (!image.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(display, image);
    }

    if (interaction_->mode == InteractionMode::CreateRectangle
        || interaction_->mode == InteractionMode::CreateArrow) {
        const qreal scale = display.width() / session_->cropRect().width();
        painter.save();
        painter.setClipRect(display);
        QTransform transform;
        transform.translate(display.left(), display.top());
        transform.scale(scale, scale);
        transform.translate(-session_->cropRect().left(), -session_->cropRect().top());
        painter.setTransform(transform, true);
        if (interaction_->mode == InteractionMode::CreateRectangle) {
            painter.setPen(QPen(currentStyle_.strokeColor, currentStyle_.strokeWidth,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(interaction_->startImage,
                                    interaction_->currentImage).normalized());
        } else {
            drawArrow(&painter,
                      ArrowGeometry{interaction_->startImage,
                                    interaction_->currentImage},
                      currentStyle_);
        }
        painter.restore();
    } else if (interaction_->mode == InteractionMode::CreateText) {
        const QRectF preview(mapImageToWidget(interaction_->startImage),
                             mapImageToWidget(interaction_->currentImage));
        painter.setPen(QPen(QColor(10, 132, 255), 1.0, Qt::DashLine));
        painter.setBrush(QColor(10, 132, 255, 24));
        painter.drawRect(preview.normalized());
    }

    if (interaction_->mode == InteractionMode::CreateMosaic
        && !interaction_->mosaicPoints.isEmpty()) {
        drawInteractiveAnnotation(
            &painter,
            Annotation::makeMosaic(
                interaction_->mosaicPoints,
                mosaicBrushWidth_,
                currentStyle_,
                nextZOrder(session_->annotations())),
            display,
            session_->cropRect());
    } else if (interaction_->mode == InteractionMode::Move
               || interaction_->mode == InteractionMode::Resize) {
        const Annotation* moving = session_->annotations().annotation(
            interaction_->annotationId);
        if (moving != nullptr) {
            Annotation preview = *moving;
            preview.geometry = interaction_->preview;
            drawInteractiveAnnotation(
                &painter, preview, display, session_->cropRect());
        }
    }

    const Annotation* selected = primarySelection();
    if (selected == nullptr) return;
    AnnotationGeometry selectedGeometry = selected->geometry;
    if ((interaction_->mode == InteractionMode::Move
         || interaction_->mode == InteractionMode::Resize)
        && interaction_->annotationId == selected->id) {
        selectedGeometry = interaction_->preview;
    }
    const qreal imagePerWidgetPixel = session_->cropRect().width() / display.width();
    const QRectF imageBounds = geometryControlRect(
        selectedGeometry, kMinimumControlExtent * imagePerWidgetPixel);
    const QRectF selectionRect(mapImageToWidget(imageBounds.topLeft()),
                               mapImageToWidget(imageBounds.bottomRight()));
    painter.setPen(QPen(QColor(10, 132, 255), 1.2, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(selectionRect.normalized());

    const QPointF center = selectionRect.center();
    const std::array<QPointF, 8> handles{
        selectionRect.topLeft(), QPointF(center.x(), selectionRect.top()),
        selectionRect.topRight(), QPointF(selectionRect.right(), center.y()),
        selectionRect.bottomRight(), QPointF(center.x(), selectionRect.bottom()),
        selectionRect.bottomLeft(), QPointF(selectionRect.left(), center.y())};
    painter.setPen(QPen(Qt::white, 1.0));
    painter.setBrush(QColor(10, 132, 255));
    for (const QPointF& point : handles) {
        painter.drawRect(QRectF(point.x() - kHandleSize * 0.5,
                                point.y() - kHandleSize * 0.5,
                                kHandleSize, kHandleSize));
    }
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || session_ == nullptr) {
        QWidget::mousePressEvent(event);
        return;
    }
    resetNudgeMergeGroup();
    const auto imagePoint = mapWidgetToImage(event->position());
    if (!imagePoint.has_value()) {
        if (tool_ == CanvasTool::Select) clearSelection();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    interaction_->startImage = *imagePoint;
    interaction_->currentImage = *imagePoint;
    interaction_->previewDirty = true;

    if (tool_ == CanvasTool::Rectangle) {
        interaction_->mode = InteractionMode::CreateRectangle;
    } else if (tool_ == CanvasTool::Arrow) {
        interaction_->mode = InteractionMode::CreateArrow;
    } else if (tool_ == CanvasTool::Text) {
        interaction_->mode = InteractionMode::CreateText;
    } else if (tool_ == CanvasTool::Mosaic) {
        interaction_->mode = InteractionMode::CreateMosaic;
        interaction_->mosaicPoints = {*imagePoint};
    } else {
        const Annotation* selected = primarySelection();
        ResizeHandle hitHandle = ResizeHandle::None;
        if (selected != nullptr) {
            const QRectF display = imageDisplayRect();
            const qreal imagePerPixel = session_->cropRect().width() / display.width();
            const QRectF bounds = geometryControlRect(
                selected->geometry, kMinimumControlExtent * imagePerPixel);
            const QRectF widgetBounds(mapImageToWidget(bounds.topLeft()),
                                      mapImageToWidget(bounds.bottomRight()));
            const QPointF center = widgetBounds.center();
            const std::array<std::pair<ResizeHandle, QPointF>, 8> handles{{
                {ResizeHandle::TopLeft, widgetBounds.topLeft()},
                {ResizeHandle::Top, QPointF(center.x(), widgetBounds.top())},
                {ResizeHandle::TopRight, widgetBounds.topRight()},
                {ResizeHandle::Right, QPointF(widgetBounds.right(), center.y())},
                {ResizeHandle::BottomRight, widgetBounds.bottomRight()},
                {ResizeHandle::Bottom, QPointF(center.x(), widgetBounds.bottom())},
                {ResizeHandle::BottomLeft, widgetBounds.bottomLeft()},
                {ResizeHandle::Left, QPointF(widgetBounds.left(), center.y())}}};
            for (const auto& [handle, point] : handles) {
                if (QRectF(point.x() - kHandleSize, point.y() - kHandleSize,
                           kHandleSize * 2.0, kHandleSize * 2.0)
                        .contains(event->position())) {
                    hitHandle = handle;
                    break;
                }
            }
        }
        if (selected != nullptr && hitHandle != ResizeHandle::None) {
            interaction_->mode = InteractionMode::Resize;
            interaction_->handle = hitHandle;
            interaction_->annotationId = selected->id;
            interaction_->before = selected->geometry;
            interaction_->preview = selected->geometry;
            const qreal imagePerPixel = session_->cropRect().width()
                / imageDisplayRect().width();
            interaction_->originalControlRect = geometryControlRect(
                selected->geometry, kMinimumControlExtent * imagePerPixel);
        } else if (const Annotation* hit = hitAnnotation(*imagePoint)) {
            setSelectedAnnotation(hit->id);
            interaction_->mode = InteractionMode::Move;
            interaction_->annotationId = hit->id;
            interaction_->before = hit->geometry;
            interaction_->preview = hit->geometry;
        } else {
            clearSelection();
            interaction_->mode = InteractionMode::None;
        }
    }
    update();
    event->accept();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (session_ == nullptr || interaction_->mode == InteractionMode::None) {
        updateHoverCursor(event->position());
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto imagePoint = mapWidgetToImage(event->position());
    if (!imagePoint.has_value()) return;
    interaction_->currentImage = *imagePoint;
    interaction_->previewDirty = true;

    if (interaction_->mode == InteractionMode::CreateMosaic) {
        if (interaction_->mosaicPoints.isEmpty()
            || QLineF(interaction_->mosaicPoints.back(), *imagePoint).length() >= 1.0) {
            interaction_->mosaicPoints.push_back(*imagePoint);
        }
    } else if (interaction_->mode == InteractionMode::Move) {
        if (const Annotation* annotation = session_->annotations().annotation(
                interaction_->annotationId)) {
            const QPointF delta = constrainedTranslation(
                *annotation, *imagePoint - interaction_->startImage,
                QRectF(session_->cropRect()));
            interaction_->preview = translatedGeometry(interaction_->before, delta);
        }
    } else if (interaction_->mode == InteractionMode::Resize) {
        const QRectF display = imageDisplayRect();
        const qreal minimum = kMinimumControlExtent * session_->cropRect().width()
            / display.width();
        const QRectF resized = adjustedResizeRect(
            interaction_->originalControlRect, interaction_->handle, *imagePoint,
            QRectF(session_->cropRect()), minimum);
        interaction_->preview = resizedGeometry(
            interaction_->before, interaction_->originalControlRect, resized);
    }
    update();
    event->accept();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || session_ == nullptr
        || interaction_->mode == InteractionMode::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const auto imagePoint = mapWidgetToImage(event->position());
    finishInteraction(imagePoint.value_or(interaction_->currentImage));
    event->accept();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (tool_ == CanvasTool::Select && event->button() == Qt::LeftButton) {
        const auto imagePoint = mapWidgetToImage(event->position());
        if (imagePoint.has_value()) {
            if (const Annotation* hit = hitAnnotation(*imagePoint);
                hit != nullptr && hit->type == AnnotationType::Text) {
                cancelInteraction();
                setSelectedAnnotation(hit->id);
                editSelectedText();
                event->accept();
                return;
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (nudgeSelection(event->key(), event->modifiers())) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Control
        || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Meta) {
        QWidget::keyPressEvent(event);
        return;
    }
    resetNudgeMergeGroup();
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        deleteSelection();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Undo) && session_ != nullptr) {
        session_->undoStack().undo();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo) && session_ != nullptr) {
        session_->undoStack().redo();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (interaction_->mode != InteractionMode::None) cancelInteraction();
        else clearSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F2 || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        editSelectedText();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool CanvasWidget::nudgeSelection(
    const int key,
    const Qt::KeyboardModifiers modifiers) {
    if (session_ == nullptr || tool_ != CanvasTool::Select
        || (modifiers & (Qt::ControlModifier | Qt::AltModifier
                         | Qt::MetaModifier)) != Qt::NoModifier) {
        return false;
    }

    QPointF direction;
    switch (key) {
        case Qt::Key_Left:
            direction.setX(-1.0);
            break;
        case Qt::Key_Right:
            direction.setX(1.0);
            break;
        case Qt::Key_Up:
            direction.setY(-1.0);
            break;
        case Qt::Key_Down:
            direction.setY(1.0);
            break;
        default:
            return false;
    }

    const Annotation* annotation = primarySelection();
    if (annotation == nullptr) {
        return false;
    }

    const qreal step = modifiers.testFlag(Qt::ShiftModifier)
        ? kAcceleratedNudgeStep : kNudgeStep;
    const QPointF delta = constrainedTranslation(
        *annotation, direction * step, QRectF(session_->cropRect()));
    if (delta.isNull()) {
        return true;
    }

    const bool continuesPrevious = nudgeMergeTimer_.isValid()
        && nudgeMergeTimer_.elapsed() <= kNudgeMergeIntervalMs
        && nudgeAnnotationId_ == annotation->id
        && nudgeMergeGroup_ != 0;
    if (!continuesPrevious) {
        nudgeMergeGroup_ = nextNudgeMergeGroup_++;
        if (nextNudgeMergeGroup_ == 0) {
            nextNudgeMergeGroup_ = 1;
        }
        nudgeAnnotationId_ = annotation->id;
    }
    nudgeMergeTimer_.restart();

    session_->undoStack().push(new TransformAnnotationCommand(
        &session_->annotations(), annotation->id,
        translatedGeometry(annotation->geometry, delta), nudgeMergeGroup_));
    return true;
}

void CanvasWidget::resetNudgeMergeGroup() {
    nudgeMergeTimer_.invalidate();
    nudgeAnnotationId_ = {};
    nudgeMergeGroup_ = 0;
}

void CanvasWidget::invalidatePresentation() {
    presentationValid_ = false;
    cachedPresentation_ = {};
    if (interaction_) {
        interaction_->previewDirty = true;
        interaction_->previewImage = {};
    }
}

void CanvasWidget::handleUndoIndexChanged() {
    cancelInteraction();
    invalidatePresentation();
    update();
    emit contentChanged();
    emit selectionChanged();
}

void CanvasWidget::setSelectedAnnotation(const QUuid& annotationId) {
    if (session_ == nullptr) return;
    const QSet<QUuid> desired = annotationId.isNull()
        ? QSet<QUuid>{} : QSet<QUuid>{annotationId};
    if (desired == session_->annotations().selectedAnnotationIds()) return;
    resetNudgeMergeGroup();
    session_->annotations().setSelectedAnnotationIds(desired);
    update();
    emit selectionChanged();
}

void CanvasWidget::clearSelection() { setSelectedAnnotation({}); }

void CanvasWidget::cancelInteraction() {
    if (!interaction_) return;
    *interaction_ = InteractionState{};
    update();
}

void CanvasWidget::updateHoverCursor(const QPointF& widgetPoint) {
    if (tool_ != CanvasTool::Select) {
        setCursor(Qt::CrossCursor);
        return;
    }
    const auto imagePoint = mapWidgetToImage(widgetPoint);
    setCursor(imagePoint.has_value() && hitAnnotation(*imagePoint) != nullptr
                  ? Qt::SizeAllCursor : Qt::ArrowCursor);
}

void CanvasWidget::finishInteraction(const QPointF& imagePoint) {
    interaction_->currentImage = imagePoint;
    const InteractionState finished = *interaction_;
    *interaction_ = InteractionState{};

    if (finished.mode == InteractionMode::CreateRectangle) {
        const QRectF rectangle(finished.startImage, imagePoint);
        if (QLineF(mapImageToWidget(finished.startImage),
                   mapImageToWidget(imagePoint)).length() >= kMinimumWidgetDrag) {
            const Annotation annotation = Annotation::makeRectangle(
                rectangle.normalized(), currentStyle_,
                nextZOrder(session_->annotations()));
            session_->undoStack().push(
                new AddAnnotationCommand(&session_->annotations(), annotation));
            setSelectedAnnotation(annotation.id);
        }
    } else if (finished.mode == InteractionMode::CreateArrow) {
        if (QLineF(finished.startImage, imagePoint).length() >= 1.0) {
            const Annotation annotation = Annotation::makeArrow(
                finished.startImage, imagePoint, currentStyle_,
                nextZOrder(session_->annotations()));
            session_->undoStack().push(
                new AddAnnotationCommand(&session_->annotations(), annotation));
            setSelectedAnnotation(annotation.id);
        }
    } else if (finished.mode == InteractionMode::CreateText) {
        createTextAnnotation(QRectF(finished.startImage, imagePoint).normalized());
    } else if (finished.mode == InteractionMode::CreateMosaic) {
        QVector<QPointF> points = finished.mosaicPoints;
        if (points.isEmpty()) points.push_back(finished.startImage);
        const QUuid annotationId = QUuid::createUuid();
        session_->undoStack().push(new AddMosaicStrokeCommand(
            &session_->annotations(), points, mosaicBrushWidth_, currentStyle_,
            nextZOrder(session_->annotations()), annotationId));
        setSelectedAnnotation(annotationId);
    } else if ((finished.mode == InteractionMode::Move
                || finished.mode == InteractionMode::Resize)
               && finished.before != finished.preview) {
        session_->undoStack().push(new TransformAnnotationCommand(
            &session_->annotations(), finished.annotationId,
            finished.before, finished.preview, 0));
    }
    invalidatePresentation();
    update();
}

void CanvasWidget::createTextAnnotation(const QRectF& requestedRect) {
    if (session_ == nullptr) return;
    const QRectF crop(session_->cropRect());
    QRectF textRect = requestedRect.intersected(crop);
    const qreal imagePerPixel = crop.width() / imageDisplayRect().width();
    if (textRect.width() < kMinimumWidgetDrag * imagePerPixel
        || textRect.height() < kMinimumWidgetDrag * imagePerPixel) {
        const QSizeF preferred(std::min<qreal>(320.0, crop.width()),
                               std::min<qreal>(100.0, crop.height()));
        textRect = QRectF(requestedRect.topLeft(), preferred);
        if (textRect.right() > crop.right()) textRect.moveRight(crop.right());
        if (textRect.bottom() > crop.bottom()) textRect.moveBottom(crop.bottom());
        textRect = textRect.intersected(crop);
    }
    if (textRect.isEmpty()) return;

    bool accepted = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("添加文字"), tr("文字内容"), {}, &accepted,
        Qt::Dialog, Qt::ImhMultiLine);
    if (!accepted || text.trimmed().isEmpty()) return;
    const Annotation annotation = Annotation::makeText(
        textRect, text, currentStyle_, nextZOrder(session_->annotations()));
    session_->undoStack().push(
        new AddAnnotationCommand(&session_->annotations(), annotation));
    setSelectedAnnotation(annotation.id);
}

void CanvasWidget::applyCurrentStyleToSelection() {
    if (session_ == nullptr) return;
    const QSet<QUuid> ids = session_->annotations().selectedAnnotationIds();
    QVector<QUuid> changedIds;
    for (const QUuid& id : ids) {
        const Annotation* annotation = session_->annotations().annotation(id);
        if (annotation != nullptr && annotation->style != currentStyle_) {
            changedIds.push_back(id);
        }
    }
    if (changedIds.isEmpty()) return;
    if (changedIds.size() == 1) {
        session_->undoStack().push(new ChangeStyleCommand(
            &session_->annotations(), changedIds.front(), currentStyle_));
        return;
    }
    auto* group = new QUndoCommand(tr("修改标注样式"));
    for (const QUuid& id : changedIds) {
        new ChangeStyleCommand(&session_->annotations(), id, currentStyle_, 0, group);
    }
    session_->undoStack().push(group);
}

const Annotation* CanvasWidget::hitAnnotation(const QPointF& imagePoint) const {
    if (session_ == nullptr) return nullptr;
    const QRectF display = imageDisplayRect();
    const qreal tolerance = display.isEmpty() ? kHitTolerance
        : kHitTolerance * session_->cropRect().width() / display.width();
    const auto ordered = session_->annotations().annotationsInPaintOrder();
    for (auto iterator = ordered.crbegin(); iterator != ordered.crend(); ++iterator) {
        if (iterator->hitTest(imagePoint, tolerance)) {
            return session_->annotations().annotation(iterator->id);
        }
    }
    return nullptr;
}

const Annotation* CanvasWidget::primarySelection() const {
    if (session_ == nullptr) return nullptr;
    const QSet<QUuid>& ids = session_->annotations().selectedAnnotationIds();
    if (ids.size() != 1) return nullptr;
    return session_->annotations().annotation(*ids.cbegin());
}

QImage CanvasWidget::presentationImage() {
    if (session_ == nullptr) return {};
    const bool editingExisting = interaction_->mode == InteractionMode::Move
        || interaction_->mode == InteractionMode::Resize;
    if (editingExisting) {
        if (interaction_->previewImage.isNull()) {
            // Build the static background without the selected annotation once
            // at interaction start. Subsequent mouse moves draw only the live
            // vector/mosaic outline in widget coordinates and never rebuild a
            // full 4K presentation frame.
            interaction_->previewImage = renderPresentation(
                *session_, interaction_->annotationId, std::nullopt,
                std::nullopt);
            interaction_->previewDirty = false;
        }
        return interaction_->previewImage;
    }
    if (!presentationValid_ || cachedRevision_ != session_->currentRevision()) {
        cachedPresentation_ = renderPresentation(*session_, {}, std::nullopt,
                                                  std::nullopt);
        cachedRevision_ = session_->currentRevision();
        presentationValid_ = true;
    }
    return cachedPresentation_;
}

}  // namespace snapask::ui::canvas
