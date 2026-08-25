#include "platform/windows/WindowBackdrop.h"
#include "ui/windowing/LinkedWindowController.h"

#include <QApplication>
#include <QCloseEvent>
#include <QScreen>
#include <QTest>
#include <QWidget>

using snapask::platform::windows::BackdropMode;
using snapask::platform::windows::BackdropPreference;
using snapask::platform::windows::WindowBackdrop;
using snapask::platform::windows::WindowBackdropOptions;
using snapask::ui::windowing::LinkedClosePolicy;
using snapask::ui::windowing::LinkedTopmostPolicy;
using snapask::ui::windowing::LinkedVisibilityPolicy;
using snapask::ui::windowing::LinkedWindowController;
using snapask::ui::windowing::LinkedWindowOptions;
using snapask::ui::windowing::WindowPlacement;

namespace {

class TrackingWindow final : public QWidget {
public:
    TrackingWindow()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint)
    {
    }

    int moveEvents{0};
    int closeEvents{0};

protected:
    void moveEvent(QMoveEvent* event) override
    {
        ++moveEvents;
        QWidget::moveEvent(event);
    }

    void closeEvent(QCloseEvent* event) override
    {
        ++closeEvents;
        QWidget::closeEvent(event);
    }
};

void showWindow(QWidget& window)
{
    window.show();
    QTRY_VERIFY(window.isVisible());
    QCoreApplication::processEvents();
}

}  // namespace

class M5WindowingTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void choosesDirectionsInRequiredPriority();
    void clampsOversizedCardInsideNegativeWorkspace();
    void followerTracksLeaderWithoutRecursiveJitter();
    void visibilityAndTopmostPoliciesAreExplicitAndReversible();
    void closePolicyAndUnbindLeaveWindowsIndependent();
    void backdropFallbackPolicyProtectsReadability();
};

void M5WindowingTests::initTestCase()
{
    QApplication::setQuitOnLastWindowClosed(false);
}

void M5WindowingTests::choosesDirectionsInRequiredPriority()
{
    const QRect workArea(0, 0, 1000, 800);

    const auto right = LinkedWindowController::chooseLayout(
        QRect(100, 100, 200, 150), QSize(300, 250), workArea, 12);
    QCOMPARE(right.placement, WindowPlacement::Right);
    QVERIFY(right.preferredPlacementFits);
    QVERIFY(workArea.contains(right.geometry));

    const auto left = LinkedWindowController::chooseLayout(
        QRect(650, 100, 300, 150), QSize(300, 250), workArea, 12);
    QCOMPARE(left.placement, WindowPlacement::Left);
    QVERIFY(left.preferredPlacementFits);
    QVERIFY(workArea.contains(left.geometry));

    const auto below = LinkedWindowController::chooseLayout(
        QRect(350, 100, 300, 100), QSize(400, 250), workArea, 12);
    QCOMPARE(below.placement, WindowPlacement::Below);
    QVERIFY(below.preferredPlacementFits);
    QVERIFY(workArea.contains(below.geometry));

    const auto above = LinkedWindowController::chooseLayout(
        QRect(350, 500, 300, 200), QSize(400, 250), workArea, 12);
    QCOMPARE(above.placement, WindowPlacement::Above);
    QVERIFY(above.preferredPlacementFits);
    QVERIFY(workArea.contains(above.geometry));
}

void M5WindowingTests::clampsOversizedCardInsideNegativeWorkspace()
{
    const QRect negativeWorkArea(-1920, -200, 1920, 1080);
    const auto regular = LinkedWindowController::chooseLayout(
        QRect(-1800, 100, 300, 200), QSize(500, 400), negativeWorkArea, 16);
    QCOMPARE(regular.placement, WindowPlacement::Right);
    QVERIFY(negativeWorkArea.contains(regular.geometry));

    const auto oversized = LinkedWindowController::chooseLayout(
        QRect(-900, 150, 400, 300), QSize(5000, 3000), negativeWorkArea, 16);
    QCOMPARE(oversized.geometry, negativeWorkArea);
    QVERIFY(!oversized.preferredPlacementFits);
}

