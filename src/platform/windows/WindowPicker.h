#pragma once

#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace snapask::platform::windows {

struct TopLevelWindow final {
    quintptr nativeHandle{0};
    QRect framePx;
    QString title;
    QString className;
    quint32 processId{0};

    [[nodiscard]] bool isValid() const noexcept;
};

class WindowPicker final {
public:
    // Returns the first eligible top-level window in Win32 Z order at a physical
    // desktop point. Exclusions make it possible to look through the capture overlay.
    [[nodiscard]] std::optional<TopLevelWindow> windowAt(
        const QPoint& desktopPointPx,
        const QSet<quintptr>& excludedHandles = {},
        bool excludeCurrentProcess = true) const;

    [[nodiscard]] QVector<TopLevelWindow> enumerate(
        const QSet<quintptr>& excludedHandles = {},
        bool excludeCurrentProcess = true) const;
};

}  // namespace snapask::platform::windows
