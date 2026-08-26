#include "domain/capture/CaptureFrame.h"
#include "ui/capture/CaptureOverlay.h"
#include "ui/glass/GlassToolbar.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include <utility>

using snapask::capture::CaptureFrame;
using snapask::capture::MonitorGeometry;
using snapask::capture::MonitorLayout;
using snapask::ui::capture::CaptureHandoffAction;
using snapask::ui::capture::CaptureOverlay;

namespace {

CaptureFrame testFrame()
{
    const QRect desktop(0, 0, 320, 220);
    QImage image(desktop.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(34, 80, 128));
    MonitorGeometry monitor{
        QStringLiteral("test-monitor"), desktop, desktop, 96, 96, true};
    return CaptureFrame(
        std::move(image), desktop, MonitorLayout(desktop, {monitor}));
}

}  // namespace

class M6CaptureToolbarTests final : public QObject {
    Q_OBJECT

private slots:
    void selectionShowsCompactActionBarAndPreservesRequestedTool();
};

void M6CaptureToolbarTests::selectionShowsCompactActionBarAndPreservesRequestedTool()
{
    CaptureOverlay overlay;
    QString error;
    QVERIFY2(overlay.beginCapture(testFrame(), &error), qPrintable(error));
    overlay.setSelectionPx(QRect(24, 18, 220, 150));
    QCoreApplication::processEvents();

    auto* actionBar = overlay.findChild<snapask::ui::glass::GlassToolbar*>(
        QStringLiteral("CaptureActionBar"));
    QVERIFY(actionBar != nullptr);
    QVERIFY(actionBar->isVisible());
    QVERIFY(actionBar->hasBackdropImage());
    const auto buttons = actionBar->findChildren<QToolButton*>();
    QVERIFY(buttons.size() >= 10);

    QToolButton* rectangle = nullptr;
    for (QToolButton* button : buttons) {
        if (button->toolTip() == QStringLiteral("矩形")) {
            rectangle = button;
            break;
        }
    }
    QVERIFY(rectangle != nullptr);
    QSignalSpy confirmed(&overlay, &CaptureOverlay::captureConfirmed);
    QTest::mouseClick(rectangle, Qt::LeftButton);
    QCOMPARE(confirmed.size(), 1);
    QCOMPARE(overlay.takeHandoffAction(), CaptureHandoffAction::Rectangle);
    QVERIFY(!overlay.isCaptureActive());
    QVERIFY(!actionBar->hasBackdropImage());
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    M6CaptureToolbarTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "M6CaptureToolbarTests.moc"
