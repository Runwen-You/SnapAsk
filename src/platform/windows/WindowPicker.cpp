#include "platform/windows/WindowPicker.h"

#include "platform/windows/MonitorCoordinateMapper.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

#include <array>
#include <algorithm>
#include <string>

namespace snapask::platform::windows {

namespace {

struct PickerContext final {
    QPoint pointPx;
    bool matchPoint{false};
    bool excludeCurrentProcess{true};
    DWORD currentProcessId{GetCurrentProcessId()};
    const QSet<quintptr>* excludedHandles{nullptr};
    QVector<TopLevelWindow> results;
};

[[nodiscard]] bool isCloaked(HWND window) noexcept
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(
               window,
               DWMWA_CLOAKED,
               &cloaked,
               sizeof(cloaked)))
        && cloaked != 0;
}

[[nodiscard]] QRect frameRect(HWND window) noexcept
{
    RECT nativeRect{};
    if (FAILED(DwmGetWindowAttribute(
            window,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &nativeRect,
            sizeof(nativeRect)))) {
        if (GetWindowRect(window, &nativeRect) == FALSE) {
            return {};
        }
    }
    return MonitorCoordinateMapper::fromNativeEdges(
        nativeRect.left,
        nativeRect.top,
        nativeRect.right,
        nativeRect.bottom);
}

[[nodiscard]] QString windowTitle(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring buffer(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, buffer.data(), length + 1);
    return copied > 0 ? QString::fromWCharArray(buffer.data(), copied) : QString{};
}

[[nodiscard]] QString windowClass(HWND window)
{
    std::array<wchar_t, 256> buffer{};
    const int copied = GetClassNameW(window, buffer.data(), static_cast<int>(buffer.size()));
    return copied > 0 ? QString::fromWCharArray(buffer.data(), copied) : QString{};
}

[[nodiscard]] std::optional<TopLevelWindow> describeWindow(
    HWND candidate,
    const PickerContext& context)
{
    if (candidate == nullptr) {
        return std::nullopt;
    }

    const HWND window = GetAncestor(candidate, GA_ROOT);
    const quintptr handle = reinterpret_cast<quintptr>(window);
    if (window == nullptr || window == GetDesktopWindow() || window == GetShellWindow()
        || IsWindowVisible(window) == FALSE || IsIconic(window) != FALSE
        || isCloaked(window)
        || (context.excludedHandles != nullptr && context.excludedHandles->contains(handle))) {
        return std::nullopt;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (context.excludeCurrentProcess && processId == context.currentProcessId) {
        return std::nullopt;
    }

    const QRect rect = frameRect(window);
    if (!rect.isValid() || rect.isEmpty()) {
        return std::nullopt;
    }
    if (context.matchPoint && !rect.contains(context.pointPx)) {
        return std::nullopt;
    }

    return TopLevelWindow{
        handle,
        rect,
        windowTitle(window),
        windowClass(window),
        static_cast<quint32>(processId),
    };
}

BOOL CALLBACK enumerateWindows(HWND window, LPARAM data)
{
    auto* context = reinterpret_cast<PickerContext*>(data);
    if (context == nullptr) {
        return FALSE;
    }

    if (const auto description = describeWindow(window, *context); description.has_value()) {
        context->results.push_back(*description);
        if (context->matchPoint) {
            return FALSE;
        }
    }
    return TRUE;
}

}  // namespace

bool TopLevelWindow::isValid() const noexcept
{
    return nativeHandle != 0 && framePx.isValid() && !framePx.isEmpty();
}

std::optional<TopLevelWindow> WindowPicker::windowAt(
    const QPoint& desktopPointPx,
    const QSet<quintptr>& excludedHandles,
    bool excludeCurrentProcess) const
{
    PickerContext context;
    context.pointPx = desktopPointPx;
    context.matchPoint = true;
    context.excludeCurrentProcess = excludeCurrentProcess;
    context.excludedHandles = &excludedHandles;

    // Fast path uses the documented point -> root-window chain. When the overlay
    // itself is hit/excluded, EnumWindows below finds the next eligible Z-order item.
    const POINT nativePoint{desktopPointPx.x(), desktopPointPx.y()};
    const HWND directWindow = GetAncestor(WindowFromPoint(nativePoint), GA_ROOT);
    if (const auto direct = describeWindow(directWindow, context); direct.has_value()) {
        return direct;
    }

    EnumWindows(enumerateWindows, reinterpret_cast<LPARAM>(&context));
    if (context.results.isEmpty()) {
        return std::nullopt;
    }
    return context.results.front();
}

QVector<TopLevelWindow> WindowPicker::enumerate(
    const QSet<quintptr>& excludedHandles,
    bool excludeCurrentProcess) const
{
    PickerContext context;
    context.excludeCurrentProcess = excludeCurrentProcess;
    context.excludedHandles = &excludedHandles;
    EnumWindows(enumerateWindows, reinterpret_cast<LPARAM>(&context));
    return context.results;
}

}  // namespace snapask::platform::windows
