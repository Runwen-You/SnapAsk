#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>

class QEvent;
class QScreen;
class QWidget;

namespace snapask::ui::windowing {

enum class WindowPlacement {
    Right,
    Left,
    Below,
    Above,
};

struct WindowLayoutResult {
    QRect geometry;
    WindowPlacement placement{WindowPlacement::Right};
    bool preferredPlacementFits{false};
};

enum class LinkedVisibilityPolicy {
    Independent,
    FollowLeader,
    KeepTogether,
};

enum class LinkedClosePolicy {
    Independent,
    CloseFollowerWithLeader,
    CloseTogether,
};

enum class LinkedTopmostPolicy {
    Independent,
    KeepTogether,
};

struct LinkedWindowOptions {
    bool followPosition{true};
    bool automaticLayout{true};
    int gap{12};
    LinkedVisibilityPolicy visibilityPolicy{LinkedVisibilityPolicy::FollowLeader};
    LinkedClosePolicy closePolicy{LinkedClosePolicy::CloseFollowerWithLeader};
    LinkedTopmostPolicy topmostPolicy{LinkedTopmostPolicy::KeepTogether};
};

// Keeps two independent top-level widgets logically connected without making
// either widget the native owner of the other. The controller deliberately
// uses public QWidget operations only, so unbinding restores full independence.
class LinkedWindowController final : public QObject {
public:
    explicit LinkedWindowController(QObject* parent = nullptr);
    LinkedWindowController(
        QWidget* leader,
        QWidget* follower,
        LinkedWindowOptions options = {},
        QObject* parent = nullptr);
    ~LinkedWindowController() override;

    LinkedWindowController(const LinkedWindowController&) = delete;
    LinkedWindowController& operator=(const LinkedWindowController&) = delete;

    [[nodiscard]] bool bind(
        QWidget* leader,
        QWidget* follower,
        LinkedWindowOptions options = {});
    void unbind();

    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] QWidget* leader() const noexcept;
    [[nodiscard]] QWidget* follower() const noexcept;

    [[nodiscard]] LinkedWindowOptions options() const noexcept;
    void setOptions(LinkedWindowOptions options);

    // Re-evaluates the screen work area and places the follower. This is also
    // called automatically for leader move/resize events when position
    // following is enabled.
    void reflow();
    [[nodiscard]] WindowPlacement currentPlacement() const noexcept;

    // Explicit group operations. They never affect a window after unbind().
    void setGroupVisible(bool visible);
    void setGroupAlwaysOnTop(bool enabled);
    void closeGroup();

    [[nodiscard]] static WindowLayoutResult chooseLayout(
        const QRect& leaderGeometry,
        const QSize& requestedFollowerSize,
        const QRect& availableGeometry,
        int gap = 12);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyFollowerGeometry(const QRect& frameGeometry);
    void setWindowAlwaysOnTop(QWidget* window, bool enabled);
    void handleVisibilityEvent(QWidget* source, bool visible);
    void handleCloseEvent(QWidget* source);
    void queueReflow();
    [[nodiscard]] QRect currentAvailableGeometry() const;
    [[nodiscard]] QRect fixedOffsetGeometry(const QRect& availableGeometry) const;

    QPointer<QWidget> leader_;
    QPointer<QWidget> follower_;
    LinkedWindowOptions options_;
    QSize preferredFollowerFrameSize_;
    QPoint followerOffset_;
    WindowPlacement placement_{WindowPlacement::Right};
    bool synchronizing_{false};
    bool reflowQueued_{false};
};

}  // namespace snapask::ui::windowing
