#include "platform/windows/WindowBackdrop.h"

#include <QColor>
#include <QOperatingSystemVersion>
#include <QPalette>
#include <QVariant>
#include <QWidget>
#include <QtCore/qt_windows.h>

#include <dwmapi.h>

namespace snapask::platform::windows {

namespace {

constexpr auto systemBackdropAttribute = static_cast<DWMWINDOWATTRIBUTE>(38);
constexpr auto windowCornerPreferenceAttribute = static_cast<DWMWINDOWATTRIBUTE>(33);
constexpr auto immersiveDarkModeAttribute = static_cast<DWMWINDOWATTRIBUTE>(20);

constexpr int backdropNone = 1;
constexpr int backdropMainWindow = 2;
constexpr int backdropTransientWindow = 3;
constexpr int cornerDefault = 0;
constexpr int cornerRound = 2;

constexpr char originalPaletteProperty[] = "snapask.backdrop.originalPalette";
constexpr char originalAutoFillProperty[] = "snapask.backdrop.originalAutoFill";
constexpr char originalTranslucentProperty[] = "snapask.backdrop.originalTranslucent";
constexpr char originalNoSystemBackgroundProperty[] =
    "snapask.backdrop.originalNoSystemBackground";
constexpr char stateSavedProperty[] = "snapask.backdrop.stateSaved";
constexpr char appliedModeProperty[] = "snapaskWindowBackdropMode";

void saveQtState(QWidget* window)
{
    if (window->property(stateSavedProperty).toBool()) {
        return;
    }
    window->setProperty(originalPaletteProperty, QVariant::fromValue(window->palette()));
    window->setProperty(originalAutoFillProperty, window->autoFillBackground());
    window->setProperty(
        originalTranslucentProperty,
        window->testAttribute(Qt::WA_TranslucentBackground));
    window->setProperty(
        originalNoSystemBackgroundProperty,
        window->testAttribute(Qt::WA_NoSystemBackground));
    window->setProperty(stateSavedProperty, true);
}

void setReadablePalette(
    QWidget* window,
    bool darkMode,
    bool highContrast,
    bool translucent)
{
    QPalette palette = window->palette();
    QColor background;
    QColor foreground;
    if (highContrast) {
        const COLORREF systemBackground = GetSysColor(COLOR_WINDOW);
        const COLORREF systemForeground = GetSysColor(COLOR_WINDOWTEXT);
        background = QColor(
            GetRValue(systemBackground),
            GetGValue(systemBackground),
            GetBValue(systemBackground));
        foreground = QColor(
            GetRValue(systemForeground),
            GetGValue(systemForeground),
            GetBValue(systemForeground));
    } else if (darkMode) {
        background = QColor(30, 30, 34, translucent ? 246 : 255);
        foreground = QColor(245, 245, 247);
    } else {
        background = QColor(248, 248, 250, translucent ? 246 : 255);
        foreground = QColor(24, 24, 28);
    }

    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::ButtonText, foreground);
    window->setPalette(palette);
    window->setAutoFillBackground(true);
    window->setAttribute(Qt::WA_TranslucentBackground, translucent);
    window->setAttribute(Qt::WA_NoSystemBackground, false);
}

void setNativeTransparentSurface(QWidget* window, bool darkMode)
{
    QPalette palette = window->palette();
    palette.setColor(QPalette::Window, QColor(0, 0, 0, 0));
    const QColor foreground = darkMode ? QColor(245, 245, 247) : QColor(24, 24, 28);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Text, foreground);
    window->setPalette(palette);
    window->setAutoFillBackground(true);
    window->setAttribute(Qt::WA_TranslucentBackground, true);
    // WA_TranslucentBackground implicitly enables WA_NoSystemBackground.
    // Re-enable backing-store clearing so translucent widgets replace their
    // previous pixels instead of blending over them during page switches or
    // streaming text updates.
    window->setAttribute(Qt::WA_NoSystemBackground, false);
}

[[nodiscard]] HWND nativeHandle(QWidget* window) noexcept
{
    return reinterpret_cast<HWND>(window->winId());
}

[[nodiscard]] WindowBackdropResult applyFallback(
    QWidget* window,
    const WindowBackdropOptions& options,
    BackdropFallbackReason reason,
    bool highContrast,
    bool remoteSession,
    bool compositionEnabled)
{
    const BackdropMode mode = WindowBackdrop::fallbackModeFor(
        highContrast,
        remoteSession,
        compositionEnabled,
        options.allowTranslucentFallback);
    const bool translucent = mode == BackdropMode::TranslucentFallback;
    setReadablePalette(window, options.darkMode, highContrast, translucent);
    window->setProperty(appliedModeProperty, static_cast<int>(mode));
    return {mode, reason, false};
}

}  // namespace

