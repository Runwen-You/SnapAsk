#pragma once

#include "domain/capture/CaptureFrame.h"

#include <QString>

#include <optional>

namespace snapask::capture {

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    [[nodiscard]] virtual std::optional<CaptureFrame> captureVirtualDesktop(
        QString* error = nullptr) const = 0;
};

}  // namespace snapask::capture
