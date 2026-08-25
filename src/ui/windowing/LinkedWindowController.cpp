#include "ui/windowing/LinkedWindowController.h"

#include <QEvent>
#include <QGuiApplication>
#include <QMargins>
#include <QScreen>
#include <QScopedValueRollback>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <limits>

namespace snapask::ui::windowing {

namespace {

[[nodiscard]] int clampedCoordinate(int value, int minimum, int maximum) noexcept
{
    if (maximum < minimum) {
        return minimum;
    }
    return std::clamp(value, minimum, maximum);
}

[[nodiscard]] QRect clampInside(QRect geometry, const QRect& available) noexcept
{
    if (!available.isValid()) {
        return geometry;
    }

    geometry.setSize({
        std::clamp(geometry.width(), 1, available.width()),
        std::clamp(geometry.height(), 1, available.height()),
    });
    geometry.moveLeft(clampedCoordinate(
        geometry.left(), available.left(), available.right() - geometry.width() + 1));
    geometry.moveTop(clampedCoordinate(
        geometry.top(), available.top(), available.bottom() - geometry.height() + 1));
    return geometry;
}

[[nodiscard]] bool isInside(const QRect& inner, const QRect& outer) noexcept
{
    return outer.isValid() && outer.contains(inner.topLeft())
        && outer.contains(inner.bottomRight());
}

[[nodiscard]] qint64 visibleArea(const QRect& geometry, const QRect& available) noexcept
{
    const QRect intersection = geometry.intersected(available);
    return static_cast<qint64>(std::max(0, intersection.width()))
        * static_cast<qint64>(std::max(0, intersection.height()));
}

[[nodiscard]] QRect candidateGeometry(
    WindowPlacement placement,
    const QRect& leader,
    const QSize& followerSize,
    const QRect& available,
    int gap) noexcept
{
    const int maximumX = available.right() - followerSize.width() + 1;
    const int maximumY = available.bottom() - followerSize.height() + 1;
    const int alignedX = clampedCoordinate(leader.left(), available.left(), maximumX);
    const int alignedY = clampedCoordinate(leader.top(), available.top(), maximumY);

    switch (placement) {
    case WindowPlacement::Right:
        return {{leader.right() + 1 + gap, alignedY}, followerSize};
    case WindowPlacement::Left:
        return {{leader.left() - gap - followerSize.width(), alignedY}, followerSize};
    case WindowPlacement::Below:
        return {{alignedX, leader.bottom() + 1 + gap}, followerSize};
    case WindowPlacement::Above:
        return {{alignedX, leader.top() - gap - followerSize.height()}, followerSize};
    }
    return {{leader.right() + 1 + gap, alignedY}, followerSize};
}

[[nodiscard]] QMargins frameMarginsFor(const QWidget* window) noexcept
{
    const QRect client = window->geometry();
    const QRect frame = window->frameGeometry();
    return {
        client.left() - frame.left(),
        client.top() - frame.top(),
        frame.right() - client.right(),
        frame.bottom() - client.bottom(),
    };
}

}  // namespace

LinkedWindowController::LinkedWindowController(QObject* parent)
    : QObject(parent)
{
}

LinkedWindowController::LinkedWindowController(
    QWidget* leader,
    QWidget* follower,
    LinkedWindowOptions options,
    QObject* parent)
    : QObject(parent)
{
    (void)bind(leader, follower, options);
}

LinkedWindowController::~LinkedWindowController()
{
    unbind();
}

bool LinkedWindowController::bind(
    QWidget* leader,
    QWidget* follower,
    LinkedWindowOptions options)
{
    if (leader == nullptr || follower == nullptr || leader == follower) {
        return false;
    }

    unbind();
    leader_ = leader;
    follower_ = follower;
    options_ = options;
    options_.gap = std::max(0, options_.gap);
    preferredFollowerFrameSize_ = follower_->frameGeometry().size();
    if (!preferredFollowerFrameSize_.isValid()) {
        preferredFollowerFrameSize_ = follower_->sizeHint().expandedTo(QSize(1, 1));
    }
    followerOffset_ = follower_->frameGeometry().topLeft()
        - leader_->frameGeometry().topLeft();

    leader_->installEventFilter(this);
    follower_->installEventFilter(this);

    if (options_.topmostPolicy == LinkedTopmostPolicy::KeepTogether) {
        const bool leaderIsTopmost = leader_->windowFlags().testFlag(
            Qt::WindowStaysOnTopHint);
        setWindowAlwaysOnTop(follower_, leaderIsTopmost);
    }

    if (options_.visibilityPolicy != LinkedVisibilityPolicy::Independent) {
        QScopedValueRollback guard(synchronizing_, true);
        follower_->setVisible(leader_->isVisible());
    }

    if (options_.followPosition) {
        reflow();
    }
    return true;
}

void LinkedWindowController::unbind()
{
    QScopedValueRollback guard(synchronizing_, true);
    if (leader_ != nullptr) {
        leader_->removeEventFilter(this);
    }
    if (follower_ != nullptr) {
        follower_->removeEventFilter(this);
    }
    leader_.clear();
    follower_.clear();
    reflowQueued_ = false;
    preferredFollowerFrameSize_ = {};
    followerOffset_ = {};
}

bool LinkedWindowController::isBound() const noexcept
{
    return leader_ != nullptr && follower_ != nullptr;
}

QWidget* LinkedWindowController::leader() const noexcept
{
    return leader_;
}

QWidget* LinkedWindowController::follower() const noexcept
{
    return follower_;
}

LinkedWindowOptions LinkedWindowController::options() const noexcept
{
    return options_;
}

void LinkedWindowController::setOptions(LinkedWindowOptions options)
{
    options.gap = std::max(0, options.gap);
    options_ = options;
    if (!isBound()) {
        return;
    }
    followerOffset_ = follower_->frameGeometry().topLeft()
        - leader_->frameGeometry().topLeft();
    if (options_.topmostPolicy == LinkedTopmostPolicy::KeepTogether) {
        QScopedValueRollback guard(synchronizing_, true);
        setWindowAlwaysOnTop(
            follower_, leader_->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    }
    if (options_.visibilityPolicy != LinkedVisibilityPolicy::Independent) {
        QScopedValueRollback guard(synchronizing_, true);
        follower_->setVisible(leader_->isVisible());
    }
    if (options_.followPosition) {
        reflow();
    }
}

void LinkedWindowController::reflow()
{
    if (!isBound() || !options_.followPosition || synchronizing_) {
        return;
    }

    const QRect available = currentAvailableGeometry();
    if (!available.isValid()) {
        return;
    }

    QRect geometry;
    if (options_.automaticLayout) {
        const WindowLayoutResult result = chooseLayout(
            leader_->frameGeometry(), preferredFollowerFrameSize_, available, options_.gap);
        geometry = result.geometry;
        placement_ = result.placement;
    } else {
        geometry = fixedOffsetGeometry(available);
    }
    applyFollowerGeometry(geometry);
    followerOffset_ = follower_->frameGeometry().topLeft()
        - leader_->frameGeometry().topLeft();
}

WindowPlacement LinkedWindowController::currentPlacement() const noexcept
{
    return placement_;
}

void LinkedWindowController::setGroupVisible(bool visible)
{
    if (!isBound()) {
        return;
    }
    QScopedValueRollback guard(synchronizing_, true);
    leader_->setVisible(visible);
    follower_->setVisible(visible);
}

void LinkedWindowController::setGroupAlwaysOnTop(bool enabled)
{
    if (!isBound()) {
        return;
    }
    QScopedValueRollback guard(synchronizing_, true);
    setWindowAlwaysOnTop(leader_, enabled);
    setWindowAlwaysOnTop(follower_, enabled);
}

void LinkedWindowController::closeGroup()
{
    if (!isBound()) {
        return;
    }
    QScopedValueRollback guard(synchronizing_, true);
    follower_->close();
    leader_->close();
}

WindowLayoutResult LinkedWindowController::chooseLayout(
    const QRect& leaderGeometry,
    const QSize& requestedFollowerSize,
    const QRect& availableGeometry,
    int gap)
{
    WindowLayoutResult result;
    const QSize safeRequested(
        std::max(1, requestedFollowerSize.width()),
        std::max(1, requestedFollowerSize.height()));
    if (!availableGeometry.isValid()) {
        result.geometry = {
            {leaderGeometry.right() + 1 + std::max(0, gap), leaderGeometry.top()},
            safeRequested,
        };
        return result;
    }

    const QSize fittedSize(
        std::min(safeRequested.width(), availableGeometry.width()),
        std::min(safeRequested.height(), availableGeometry.height()));
    const std::array order{
        WindowPlacement::Right,
        WindowPlacement::Left,
        WindowPlacement::Below,
        WindowPlacement::Above,
    };
    const int safeGap = std::max(0, gap);

    qint64 bestVisibleArea = std::numeric_limits<qint64>::min();
    QRect bestCandidate;
    WindowPlacement bestPlacement = WindowPlacement::Right;
    for (const WindowPlacement placement : order) {
        const QRect candidate = candidateGeometry(
            placement, leaderGeometry, fittedSize, availableGeometry, safeGap);
        if (isInside(candidate, availableGeometry)) {
            return {candidate, placement, true};
        }

        const qint64 area = visibleArea(candidate, availableGeometry);
        if (area > bestVisibleArea) {
            bestVisibleArea = area;
            bestCandidate = candidate;
            bestPlacement = placement;
        }
    }

    result.geometry = clampInside(bestCandidate, availableGeometry);
    result.placement = bestPlacement;
    result.preferredPlacementFits = false;
    return result;
}

bool LinkedWindowController::eventFilter(QObject* watched, QEvent* event)
{
    if (!isBound() || synchronizing_) {
        return QObject::eventFilter(watched, event);
    }

    QWidget* source = qobject_cast<QWidget*>(watched);
    if (source == nullptr) {
        return QObject::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::Move:
        if (source == leader_ && options_.followPosition) {
            queueReflow();
        } else if (source == follower_ && !options_.automaticLayout) {
            followerOffset_ = follower_->frameGeometry().topLeft()
                - leader_->frameGeometry().topLeft();
        }
        break;
    case QEvent::Resize:
        if (source == leader_ && options_.followPosition) {
            queueReflow();
        } else if (source == follower_) {
            preferredFollowerFrameSize_ = follower_->frameGeometry().size();
            if (options_.followPosition && options_.automaticLayout) {
                queueReflow();
            }
        }
        break;
    case QEvent::Show:
        handleVisibilityEvent(source, true);
        if (options_.topmostPolicy == LinkedTopmostPolicy::KeepTogether) {
            QWidget* target = source == leader_ ? follower_.data() : leader_.data();
            if (target != nullptr) {
                QScopedValueRollback guard(synchronizing_, true);
                setWindowAlwaysOnTop(
                    target,
                    source->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            }
        }
        break;
    case QEvent::Hide:
        handleVisibilityEvent(source, false);
        break;
    case QEvent::Close:
        handleCloseEvent(source);
        break;
    case QEvent::Destroy:
        unbind();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void LinkedWindowController::queueReflow()
{
    if (reflowQueued_ || !isBound() || follower_ == nullptr
        || !follower_->isVisible()) {
        return;
    }
    reflowQueued_ = true;
    // Native move events can arrive far faster than the compositor refreshes.
    // Coalescing them to a short frame interval avoids repeatedly querying the
    // work area and repainting two acrylic windows for one visible frame.
    QTimer::singleShot(8, this, [this] {
        reflowQueued_ = false;
        reflow();
    });
}

void LinkedWindowController::applyFollowerGeometry(const QRect& frameGeometry)
{
    if (follower_ == nullptr || !frameGeometry.isValid()) {
        return;
    }
    QScopedValueRollback guard(synchronizing_, true);
    const QMargins margins = frameMarginsFor(follower_);
    QRect clientGeometry = frameGeometry.marginsRemoved(margins);
    clientGeometry.setWidth(std::max(1, clientGeometry.width()));
    clientGeometry.setHeight(std::max(1, clientGeometry.height()));
    follower_->setGeometry(clientGeometry);
}

void LinkedWindowController::setWindowAlwaysOnTop(QWidget* window, bool enabled)
{
    if (window == nullptr
        || window->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == enabled) {
        return;
    }
    const bool wasVisible = window->isVisible();
    const QRect previousFrame = window->frameGeometry();
    window->setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    if (wasVisible) {
        window->show();
        const QMargins margins = frameMarginsFor(window);
        window->setGeometry(previousFrame.marginsRemoved(margins));
        window->raise();
    }
}

void LinkedWindowController::handleVisibilityEvent(QWidget* source, bool visible)
{
    if (!isBound() || options_.visibilityPolicy == LinkedVisibilityPolicy::Independent) {
        return;
    }
    if (source == follower_
        && options_.visibilityPolicy != LinkedVisibilityPolicy::KeepTogether) {
        return;
    }

    QWidget* target = source == leader_ ? follower_.data() : leader_.data();
    if (target != nullptr && target->isVisible() != visible) {
        QScopedValueRollback guard(synchronizing_, true);
        target->setVisible(visible);
    }
}

void LinkedWindowController::handleCloseEvent(QWidget* source)
{
    if (!isBound() || options_.closePolicy == LinkedClosePolicy::Independent) {
        return;
    }
    if (source == follower_
        && options_.closePolicy != LinkedClosePolicy::CloseTogether) {
        return;
    }

    QPointer<QWidget> sourceGuard(source);
    QPointer<QWidget> targetGuard(
        source == leader_ ? follower_.data() : leader_.data());
    // A close event can still be rejected by the source window (for example,
    // an editor with unsaved work). Defer the paired close until the event has
    // completed, and restore a follower whose leader rejected CloseTogether.
    QTimer::singleShot(0, this, [this, sourceGuard, targetGuard] {
        if (!isBound() || sourceGuard == nullptr || targetGuard == nullptr
            || sourceGuard->isVisible()) {
            return;
        }
        const bool restoreSourceOnRejection =
            sourceGuard == follower_
            && options_.closePolicy == LinkedClosePolicy::CloseTogether;
        {
            QScopedValueRollback guard(synchronizing_, true);
            targetGuard->close();
        }
        if (restoreSourceOnRejection) {
            QTimer::singleShot(0, this, [this, sourceGuard, targetGuard] {
                if (isBound() && sourceGuard != nullptr && targetGuard != nullptr
                    && targetGuard->isVisible() && !sourceGuard->isVisible()) {
                    QScopedValueRollback guard(synchronizing_, true);
                    sourceGuard->show();
                }
            });
        }
    });
}

QRect LinkedWindowController::currentAvailableGeometry() const
{
    if (leader_ == nullptr) {
        return {};
    }
    const QPoint anchor = leader_->frameGeometry().center();
    QScreen* screen = QGuiApplication::screenAt(anchor);
    if (screen == nullptr) {
        screen = leader_->screen();
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen != nullptr ? screen->availableGeometry() : QRect{};
}

QRect LinkedWindowController::fixedOffsetGeometry(const QRect& availableGeometry) const
{
    if (!isBound()) {
        return {};
    }
    const QRect requested(
        leader_->frameGeometry().topLeft() + followerOffset_,
        preferredFollowerFrameSize_);
    return clampInside(requested, availableGeometry);
}

}  // namespace snapask::ui::windowing
