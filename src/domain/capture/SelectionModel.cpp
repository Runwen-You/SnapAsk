#include "domain/capture/SelectionModel.h"

#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace snapask::capture {

namespace {

[[nodiscard]] bool changesLeft(SelectionHandle handle) noexcept
{
    return handle == SelectionHandle::West || handle == SelectionHandle::NorthWest
        || handle == SelectionHandle::SouthWest;
}

[[nodiscard]] bool changesRight(SelectionHandle handle) noexcept
{
    return handle == SelectionHandle::East || handle == SelectionHandle::NorthEast
        || handle == SelectionHandle::SouthEast;
}

[[nodiscard]] bool changesTop(SelectionHandle handle) noexcept
{
    return handle == SelectionHandle::North || handle == SelectionHandle::NorthWest
        || handle == SelectionHandle::NorthEast;
}

[[nodiscard]] bool changesBottom(SelectionHandle handle) noexcept
{
    return handle == SelectionHandle::South || handle == SelectionHandle::SouthWest
        || handle == SelectionHandle::SouthEast;
}

[[nodiscard]] qint64 squaredDistance(const QPoint& left, const QPoint& right) noexcept
{
    const qint64 dx = static_cast<qint64>(left.x()) - static_cast<qint64>(right.x());
    const qint64 dy = static_cast<qint64>(left.y()) - static_cast<qint64>(right.y());
    return dx * dx + dy * dy;
}

}  // namespace

SelectionModel::SelectionModel(QRect boundsPx)
    : boundsPx_(boundsPx)
{
}

void SelectionModel::setBounds(QRect boundsPx)
{
    if (interactionActive_) {
        cancelInteraction();
    }
    boundsPx_ = boundsPx;
    if (hasSelection()) {
        selectionPx_ = clampMove(selectionPx_.intersected(boundsPx_));
    }
}

const QRect& SelectionModel::boundsPx() const noexcept
{
    return boundsPx_;
}

void SelectionModel::setMinimumSize(QSize minimumSizePx)
{
    minimumSizePx_.setWidth(std::max(1, minimumSizePx.width()));
    minimumSizePx_.setHeight(std::max(1, minimumSizePx.height()));
}

QSize SelectionModel::minimumSizePx() const noexcept
{
    return minimumSizePx_;
}

void SelectionModel::setSelection(QRect selectionPx)
{
    if (interactionActive_) {
        cancelInteraction();
    }
    if (!selectionPx.isValid() || selectionPx.isEmpty() || boundsPx_.isEmpty()) {
        selectionPx_ = {};
        return;
    }
    selectionPx_ = selectionPx.intersected(boundsPx_);
}

void SelectionModel::clearSelection() noexcept
{
    interactionActive_ = false;
    creatingSelection_ = false;
    activeHandle_ = SelectionHandle::None;
    selectionPx_ = {};
    selectionBeforeInteractionPx_ = {};
    interactionStartPx_ = {};
}

const QRect& SelectionModel::selectionPx() const noexcept
{
    return selectionPx_;
}

bool SelectionModel::hasSelection() const noexcept
{
    return selectionPx_.isValid() && !selectionPx_.isEmpty();
}

bool SelectionModel::beginCreate(const QPoint& pressPointPx)
{
    if (!boundsPx_.isValid() || boundsPx_.isEmpty()) {
        return false;
    }

    selectionBeforeInteractionPx_ = selectionPx_;
    pressPointPx_ = clampPoint(pressPointPx);
    interactionStartPx_ = QRect(pressPointPx_, QSize(1, 1));
    selectionPx_ = interactionStartPx_;
    activeHandle_ = SelectionHandle::None;
    creatingSelection_ = true;
    interactionActive_ = true;
    return true;
}

bool SelectionModel::beginTransform(SelectionHandle handle, const QPoint& pressPointPx)
{
    if (!hasSelection() || handle == SelectionHandle::None || !boundsPx_.isValid()) {
        return false;
    }

    selectionBeforeInteractionPx_ = selectionPx_;
    interactionStartPx_ = selectionPx_;
    pressPointPx_ = clampPoint(pressPointPx);
    activeHandle_ = handle;
    creatingSelection_ = false;
    interactionActive_ = true;
    return true;
}

bool SelectionModel::updateInteraction(const QPoint& currentPointPx)
{
    if (!interactionActive_) {
        return false;
    }

    const QRect previous = selectionPx_;
    const QPoint current = clampPoint(currentPointPx);
    if (creatingSelection_) {
        // QRect::normalized() adjusts negative-width rectangles around its
        // inclusive right/bottom edges. Build ordered endpoints explicitly so a
        // reverse drag selects the exact same physical pixels as a forward drag.
        selectionPx_ = QRect(
            QPoint(
                std::min(pressPointPx_.x(), current.x()),
                std::min(pressPointPx_.y(), current.y())),
            QPoint(
                std::max(pressPointPx_.x(), current.x()),
                std::max(pressPointPx_.y(), current.y())))
                           .intersected(boundsPx_);
    } else if (activeHandle_ == SelectionHandle::Move) {
        selectionPx_ = clampMove(interactionStartPx_.translated(current - pressPointPx_));
    } else {
        selectionPx_ = resizedRect(current);
    }
    return selectionPx_ != previous;
}

void SelectionModel::commitInteraction() noexcept
{
    interactionActive_ = false;
    creatingSelection_ = false;
    activeHandle_ = SelectionHandle::None;
    selectionBeforeInteractionPx_ = {};
    interactionStartPx_ = {};
}

