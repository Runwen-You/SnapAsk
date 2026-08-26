#include "ui/glass/GlassBackdropCache.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace snapask::ui::glass {
namespace {

[[nodiscard]] QImage boxBlurPass(const QImage& source, const int radius)
{
    if (source.isNull() || radius <= 0) {
        return source;
    }
    const QImage input = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage output(input.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < input.height(); ++y) {
        auto* outputLine = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < input.width(); ++x) {
            quint64 red = 0;
            quint64 green = 0;
            quint64 blue = 0;
            quint64 alpha = 0;
            int count = 0;
            const int top = std::max(0, y - radius);
            const int bottom = std::min(input.height() - 1, y + radius);
            const int left = std::max(0, x - radius);
            const int right = std::min(input.width() - 1, x + radius);
            for (int sampleY = top; sampleY <= bottom; ++sampleY) {
                const auto* inputLine =
                    reinterpret_cast<const QRgb*>(input.constScanLine(sampleY));
                for (int sampleX = left; sampleX <= right; ++sampleX) {
                    const QRgb pixel = inputLine[sampleX];
                    red += static_cast<quint64>(qRed(pixel));
                    green += static_cast<quint64>(qGreen(pixel));
                    blue += static_cast<quint64>(qBlue(pixel));
                    alpha += static_cast<quint64>(qAlpha(pixel));
                    ++count;
                }
            }
            const quint64 divisor = static_cast<quint64>(std::max(1, count));
            outputLine[x] = qRgba(
                static_cast<int>(red / divisor),
                static_cast<int>(green / divisor),
                static_cast<int>(blue / divisor),
                static_cast<int>(alpha / divisor));
        }
    }
    return output;
}

void adjustSaturation(QImage& image, const qreal saturation)
{
    if (image.isNull()) {
        return;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const qreal luminance =
                (0.2126 * qRed(pixel))
                + (0.7152 * qGreen(pixel))
                + (0.0722 * qBlue(pixel));
            const auto channel = [luminance, saturation](const int value) {
                return std::clamp(
                    qRound(luminance + ((value - luminance) * saturation)),
                    0,
                    255);
            };
            line[x] = qRgba(
                channel(qRed(pixel)),
                channel(qGreen(pixel)),
                channel(qBlue(pixel)),
                qAlpha(pixel));
        }
    }
}

void applyEdgeLensing(QImage& image)
{
    if (image.width() < 8 || image.height() < 8) {
        return;
    }
    const QSize magnifiedSize(
        std::max(image.width() + 2, qRound(image.width() * 1.025)),
        std::max(image.height() + 2, qRound(image.height() * 1.025)));
    const QImage magnified = image.scaled(
        magnifiedSize,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);
    const QRect centered(
        (magnified.width() - image.width()) / 2,
        (magnified.height() - image.height()) / 2,
        image.width(),
        image.height());
    const QImage refracted = magnified.copy(centered);

    QPainter painter(&image);
    QPainterPath outer;
    outer.addRect(QRectF(image.rect()));
    const qreal horizontalInset = std::max(2.0, image.width() * 0.07);
    const qreal verticalInset = std::max(2.0, image.height() * 0.2);
    QPainterPath inner;
    inner.addRoundedRect(
        QRectF(image.rect()).adjusted(
            horizontalInset,
            verticalInset,
            -horizontalInset,
            -verticalInset),
        verticalInset,
        verticalInset);
    painter.setClipPath(outer.subtracted(inner));
    painter.setOpacity(0.22);
    painter.drawImage(QPoint(0, 0), refracted);
}

}  // namespace

const QImage& GlassBackdropCache::imageFor(
    const QImage& source,
    const QRect& sourceRect,
    const QSize& targetLogicalSize,
    const qreal devicePixelRatio,
    const bool darkMode,
    const int blurRadius,
    const quint64 sourceRevision)
{
    const qreal boundedRatio = std::clamp(devicePixelRatio, 1.0, 4.0);
    const Key requested{
        source.cacheKey(),
        sourceRect,
        targetLogicalSize,
        qRound(boundedRatio * 100.0),
        std::clamp(blurRadius, 0, 32),
        sourceRevision,
        darkMode,
    };
    if (!hasKey_ || !(requested == key_)) {
        cachedImage_ = process(
            source,
            sourceRect,
            targetLogicalSize,
            boundedRatio,
            darkMode,
            requested.blurRadius);
        key_ = requested;
        hasKey_ = true;
        ++generationCount_;
    }
    return cachedImage_;
}

void GlassBackdropCache::invalidate()
{
    cachedImage_ = {};
    hasKey_ = false;
}

quint64 GlassBackdropCache::generationCount() const noexcept
{
    return generationCount_;
}

QImage GlassBackdropCache::process(
    const QImage& source,
    const QRect& sourceRect,
    const QSize& targetLogicalSize,
    const qreal devicePixelRatio,
    const bool darkMode,
    const int blurRadius)
{
    if (source.isNull() || !targetLogicalSize.isValid()
        || targetLogicalSize.isEmpty()) {
        return {};
    }
    QRect crop = sourceRect.isValid() && !sourceRect.isEmpty()
        ? sourceRect.normalized().intersected(source.rect())
        : source.rect();
    if (crop.isEmpty()) {
        return {};
    }

    const int margin = std::max(6, blurRadius * 2);
    crop = crop.adjusted(-margin, -margin, margin, margin)
               .intersected(source.rect());
    const QSize targetPhysical(
        std::max(1, qRound(targetLogicalSize.width() * devicePixelRatio)),
        std::max(1, qRound(targetLogicalSize.height() * devicePixelRatio)));
    const QSize downsampledSize(
        std::max(1, targetPhysical.width() / 4),
        std::max(1, targetPhysical.height() / 4));
    QImage processed = source.copy(crop).scaled(
        downsampledSize,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);
    const int reducedRadius = std::clamp(
        qRound((blurRadius * devicePixelRatio) / 4.0),
        1,
        5);
    processed = boxBlurPass(processed, reducedRadius);
    processed = boxBlurPass(processed, reducedRadius);
    adjustSaturation(processed, darkMode ? 1.04 : 1.08);
    processed = processed.scaled(
        targetPhysical,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);
    applyEdgeLensing(processed);
    processed.setDevicePixelRatio(devicePixelRatio);
    return processed;
}

}  // namespace snapask::ui::glass
