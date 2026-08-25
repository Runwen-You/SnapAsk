#include "platform/windows/WindowsDpi.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>

namespace snapask::platform::windows {

namespace {

[[nodiscard]] QString formatWindowsError(DWORD errorCode)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    QString result = length > 0 && message != nullptr
        ? QString::fromWCharArray(message, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Unknown Windows error");
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}

}  // namespace

DpiAwarenessResult WindowsDpi::enablePerMonitorV2(QString* error)
{
    if (error != nullptr) {
        error->clear();
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using SetContextFunction = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        const auto setContext = reinterpret_cast<SetContextFunction>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr) {
            SetLastError(ERROR_SUCCESS);
            if (setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
                return DpiAwarenessResult::PerMonitorV2Enabled;
            }

            const DWORD code = GetLastError();
            if (code == ERROR_ACCESS_DENIED) {
                return DpiAwarenessResult::AlreadyConfigured;
            }
            if (code != ERROR_INVALID_PARAMETER && error != nullptr) {
                *error = QStringLiteral("SetProcessDpiAwarenessContext failed (%1): %2")
                             .arg(code)
                             .arg(formatWindowsError(code));
            }
        }
    }

    const HRESULT perMonitorResult = SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    if (SUCCEEDED(perMonitorResult)) {
        return DpiAwarenessResult::PerMonitorV1Fallback;
    }
    if (perMonitorResult == E_ACCESSDENIED) {
        return DpiAwarenessResult::AlreadyConfigured;
    }

    SetLastError(ERROR_SUCCESS);
    if (SetProcessDPIAware() != FALSE) {
        return DpiAwarenessResult::SystemAwareFallback;
    }

    const DWORD code = GetLastError();
    if (error != nullptr) {
        *error = QStringLiteral("Windows DPI awareness could not be enabled (%1): %2")
                     .arg(code)
                     .arg(formatWindowsError(code));
    }
    return DpiAwarenessResult::Failed;
}

quint32 WindowsDpi::dpiForWindow(quintptr nativeWindowHandle) noexcept
{
    if (nativeWindowHandle == 0) {
        return 96;
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiFunction = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiFunction>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (getDpi != nullptr) {
            const UINT dpi = getDpi(reinterpret_cast<HWND>(nativeWindowHandle));
            if (dpi > 0) {
                return dpi;
            }
        }
    }
    return 96;
}

}  // namespace snapask::platform::windows