void M5WindowingTests::followerTracksLeaderWithoutRecursiveJitter()
{
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QRect available = screen->availableGeometry();
    QVERIFY(available.width() >= 500);
    QVERIFY(available.height() >= 350);

    TrackingWindow leader;
    TrackingWindow follower;
    leader.setGeometry(available.left() + 40, available.top() + 40, 160, 100);
    follower.resize(220, 180);
    showWindow(leader);
    showWindow(follower);

    LinkedWindowOptions options;
    options.gap = 14;
    options.visibilityPolicy = LinkedVisibilityPolicy::Independent;
    options.closePolicy = LinkedClosePolicy::Independent;
    LinkedWindowController controller(&leader, &follower, options);
    QCoreApplication::processEvents();

    const QPoint initialLeaderPosition = leader.frameGeometry().topLeft();
    const QPoint initialFollowerPosition = follower.frameGeometry().topLeft();
    const QPoint moveDelta(23, 17);
    follower.moveEvents = 0;
    leader.move(initialLeaderPosition + moveDelta);

    QTRY_COMPARE(
        follower.frameGeometry().topLeft(), initialFollowerPosition + moveDelta);
    QVERIFY2(follower.moveEvents <= 2, "position coupling recursed or visibly jittered");
    QVERIFY(available.contains(follower.frameGeometry()));
}

void M5WindowingTests::visibilityAndTopmostPoliciesAreExplicitAndReversible()
{
    TrackingWindow leader;
    TrackingWindow follower;
    leader.resize(160, 100);
    follower.resize(220, 180);

    LinkedWindowOptions options;
    options.followPosition = false;
    options.visibilityPolicy = LinkedVisibilityPolicy::KeepTogether;
    options.closePolicy = LinkedClosePolicy::Independent;
    options.topmostPolicy = LinkedTopmostPolicy::KeepTogether;
    LinkedWindowController controller(&leader, &follower, options);

    showWindow(leader);
    QTRY_VERIFY(follower.isVisible());
    leader.hide();
    QTRY_VERIFY(!follower.isVisible());
    follower.show();
    QTRY_VERIFY(leader.isVisible());

    controller.setGroupAlwaysOnTop(true);
    QVERIFY(leader.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QVERIFY(follower.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QVERIFY(leader.isVisible());
    QVERIFY(follower.isVisible());

    controller.unbind();
    leader.hide();
    QCoreApplication::processEvents();
    QVERIFY(follower.isVisible());

    leader.setWindowFlag(Qt::WindowStaysOnTopHint, false);
    QVERIFY(!leader.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QVERIFY(follower.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
}

void M5WindowingTests::closePolicyAndUnbindLeaveWindowsIndependent()
{
    TrackingWindow leader;
    TrackingWindow follower;
    leader.resize(160, 100);
    follower.resize(220, 180);
    showWindow(leader);
    showWindow(follower);

    LinkedWindowOptions options;
    options.followPosition = false;
    options.visibilityPolicy = LinkedVisibilityPolicy::Independent;
    options.closePolicy = LinkedClosePolicy::CloseTogether;
    LinkedWindowController controller(&leader, &follower, options);

    QVERIFY(follower.close());
    QTRY_COMPARE(follower.closeEvents, 1);
    QTRY_COMPARE(leader.closeEvents, 1);

    showWindow(leader);
    showWindow(follower);
    controller.unbind();
    QVERIFY(leader.close());
    QTRY_COMPARE(leader.closeEvents, 2);
    QCOMPARE(follower.closeEvents, 1);
    QVERIFY(follower.isVisible());
}

void M5WindowingTests::backdropFallbackPolicyProtectsReadability()
{
    QCOMPARE(
        WindowBackdrop::fallbackModeFor(true, false, true, true),
        BackdropMode::SolidFallback);
    QCOMPARE(
        WindowBackdrop::fallbackModeFor(false, true, true, true),
        BackdropMode::SolidFallback);
    QCOMPARE(
        WindowBackdrop::fallbackModeFor(false, false, false, true),
        BackdropMode::SolidFallback);
    QCOMPARE(
        WindowBackdrop::fallbackModeFor(false, false, true, false),
        BackdropMode::SolidFallback);
    QCOMPARE(
        WindowBackdrop::fallbackModeFor(false, false, true, true),
        BackdropMode::TranslucentFallback);

    QWidget window(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    const QPalette originalPalette = window.palette();
    WindowBackdropOptions options;
    options.preference = BackdropPreference::Solid;
    const auto result = WindowBackdrop::apply(&window, options);
    QCOMPARE(result.mode, BackdropMode::SolidFallback);
    QCOMPARE(window.palette().color(QPalette::Window).alpha(), 255);
    WindowBackdrop::reset(&window);
    QCOMPARE(window.palette(), originalPalette);
}

QTEST_MAIN(M5WindowingTests)
#include "M5WindowingTests.moc"