WindowBackdropResult WindowBackdrop::apply(
    QWidget* window,
    WindowBackdropOptions options)
{
    if (window == nullptr || !window->isWindow()) {
        return {
            BackdropMode::SolidFallback,
            BackdropFallbackReason::InvalidWindow,
            false,
        };
    }

    saveQtState(window);
    const bool highContrast = isHighContrastEnabled();
    const bool remoteSession = isRemoteSession();
    const bool compositionEnabled = isDesktopCompositionEnabled();

    if (options.preference == BackdropPreference::Solid) {
        WindowBackdropOptions solidOptions = options;
        solidOptions.allowTranslucentFallback = false;
        return applyFallback(
            window,
            solidOptions,
            BackdropFallbackReason::ExplicitSolid,
            highContrast,
            remoteSession,
            compositionEnabled);
    }
    if (highContrast) {
        return applyFallback(
            window,
            options,
            BackdropFallbackReason::HighContrast,
            true,
            remoteSession,
            compositionEnabled);
    }
    if (remoteSession) {
        return applyFallback(
            window,
            options,
            BackdropFallbackReason::RemoteSession,
            false,
            true,
            compositionEnabled);
    }
    if (!compositionEnabled) {
        return applyFallback(
            window,
            options,
            BackdropFallbackReason::CompositionDisabled,
            false,
            false,
            false);
    }
    if (!supportsSystemBackdrop()) {
        return applyFallback(
            window,
            options,
            BackdropFallbackReason::UnsupportedOperatingSystem,
            false,
            false,
            true);
    }

    const HWND handle = nativeHandle(window);
    const BOOL darkMode = options.darkMode ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(
        handle, immersiveDarkModeAttribute, &darkMode, sizeof(darkMode));

    if (options.roundedCorners) {
        const int cornerPreference = cornerRound;
        (void)DwmSetWindowAttribute(
            handle,
            windowCornerPreferenceAttribute,
            &cornerPreference,
            sizeof(cornerPreference));
    }

    const bool transient = options.preference == BackdropPreference::Transient;
    const int backdrop = transient ? backdropTransientWindow : backdropMainWindow;
    const HRESULT backdropResult = DwmSetWindowAttribute(
        handle, systemBackdropAttribute, &backdrop, sizeof(backdrop));
    if (FAILED(backdropResult)) {
        return applyFallback(
            window,
            options,
            BackdropFallbackReason::NativeCallFailed,
            false,
            false,
            true);
    }

    const MARGINS fullClientArea{-1, -1, -1, -1};
    (void)DwmExtendFrameIntoClientArea(handle, &fullClientArea);
    setNativeTransparentSurface(window, options.darkMode);
    const BackdropMode mode = transient
        ? BackdropMode::SystemTransient
        : BackdropMode::SystemMica;
    window->setProperty(appliedModeProperty, static_cast<int>(mode));
    return {mode, BackdropFallbackReason::None, true};
}

void WindowBackdrop::reset(QWidget* window)
{
    if (window == nullptr) {
        return;
    }

    if (window->isWindow() && window->internalWinId() != 0) {
        const HWND handle = reinterpret_cast<HWND>(window->internalWinId());
        const int noBackdrop = backdropNone;
        const int defaultCorner = cornerDefault;
        const MARGINS noExtension{0, 0, 0, 0};
        (void)DwmSetWindowAttribute(
            handle, systemBackdropAttribute, &noBackdrop, sizeof(noBackdrop));
        (void)DwmSetWindowAttribute(
            handle,
            windowCornerPreferenceAttribute,
            &defaultCorner,
            sizeof(defaultCorner));
        (void)DwmExtendFrameIntoClientArea(handle, &noExtension);
    }

    if (window->property(stateSavedProperty).toBool()) {
        const QVariant originalPalette = window->property(originalPaletteProperty);
        if (originalPalette.canConvert<QPalette>()) {
            window->setPalette(originalPalette.value<QPalette>());
        }
        window->setAutoFillBackground(
            window->property(originalAutoFillProperty).toBool());
        window->setAttribute(
            Qt::WA_TranslucentBackground,
            window->property(originalTranslucentProperty).toBool());
        window->setAttribute(
            Qt::WA_NoSystemBackground,
            window->property(originalNoSystemBackgroundProperty).toBool());
    }
    window->setProperty(originalPaletteProperty, {});
    window->setProperty(originalAutoFillProperty, {});
    window->setProperty(originalTranslucentProperty, {});
    window->setProperty(originalNoSystemBackgroundProperty, {});
    window->setProperty(stateSavedProperty, {});
    window->setProperty(appliedModeProperty, {});
}

bool WindowBackdrop::isHighContrastEnabled() noexcept
{
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    if (SystemParametersInfoW(
            SPI_GETHIGHCONTRAST,
            static_cast<UINT>(sizeof(highContrast)),
            &highContrast,
            0)
        == FALSE) {
        return false;
    }
    return (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool WindowBackdrop::isRemoteSession() noexcept
{
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
}

bool WindowBackdrop::isDesktopCompositionEnabled() noexcept
{
    BOOL enabled = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled == TRUE;
}

bool WindowBackdrop::supportsSystemBackdrop() noexcept
{
    return QOperatingSystemVersion::current()
        >= QOperatingSystemVersion::Windows11;
}

BackdropMode WindowBackdrop::fallbackModeFor(
    bool highContrast,
    bool remoteSession,
    bool compositionEnabled,
    bool allowTranslucentFallback) noexcept
{
    if (highContrast || remoteSession || !compositionEnabled
        || !allowTranslucentFallback) {
        return BackdropMode::SolidFallback;
    }
    return BackdropMode::TranslucentFallback;
}

}  // namespace snapask::platform::windows
