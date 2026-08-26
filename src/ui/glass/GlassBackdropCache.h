#pragma once

#include <QImage>
#include <QRect>
#include <QSize>

namespace snapask::ui::glass {

class GlassBackdropCache final {
public:
    GlassBackdropCache() = default;

    [[nodiscard]] const QImage& imageFor(
        const QImage& source,
        const QRect& sourceRect,
        const QSize& targetLogicalSize,
        qreal devicePixelRatio,
        bool darkMode,
        int blurRadius = 12,
        quint64 sourceRevision = 0);

    void invalidate();
    [[nodiscard]] quint64 generationCount() const noexcept;

private:
    struct Key {
        qint64 sourceCacheKey{0};
        QRect sourceRect;
        QSize targetLogicalSize;
        int devicePixelRatio100{100};
        int blurRadius{12};
        quint64 sourceRevision{0};
        bool darkMode{false};

        [[nodiscard]] bool operator==(const Key&) const = default;
    };

    [[nodiscard]] static QImage process(
        const QImage& source,
        const QRect& sourceRect,
        const QSize& targetLogicalSize,
        qreal devicePixelRatio,
        bool darkMode,
        int blurRadius);

    Key key_;
    QImage cachedImage_;
    quint64 generationCount_{0};
    bool hasKey_{false};
};

}  // namespace snapask::ui::glass
