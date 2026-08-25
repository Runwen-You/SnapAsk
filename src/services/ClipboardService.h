#pragma once

#include <QString>

namespace snapask {

class RenderedSnapshot;

class ClipboardService final {
public:
    // Publishes the exact rendered image and its already-encoded PNG bytes. This
    // service deliberately has no overload accepting a session or QImage.
    [[nodiscard]] static bool copy(
        const RenderedSnapshot& snapshot,
        QString* error = nullptr);
};

}  // namespace snapask
