#pragma once

#include <QString>

namespace snapask {

class RenderedSnapshot;

class SaveService final {
public:
    // Writes the exact PNG bytes already produced by SnapshotRenderer. This
    // service deliberately has no overload accepting a session or QImage.
    [[nodiscard]] static bool savePng(
        const RenderedSnapshot& snapshot,
        const QString& filePath,
        QString* error = nullptr);
};

}  // namespace snapask
