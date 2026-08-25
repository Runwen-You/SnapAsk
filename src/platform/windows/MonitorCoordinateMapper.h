#pragma once

#include "domain/capture/CaptureGeometry.h"

#include <QPoint>
#include <QRect>
#include <QString>

namespace snapask::platform::windows {

class MonitorCoordinateMapper final {
public:
    [[nodiscard]] static snapask::capture::MonitorLayout query(QString* error = nullptr);
    [[nodiscard]] static QRect virtualDesktopRectPx() noexcept;
    [[nodiscard]] static QPoint cursorPositionPx(bool* ok = nullptr) noexcept;

    // Converts Win32's half-open RECT edge convention without importing Windows
    // types into domain or UI headers.
    [[nodiscard]] static QRect fromNativeEdges(
        long left,
        long top,
        long right,
        long bottom) noexcept;
};

}  // namespace snapask::platform::windows
