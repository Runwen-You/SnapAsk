#pragma once

#include "domain/capture/CaptureGeometry.h"

#include <QDateTime>
#include <QImage>
#include <QMetaType>
#include <QRect>
#include <QString>

#include <optional>

namespace snapask::capture {

struct CaptureSelection final {
    QRect desktopRectPx;
    QImage image;

    [[nodiscard]] bool isValid() const noexcept;
};

class CaptureFrame final {
public:
    CaptureFrame() = default;
    CaptureFrame(
        QImage image,
        QRect desktopRectPx,
        MonitorLayout monitorLayout,
        QDateTime capturedAtUtc = QDateTime::currentDateTimeUtc());

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const QImage& image() const noexcept;
    [[nodiscard]] const QRect& desktopRectPx() const noexcept;
    [[nodiscard]] const MonitorLayout& monitorLayout() const noexcept;
    [[nodiscard]] const QDateTime& capturedAtUtc() const noexcept;

    // Crops pixels in virtual-desktop physical coordinates. PNG serialization
    // belongs exclusively to SnapshotRenderer after the screenshot session and
    // its annotations have been frozen into a canonical revision.
    [[nodiscard]] std::optional<CaptureSelection> cropPixels(
        const QRect& requestedDesktopRectPx,
        QString* error = nullptr) const;

private:
    QImage image_;
    QRect desktopRectPx_;
    MonitorLayout monitorLayout_;
    QDateTime capturedAtUtc_;
};

}  // namespace snapask::capture

Q_DECLARE_METATYPE(snapask::capture::CaptureSelection)
