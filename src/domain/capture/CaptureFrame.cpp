#include "domain/capture/CaptureFrame.h"

#include <utility>

namespace snapask::capture {

bool CaptureSelection::isValid() const noexcept
{
    return desktopRectPx.isValid() && !desktopRectPx.isEmpty()
        && !image.isNull() && image.size() == desktopRectPx.size();
}

CaptureFrame::CaptureFrame(
    QImage image,
    QRect desktopRectPx,
    MonitorLayout monitorLayout,
    QDateTime capturedAtUtc)
    : image_(std::move(image))
    , desktopRectPx_(desktopRectPx)
    , monitorLayout_(std::move(monitorLayout))
    , capturedAtUtc_(std::move(capturedAtUtc))
{
}

bool CaptureFrame::isValid() const noexcept
{
    return !image_.isNull() && desktopRectPx_.isValid() && !desktopRectPx_.isEmpty()
        && image_.size() == desktopRectPx_.size() && monitorLayout_.isValid()
        && monitorLayout_.virtualDesktopPx() == desktopRectPx_;
}

const QImage& CaptureFrame::image() const noexcept
{
    return image_;
}

const QRect& CaptureFrame::desktopRectPx() const noexcept
{
    return desktopRectPx_;
}

const MonitorLayout& CaptureFrame::monitorLayout() const noexcept
{
    return monitorLayout_;
}

const QDateTime& CaptureFrame::capturedAtUtc() const noexcept
{
    return capturedAtUtc_;
}

std::optional<CaptureSelection> CaptureFrame::cropPixels(
    const QRect& requestedDesktopRectPx,
    QString* error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("The captured desktop frame is invalid.");
        }
        return std::nullopt;
    }

    const QRect clippedDesktopRect = requestedDesktopRectPx.intersected(desktopRectPx_);
    if (!clippedDesktopRect.isValid() || clippedDesktopRect.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected rectangle does not intersect the captured desktop.");
        }
        return std::nullopt;
    }

    const QRect imageRect = monitorLayout_.desktopToImage(clippedDesktopRect);
    QImage cropped = image_.copy(imageRect);
    if (cropped.isNull() || cropped.size() != clippedDesktopRect.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected pixels could not be copied from the captured desktop.");
        }
        return std::nullopt;
    }

    return CaptureSelection{clippedDesktopRect, std::move(cropped)};
}

}  // namespace snapask::capture
