#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

namespace snapask::capture {

// All coordinates in this module are Win32 virtual-desktop physical pixels.
// Qt logical coordinates must be converted at the UI boundary before use here.
struct MonitorGeometry final {
    QString deviceId;
    QRect geometryPx;
    QRect workAreaPx;
    quint32 dpiX{96};
    quint32 dpiY{96};
    bool primary{false};

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] qreal scaleX() const noexcept;
    [[nodiscard]] qreal scaleY() const noexcept;
};

class MonitorLayout final {
public:
    MonitorLayout() = default;
    MonitorLayout(QRect virtualDesktopPx, QVector<MonitorGeometry> monitors);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const QRect& virtualDesktopPx() const noexcept;
    [[nodiscard]] const QVector<MonitorGeometry>& monitors() const noexcept;

    [[nodiscard]] QPoint desktopToImage(const QPoint& desktopPointPx) const noexcept;
    [[nodiscard]] QRect desktopToImage(const QRect& desktopRectPx) const noexcept;
    [[nodiscard]] QPoint imageToDesktop(const QPoint& imagePointPx) const noexcept;
    [[nodiscard]] QRect imageToDesktop(const QRect& imageRectPx) const noexcept;

    [[nodiscard]] QRect intersectWithDesktop(const QRect& desktopRectPx) const noexcept;
    [[nodiscard]] QRect clampToDesktop(const QRect& desktopRectPx) const noexcept;

    [[nodiscard]] const MonitorGeometry* monitorAt(const QPoint& desktopPointPx) const noexcept;
    [[nodiscard]] const MonitorGeometry* monitorForRect(const QRect& desktopRectPx) const noexcept;

private:
    QRect virtualDesktopPx_;
    QVector<MonitorGeometry> monitors_;
};

}  // namespace snapask::capture
