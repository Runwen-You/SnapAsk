#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

#include <array>

namespace snapask::capture {

enum class SelectionHandle {
    None,
    Move,
    NorthWest,
    North,
    NorthEast,
    East,
    SouthEast,
    South,
    SouthWest,
    West,
};

class SelectionModel final {
public:
    SelectionModel() = default;
    explicit SelectionModel(QRect boundsPx);

    void setBounds(QRect boundsPx);
    [[nodiscard]] const QRect& boundsPx() const noexcept;

    void setMinimumSize(QSize minimumSizePx);
    [[nodiscard]] QSize minimumSizePx() const noexcept;

    void setSelection(QRect selectionPx);
    void clearSelection() noexcept;
    [[nodiscard]] const QRect& selectionPx() const noexcept;
    [[nodiscard]] bool hasSelection() const noexcept;

    [[nodiscard]] bool beginCreate(const QPoint& pressPointPx);
    [[nodiscard]] bool beginTransform(SelectionHandle handle, const QPoint& pressPointPx);
    [[nodiscard]] bool updateInteraction(const QPoint& currentPointPx);
    void commitInteraction() noexcept;
    void cancelInteraction() noexcept;

    [[nodiscard]] bool interactionActive() const noexcept;
    [[nodiscard]] bool creatingSelection() const noexcept;
    [[nodiscard]] SelectionHandle activeHandle() const noexcept;

    [[nodiscard]] static SelectionHandle hitTest(
        const QRect& selectionPx,
        const QPoint& pointPx,
        int handleRadiusPx) noexcept;

    [[nodiscard]] static std::array<QPoint, 8> handleCenters(const QRect& selectionPx) noexcept;

private:
    [[nodiscard]] QPoint clampPoint(const QPoint& pointPx) const noexcept;
    [[nodiscard]] QRect clampMove(const QRect& rectPx) const noexcept;
    [[nodiscard]] QRect resizedRect(const QPoint& currentPointPx) const noexcept;

    QRect boundsPx_;
    QRect selectionPx_;
    QSize minimumSizePx_{1, 1};

    bool interactionActive_{false};
    bool creatingSelection_{false};
    SelectionHandle activeHandle_{SelectionHandle::None};
    QPoint pressPointPx_;
    QRect selectionBeforeInteractionPx_;
    QRect interactionStartPx_;
};

}  // namespace snapask::capture