void SelectionModel::cancelInteraction() noexcept
{
    if (!interactionActive_) {
        return;
    }
    selectionPx_ = selectionBeforeInteractionPx_;
    interactionActive_ = false;
    creatingSelection_ = false;
    activeHandle_ = SelectionHandle::None;
    selectionBeforeInteractionPx_ = {};
    interactionStartPx_ = {};
}

bool SelectionModel::interactionActive() const noexcept
{
    return interactionActive_;
}

bool SelectionModel::creatingSelection() const noexcept
{
    return interactionActive_ && creatingSelection_;
}

SelectionHandle SelectionModel::activeHandle() const noexcept
{
    return activeHandle_;
}

SelectionHandle SelectionModel::hitTest(
    const QRect& selectionPx,
    const QPoint& pointPx,
    int handleRadiusPx) noexcept
{
    if (!selectionPx.isValid() || selectionPx.isEmpty()) {
        return SelectionHandle::None;
    }

    const int radius = std::max(1, handleRadiusPx);
    const qint64 radiusSquared = static_cast<qint64>(radius) * static_cast<qint64>(radius);
    constexpr std::array<SelectionHandle, 8> handles{
        SelectionHandle::NorthWest,
        SelectionHandle::North,
        SelectionHandle::NorthEast,
        SelectionHandle::East,
        SelectionHandle::SouthEast,
        SelectionHandle::South,
        SelectionHandle::SouthWest,
        SelectionHandle::West,
    };
    const auto centers = handleCenters(selectionPx);

    for (std::size_t index = 0; index < handles.size(); ++index) {
        if (squaredDistance(centers[index], pointPx) <= radiusSquared) {
            return handles[index];
        }
    }

    return selectionPx.contains(pointPx) ? SelectionHandle::Move : SelectionHandle::None;
}

std::array<QPoint, 8> SelectionModel::handleCenters(const QRect& selectionPx) noexcept
{
    const int horizontalCenter = selectionPx.left() + ((selectionPx.width() - 1) / 2);
    const int verticalCenter = selectionPx.top() + ((selectionPx.height() - 1) / 2);
    return {
        selectionPx.topLeft(),
        QPoint(horizontalCenter, selectionPx.top()),
        selectionPx.topRight(),
        QPoint(selectionPx.right(), verticalCenter),
        selectionPx.bottomRight(),
        QPoint(horizontalCenter, selectionPx.bottom()),
        selectionPx.bottomLeft(),
        QPoint(selectionPx.left(), verticalCenter),
    };
}

QPoint SelectionModel::clampPoint(const QPoint& pointPx) const noexcept
{
    if (boundsPx_.isEmpty()) {
        return pointPx;
    }
    return {
        std::clamp(pointPx.x(), boundsPx_.left(), boundsPx_.right()),
        std::clamp(pointPx.y(), boundsPx_.top(), boundsPx_.bottom()),
    };
}

QRect SelectionModel::clampMove(const QRect& rectPx) const noexcept
{
    if (!rectPx.isValid() || boundsPx_.isEmpty()) {
        return {};
    }

    QRect result(
        rectPx.topLeft(),
        QSize(
            std::min(rectPx.width(), boundsPx_.width()),
            std::min(rectPx.height(), boundsPx_.height())));
    if (result.left() < boundsPx_.left()) {
        result.moveLeft(boundsPx_.left());
    }
    if (result.right() > boundsPx_.right()) {
        result.moveRight(boundsPx_.right());
    }
    if (result.top() < boundsPx_.top()) {
        result.moveTop(boundsPx_.top());
    }
    if (result.bottom() > boundsPx_.bottom()) {
        result.moveBottom(boundsPx_.bottom());
    }
    return result;
}

QRect SelectionModel::resizedRect(const QPoint& currentPointPx) const noexcept
{
    int left = interactionStartPx_.left();
    int right = interactionStartPx_.right();
    int top = interactionStartPx_.top();
    int bottom = interactionStartPx_.bottom();

    if (changesLeft(activeHandle_)) {
        const int effectiveMinimumWidth = std::min(
            minimumSizePx_.width(), right - boundsPx_.left() + 1);
        const int maximumLeft = right - effectiveMinimumWidth + 1;
        left = std::clamp(currentPointPx.x(), boundsPx_.left(), maximumLeft);
    }
    if (changesRight(activeHandle_)) {
        const int effectiveMinimumWidth = std::min(
            minimumSizePx_.width(), boundsPx_.right() - left + 1);
        const int minimumRight = left + effectiveMinimumWidth - 1;
        right = std::clamp(currentPointPx.x(), minimumRight, boundsPx_.right());
    }
    if (changesTop(activeHandle_)) {
        const int effectiveMinimumHeight = std::min(
            minimumSizePx_.height(), bottom - boundsPx_.top() + 1);
        const int maximumTop = bottom - effectiveMinimumHeight + 1;
        top = std::clamp(currentPointPx.y(), boundsPx_.top(), maximumTop);
    }
    if (changesBottom(activeHandle_)) {
        const int effectiveMinimumHeight = std::min(
            minimumSizePx_.height(), boundsPx_.bottom() - top + 1);
        const int minimumBottom = top + effectiveMinimumHeight - 1;
        bottom = std::clamp(currentPointPx.y(), minimumBottom, boundsPx_.bottom());
    }

    return QRect(QPoint(left, top), QPoint(right, bottom));
}

}  // namespace snapask::capture
