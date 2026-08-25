#include "services/SnapshotRenderer.h"

#include "domain/annotation/Annotation.h"
#include "domain/capture/ScreenshotSession.h"

#include <QCryptographicHash>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QtMath>
#include <QtPng/png.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace snapask {
namespace {

constexpr qreal kArrowHeadAngleDegrees = 28.0;

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
    const QRectF sourceBounds =
        QRectF(QPointF(left, top), QPointF(right, bottom))
            .normalized()
            .adjusted(-radius, -radius, radius, radius);
    return sourceBounds.translated(-cropOrigin).toAlignedRect().intersected(
        imageRect);
}

QColor averageBlockColor(const QImage& image, const QRect& block) {
    quint64 alphaSum = 0;
    quint64 redWeightedSum = 0;
    quint64 greenWeightedSum = 0;
    quint64 blueWeightedSum = 0;
    quint64 pixelCount = 0;

    for (int y = block.top(); y <= block.bottom(); ++y) {
        for (int x = block.left(); x <= block.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const quint64 alpha = static_cast<quint64>(color.alpha());
            alphaSum += alpha;
            redWeightedSum += static_cast<quint64>(color.red()) * alpha;
            greenWeightedSum += static_cast<quint64>(color.green()) * alpha;
            blueWeightedSum += static_cast<quint64>(color.blue()) * alpha;
            ++pixelCount;
        }
    }

    if (pixelCount == 0 || alphaSum == 0) {
        return Qt::transparent;
    }
    return QColor(
        static_cast<int>(redWeightedSum / alphaSum),
        static_cast<int>(greenWeightedSum / alphaSum),
        static_cast<int>(blueWeightedSum / alphaSum),
        static_cast<int>(alphaSum / pixelCount));
}

QImage pixelatedRegion(const QImage& image, const QRect& target, int blockSize) {
    QImage result = image.copy(target);
    blockSize = std::max(2, blockSize);

    const int firstBlockX = (target.left() / blockSize) * blockSize;
    const int firstBlockY = (target.top() / blockSize) * blockSize;
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (int y = firstBlockY; y <= target.bottom(); y += blockSize) {
        for (int x = firstBlockX; x <= target.right(); x += blockSize) {
            const QRect block(x, y, blockSize, blockSize);
            const QRect sourceBlock = block.intersected(image.rect());
            const QRect targetBlock = block.intersected(target);
            if (sourceBlock.isEmpty() || targetBlock.isEmpty()) {
                continue;
            }
            painter.fillRect(targetBlock.translated(-target.topLeft()),
                             averageBlockColor(image, sourceBlock));
        }
    }
    return result;
}

