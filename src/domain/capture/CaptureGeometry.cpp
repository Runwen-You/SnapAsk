#include "domain/capture/CaptureGeometry.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace snapask::capture {

bool MonitorGeometry::isValid() const noexcept
{
    return geometryPx.isValid() && !geometryPx.isEmpty() && workAreaPx.isValid()
        && dpiX > 0 && dpiY > 0;
}

qreal MonitorGeometry::scaleX() const noexcept
{
    return static_cast<qreal>(dpiX) / 96.0;
}

qreal MonitorGeometry::scaleY() const noexcept
{
    return static_cast<qreal>(dpiY) / 96.0;
}

MonitorLayout::MonitorLayout(QRect virtualDesktopPx, QVector<MonitorGeometry> monitors)
    : virtualDesktopPx_(virtualDesktopPx)
    , monitors_(std::move(monitors))
{
}

bool MonitorLayout::isValid() const noexcept
{
    if (!virtualDesktopPx_.isValid() || virtualDesktopPx_.isEmpty() || monitors_.isEmpty()) {
        return false;
    }

    return std::all_of(monitors_.cbegin(), monitors_.cend(), [](const MonitorGeometry& monitor) {
        return monitor.isValid();
    });
}

const QRect& MonitorLayout::virtualDesktopPx() const noexcept
{
    return virtualDesktopPx_;
}

const QVector<MonitorGeometry>& MonitorLayout::monitors() const noexcept
{
    return monitors_;
}

QPoint MonitorLayout::desktopToImage(const QPoint& desktopPointPx) const noexcept
{
    return desktopPointPx - virtualDesktopPx_.topLeft();
}

QRect MonitorLayout::desktopToImage(const QRect& desktopRectPx) const noexcept
{
    return desktopRectPx.translated(-virtualDesktopPx_.topLeft());
}

QPoint MonitorLayout::imageToDesktop(const QPoint& imagePointPx) const noexcept
{
    return imagePointPx + virtualDesktopPx_.topLeft();
}

QRect MonitorLayout::imageToDesktop(const QRect& imageRectPx) const noexcept
{
    return imageRectPx.translated(virtualDesktopPx_.topLeft());
}

QRect MonitorLayout::intersectWithDesktop(const QRect& desktopRectPx) const noexcept
{
    return desktopRectPx.intersected(virtualDesktopPx_);
}

QRect MonitorLayout::clampToDesktop(const QRect& desktopRectPx) const noexcept
{
    if (!desktopRectPx.isValid() || virtualDesktopPx_.isEmpty()) {
        return {};
    }

    QRect result(
        desktopRectPx.topLeft(),
        QSize(
            std::min(desktopRectPx.width(), virtualDesktopPx_.width()),
            std::min(desktopRectPx.height(), virtualDesktopPx_.height())));
    if (result.left() < virtualDesktopPx_.left()) {
        result.moveLeft(virtualDesktopPx_.left());
    }
    if (result.right() > virtualDesktopPx_.right()) {
        result.moveRight(virtualDesktopPx_.right());
    }
    if (result.top() < virtualDesktopPx_.top()) {
        result.moveTop(virtualDesktopPx_.top());
    }
    if (result.bottom() > virtualDesktopPx_.bottom()) {
        result.moveBottom(virtualDesktopPx_.bottom());
    }
    return result;
}

const MonitorGeometry* MonitorLayout::monitorAt(const QPoint& desktopPointPx) const noexcept
{
    for (const MonitorGeometry& monitor : monitors_) {
        if (monitor.geometryPx.contains(desktopPointPx)) {
            return &monitor;
        }
    }
    return nullptr;
}

const MonitorGeometry* MonitorLayout::monitorForRect(const QRect& desktopRectPx) const noexcept
{
    const MonitorGeometry* best = nullptr;
    qint64 bestArea = -1;

    for (const MonitorGeometry& monitor : monitors_) {
        const QRect overlap = desktopRectPx.intersected(monitor.geometryPx);
        const qint64 area = overlap.isEmpty()
            ? 0
            : static_cast<qint64>(overlap.width()) * static_cast<qint64>(overlap.height());
        if (area > bestArea) {
            bestArea = area;
            best = &monitor;
        }
    }

    return bestArea > 0 ? best : nullptr;
}

}  // namespace snapask::capture
