#include "platform/windows/MonitorCoordinateMapper.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>

#include <QVector>

#include <limits>
#include <utility>

namespace snapask::platform::windows {

namespace {

struct EnumerationContext final {
    QVector<snapask::capture::MonitorGeometry> monitors;
};

[[nodiscard]] quint32 safeLongToDpi(UINT value) noexcept
{
    return value > 0 ? static_cast<quint32>(value) : 96U;
}

BOOL CALLBACK collectMonitor(HMONITOR monitorHandle, HDC, LPRECT, LPARAM data)
{
    auto* context = reinterpret_cast<EnumerationContext*>(data);
    if (context == nullptr) {
        return FALSE;
    }

    MONITORINFOEXW nativeInfo{};
    nativeInfo.cbSize = sizeof(nativeInfo);
    if (GetMonitorInfoW(monitorHandle, &nativeInfo) == FALSE) {
        return TRUE;
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitorHandle, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiX = 96;
        dpiY = 96;
    }

    snapask::capture::MonitorGeometry geometry;
    geometry.deviceId = QString::fromWCharArray(nativeInfo.szDevice);
    geometry.geometryPx = MonitorCoordinateMapper::fromNativeEdges(
        nativeInfo.rcMonitor.left,
        nativeInfo.rcMonitor.top,
        nativeInfo.rcMonitor.right,
        nativeInfo.rcMonitor.bottom);
    geometry.workAreaPx = MonitorCoordinateMapper::fromNativeEdges(
        nativeInfo.rcWork.left,
        nativeInfo.rcWork.top,
        nativeInfo.rcWork.right,
        nativeInfo.rcWork.bottom);
    geometry.dpiX = safeLongToDpi(dpiX);
    geometry.dpiY = safeLongToDpi(dpiY);
    geometry.primary = (nativeInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    context->monitors.push_back(std::move(geometry));
    return TRUE;
}

}  // namespace

snapask::capture::MonitorLayout MonitorCoordinateMapper::query(QString* error)
{
    if (error != nullptr) {
        error->clear();
    }

    EnumerationContext context;
    SetLastError(ERROR_SUCCESS);
    if (EnumDisplayMonitors(
            nullptr,
            nullptr,
            collectMonitor,
            reinterpret_cast<LPARAM>(&context))
        == FALSE) {
        if (error != nullptr) {
            *error = QStringLiteral("EnumDisplayMonitors failed with Windows error %1.")
                         .arg(GetLastError());
        }
        return {};
    }

    const QRect virtualDesktop = virtualDesktopRectPx();
    snapask::capture::MonitorLayout layout(virtualDesktop, std::move(context.monitors));
    if (!layout.isValid() && error != nullptr) {
        *error = QStringLiteral("Windows returned an invalid or empty physical monitor layout.");
    }
    return layout;
}

QRect MonitorCoordinateMapper::virtualDesktopRectPx() noexcept
{
    return {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

QPoint MonitorCoordinateMapper::cursorPositionPx(bool* ok) noexcept
{
    POINT point{};
    const bool success = GetCursorPos(&point) != FALSE;
    if (ok != nullptr) {
        *ok = success;
    }
    if (!success) {
        return {};
    }
    return {point.x, point.y};
}

QRect MonitorCoordinateMapper::fromNativeEdges(
    long left,
    long top,
    long right,
    long bottom) noexcept
{
    const qint64 width = static_cast<qint64>(right) - static_cast<qint64>(left);
    const qint64 height = static_cast<qint64>(bottom) - static_cast<qint64>(top);
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max()
        || height > std::numeric_limits<int>::max()) {
        return {};
    }
    return {
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(width),
        static_cast<int>(height),
    };
}

}  // namespace snapask::platform::windows