void rasterizeMosaic(QImage* canvas,
                     const MosaicGeometry& geometry,
                     const AnnotationStyle& style,
                     const QPoint& cropOrigin) {
    if (canvas == nullptr || canvas->isNull() || geometry.points.isEmpty() ||
        geometry.brushWidth <= 0.0) {
        return;
    }

    const QRect target =
        alignedMosaicBounds(geometry, cropOrigin, canvas->rect());
    if (target.isEmpty()) {
        return;
    }

    QImage masked = pixelatedRegion(*canvas, target, style.mosaicBlockSize);
    QImage mask(target.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);

    QPainter maskPainter(&mask);
    maskPainter.setRenderHint(QPainter::Antialiasing, true);
    QPen maskPen(Qt::white, geometry.brushWidth, Qt::SolidLine,
                 Qt::RoundCap, Qt::RoundJoin);
    maskPainter.setPen(maskPen);
    const auto toLocal = [&target, &cropOrigin](const QPointF& point) {
        return point - QPointF(cropOrigin) - QPointF(target.topLeft());
    };

    if (geometry.points.size() == 1) {
        const qreal radius = geometry.brushWidth * 0.5;
        const QPointF center = toLocal(geometry.points.front());
        maskPainter.setPen(Qt::NoPen);
        maskPainter.setBrush(Qt::white);
        maskPainter.drawEllipse(center, radius, radius);
    } else {
        QPainterPath path(toLocal(geometry.points.front()));
        for (qsizetype index = 1; index < geometry.points.size(); ++index) {
            path.lineTo(toLocal(geometry.points[index]));
        }
        maskPainter.drawPath(path);
    }
    maskPainter.end();

    QPainter maskedPainter(&masked);
    maskedPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    maskedPainter.drawImage(QPoint(0, 0), mask);
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

    QPen pen(style.strokeColor, style.strokeWidth, Qt::SolidLine,
             Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(style.strokeColor);
    painter->drawLine(shaft);

    const qreal headLength = std::max<qreal>(8.0, style.strokeWidth * 4.0);
    const qreal shaftAngle = std::atan2(arrow.start.y() - arrow.end.y(),
                                       arrow.start.x() - arrow.end.x());
    const qreal spread = qDegreesToRadians(kArrowHeadAngleDegrees);
    const QPointF first = arrow.end +
                          QPointF(std::cos(shaftAngle + spread) * headLength,
                                  std::sin(shaftAngle + spread) * headLength);
    const QPointF second = arrow.end +
                           QPointF(std::cos(shaftAngle - spread) * headLength,
                                   std::sin(shaftAngle - spread) * headLength);
    painter->drawPolygon(QPolygonF{arrow.end, first, second});
}

void drawVectorAnnotation(QImage* canvas,
                          const Annotation& annotation,
                          const QPoint& cropOrigin) {
    QPainter painter(canvas);
    painter.setRenderHints(QPainter::Antialiasing |
                           QPainter::TextAntialiasing);
    painter.translate(-cropOrigin);

    std::visit(
        [&painter, &annotation](const auto& geometry) {
            using Geometry = std::decay_t<decltype(geometry)>;
            if constexpr (std::is_same_v<Geometry, RectangleGeometry>) {
                QPen pen(annotation.style.strokeColor,
                         annotation.style.strokeWidth, Qt::SolidLine,
                         Qt::RoundCap, Qt::RoundJoin);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(geometry.rect.normalized());
            } else if constexpr (std::is_same_v<Geometry, ArrowGeometry>) {
                drawArrow(&painter, geometry, annotation.style);
            } else if constexpr (std::is_same_v<Geometry, TextGeometry>) {
                painter.setPen(annotation.style.strokeColor);
                painter.setFont(annotation.style.font);
                painter.setBrush(Qt::NoBrush);
                painter.drawText(geometry.rect.normalized(),
                                 Qt::AlignLeft | Qt::AlignTop |
                                     Qt::TextWordWrap,
                                 geometry.text);
            }
        },
        annotation.geometry);
}

void appendPngBytes(png_structp png,
                    png_bytep data,
                    const png_size_t length)
{
    auto* bytes = static_cast<QByteArray*>(png_get_io_ptr(png));
    if (bytes == nullptr
        || length > static_cast<png_size_t>(
            std::numeric_limits<qsizetype>::max())) {
        png_error(png, "SnapAsk PNG output overflow");
        return;
    }
    try {
        bytes->append(reinterpret_cast<const char*>(data),
                      static_cast<qsizetype>(length));
    } catch (...) {
        png_error(png, "SnapAsk PNG output allocation failed");
    }
}

void flushPngBytes(png_structp) {}

void ignorePngDiagnostic(png_structp, png_const_charp) {}

QByteArray encodePng(const QImage& image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return {};
    }

    QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
        return {};
    }

    QByteArray pngBytes;
    if (rgba.sizeInBytes() > 0
        && rgba.sizeInBytes()
               < static_cast<qsizetype>(
                   std::numeric_limits<qsizetype>::max() - 64 * 1024)) {
        pngBytes.reserve(rgba.sizeInBytes() + 64 * 1024);
    }

    png_structp png = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, nullptr, ignorePngDiagnostic,
        ignorePngDiagnostic);
    if (png == nullptr) {
        return {};
    }

    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return {};
    }

    std::vector<png_bytep> rows(static_cast<std::size_t>(rgba.height()));
    for (int y = 0; y < rgba.height(); ++y) {
        rows[static_cast<std::size_t>(y)] = rgba.scanLine(y);
    }

    // libpng's documented error boundary is setjmp/longjmp. MSVC warns about
    // this even though all Qt/C++ values are created before the boundary and
    // no C++ stack object requiring destruction is introduced inside it.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        return {};
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    png_set_write_fn(png, &pngBytes, appendPngBytes, flushPngBytes);
    png_set_IHDR(png, info,
                 static_cast<png_uint_32>(rgba.width()),
                 static_cast<png_uint_32>(rgba.height()),
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    // Trying all five PNG row filters dominates high-entropy 4K frames. A
    // single Sub filter preserves every pixel, is deterministic, and gives
    // screenshots useful compression without the expensive per-row heuristic.
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
    png_set_compression_level(png, 1);

    png_set_rows(png, info, rows.data());
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, nullptr);
    png_destroy_write_struct(&png, &info);
    return pngBytes;
}

}  // namespace

