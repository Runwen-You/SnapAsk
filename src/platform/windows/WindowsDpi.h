#pragma once

#include <QString>
#include <QtGlobal>

namespace snapask::platform::windows {

enum class DpiAwarenessResult {
    PerMonitorV2Enabled,
    AlreadyConfigured,
    PerMonitorV1Fallback,
    SystemAwareFallback,
    Failed,
};

class WindowsDpi final {
public:
    // Call before constructing QApplication. Uses documented APIs only and
    // degrades through Per-Monitor V1 and system awareness on older Windows.
    [[nodiscard]] static DpiAwarenessResult enablePerMonitorV2(QString* error = nullptr);

    [[nodiscard]] static quint32 dpiForWindow(quintptr nativeWindowHandle) noexcept;
};

}  // namespace snapask::platform::windows
