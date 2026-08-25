#pragma once

class QWidget;

namespace snapask::platform::windows {

enum class BackdropPreference {
    Automatic,
    Mica,
    Transient,
    Solid,
};

enum class BackdropMode {
    SystemMica,
    SystemTransient,
    TranslucentFallback,
    SolidFallback,
};

enum class BackdropFallbackReason {
    None,
    InvalidWindow,
    HighContrast,
    RemoteSession,
    CompositionDisabled,
    UnsupportedOperatingSystem,
    NativeCallFailed,
    ExplicitSolid,
};

struct WindowBackdropOptions {
    BackdropPreference preference{BackdropPreference::Automatic};
    bool darkMode{false};
    bool roundedCorners{true};
    bool allowTranslucentFallback{true};
};

struct WindowBackdropResult {
    BackdropMode mode{BackdropMode::SolidFallback};
    BackdropFallbackReason fallbackReason{BackdropFallbackReason::None};
    bool nativeBackdropApplied{false};
};

// Applies only documented DWM attributes. If the attributes are unavailable,
// the widget receives a high-opacity Qt palette (or an opaque system palette
// in high-contrast/remote sessions) so text remains readable.
class WindowBackdrop final {
public:
    WindowBackdrop() = delete;

    [[nodiscard]] static WindowBackdropResult apply(
        QWidget* window,
        WindowBackdropOptions options = {});
    static void reset(QWidget* window);

    [[nodiscard]] static bool isHighContrastEnabled() noexcept;
    [[nodiscard]] static bool isRemoteSession() noexcept;
    [[nodiscard]] static bool isDesktopCompositionEnabled() noexcept;
    [[nodiscard]] static bool supportsSystemBackdrop() noexcept;

    // Deterministic policy helper used by tests and by apply(). System blur is
    // never emulated when accessibility or remote-session constraints apply.
    [[nodiscard]] static BackdropMode fallbackModeFor(
        bool highContrast,
        bool remoteSession,
        bool compositionEnabled,
        bool allowTranslucentFallback) noexcept;
};

}  // namespace snapask::platform::windows