RenderedSnapshot::RenderedSnapshot(QImage image,
                                   QByteArray pngBytes,
                                   QByteArray sha256,
                                   quint64 revision)
    : image_(std::move(image)),
      pngBytes_(std::move(pngBytes)),
      sha256_(std::move(sha256)),
      pixelSize_(image_.size()),
      revision_(revision) {
    image_.detach();
}

RenderedSnapshot::RenderedSnapshot(QByteArray pngBytes,
                                   QByteArray sha256,
                                   QSize pixelSize,
                                   quint64 revision)
    : pngBytes_(std::move(pngBytes)),
      sha256_(std::move(sha256)),
      pixelSize_(pixelSize),
      revision_(revision) {}

RenderedSnapshot::RenderedSnapshot(const RenderedSnapshot& other) {
    QMutexLocker locker(&other.decodedImageMutex_);
    image_ = other.image_;
    pngBytes_ = other.pngBytes_;
    sha256_ = other.sha256_;
    pixelSize_ = other.pixelSize_;
    revision_ = other.revision_;
}

RenderedSnapshot::RenderedSnapshot(RenderedSnapshot&& other) noexcept {
    QMutexLocker locker(&other.decodedImageMutex_);
    image_ = std::move(other.image_);
    pngBytes_ = std::move(other.pngBytes_);
    sha256_ = std::move(other.sha256_);
    pixelSize_ = other.pixelSize_;
    revision_ = other.revision_;
    other.pixelSize_ = {};
    other.revision_ = 0;
}

RenderedSnapshot& RenderedSnapshot::operator=(const RenderedSnapshot& other) {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(decodedImageMutex_, other.decodedImageMutex_);
    image_ = other.image_;
    pngBytes_ = other.pngBytes_;
    sha256_ = other.sha256_;
    pixelSize_ = other.pixelSize_;
    revision_ = other.revision_;
    return *this;
}

RenderedSnapshot& RenderedSnapshot::operator=(RenderedSnapshot&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(decodedImageMutex_, other.decodedImageMutex_);
    image_ = std::move(other.image_);
    pngBytes_ = std::move(other.pngBytes_);
    sha256_ = std::move(other.sha256_);
    pixelSize_ = other.pixelSize_;
    revision_ = other.revision_;
    other.pixelSize_ = {};
    other.revision_ = 0;
    return *this;
}

bool RenderedSnapshot::isValid() const noexcept {
    return !pngBytes_.isEmpty() && sha256_.size() == 32 &&
           pixelSize_.isValid();
}

