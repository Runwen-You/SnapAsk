#pragma once

#include "domain/capture/IScreenCapture.h"

namespace snapask::platform::windows {

class GdiScreenCapture final : public snapask::capture::IScreenCapture {
public:
    [[nodiscard]] std::optional<snapask::capture::CaptureFrame> captureVirtualDesktop(
        QString* error = nullptr) const override;
};

}  // namespace snapask::platform::windows