QImage RenderedSnapshot::image() const {
    QMutexLocker locker(&decodedImageMutex_);
    if (image_.isNull() && isValid()) {
        QImage decoded = QImage::fromData(pngBytes_, "PNG");
        if (!decoded.isNull() && decoded.size() == pixelSize_) {
            if (decoded.format() != QImage::Format_ARGB32_Premultiplied) {
                decoded = decoded.convertToFormat(
                    QImage::Format_ARGB32_Premultiplied);
            }
            decoded.setDevicePixelRatio(1.0);
            image_ = std::move(decoded);
        }
    }
    return image_;
}

bool RenderedSnapshot::hasDecodedImage() const noexcept {
    QMutexLocker locker(&decodedImageMutex_);
    return !image_.isNull();
}

quint64 RenderedSnapshot::decodedImageByteSize() const noexcept {
    QMutexLocker locker(&decodedImageMutex_);
    if (image_.isNull()) {
        return 0;
    }
    return static_cast<quint64>(image_.sizeInBytes());
}

quint64 RenderedSnapshot::releaseDecodedImage() const noexcept {
    QMutexLocker locker(&decodedImageMutex_);
    const quint64 releasedBytes = image_.isNull()
                                      ? 0
                                      : static_cast<quint64>(
                                            image_.sizeInBytes());
    image_ = {};
    return releasedBytes;
}

const QByteArray& RenderedSnapshot::pngBytes() const noexcept {
    return pngBytes_;
}

const QByteArray& RenderedSnapshot::sha256() const noexcept {
    return sha256_;
}

QSize RenderedSnapshot::pixelSize() const noexcept {
    return pixelSize_;
}

quint64 RenderedSnapshot::revision() const noexcept {
    return revision_;
}

bool SnapshotRenderInput::isValid() const noexcept
{
    return !sessionId.isNull() && !sourceImage.isNull()
        && !cropRect.isEmpty() && sourceImage.rect().contains(cropRect);
}

SnapshotRenderInput SnapshotRenderer::freezeCurrent(
    const ScreenshotSession& session)
{
    SnapshotRenderInput input;
    input.sessionId = session.sessionId();
    input.sourceImage = session.sourceImage();
    input.cropRect = session.cropRect();
    input.annotations = session.annotations().annotationsInPaintOrder();
    input.revision = session.currentRevision();
    return input;
}

SnapshotRenderResult SnapshotRenderer::renderFrozen(
    SnapshotRenderInput input)
{
    SnapshotRenderResult result;
    result.sessionId = input.sessionId;
    result.revision = input.revision;
    if (!input.isValid()) {
        return result;
    }

    QImage canvas = input.sourceImage.copy(input.cropRect);
    if (canvas.format() != QImage::Format_ARGB32_Premultiplied) {
        canvas = canvas.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    canvas.setDevicePixelRatio(1.0);

    const QPoint cropOrigin = input.cropRect.topLeft();
    for (const Annotation& annotation : input.annotations) {
        if (!annotation.isValid() ||
            !annotation.bounds().intersects(QRectF(input.cropRect))) {
            continue;
        }
        if (annotation.type == AnnotationType::Mosaic) {
            rasterizeMosaic(&canvas, std::get<MosaicGeometry>(annotation.geometry),
                            annotation.style, cropOrigin);
        } else {
            drawVectorAnnotation(&canvas, annotation, cropOrigin);
        }
    }

    canvas.detach();
    QByteArray pngBytes = encodePng(canvas);
    if (pngBytes.isEmpty()) {
        return result;
    }
    QByteArray sha256 =
        QCryptographicHash::hash(pngBytes, QCryptographicHash::Sha256);
    result.snapshot = RenderedSnapshot(
        std::move(canvas), std::move(pngBytes), std::move(sha256),
        input.revision);
    return result;
}

RenderedSnapshot SnapshotRenderer::renderCurrent(
    const ScreenshotSession& session)
{
    SnapshotRenderResult result = renderFrozen(freezeCurrent(session));
    return std::move(result.snapshot);
}

}  // namespace snapask
