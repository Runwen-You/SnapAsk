#include "domain/capture/CaptureFrame.h"
#include "domain/capture/CaptureGeometry.h"
#include "platform/windows/MonitorCoordinateMapper.h"
#include "platform/windows/ScreenCapture.h"

#include <QColor>
#include <QImage>
#include <QtTest>

using snapask::capture::CaptureFrame;
using snapask::capture::MonitorGeometry;
using snapask::capture::MonitorLayout;

namespace {

[[nodiscard]] MonitorGeometry monitor(
    const QString& id,
    const QRect& geometry,
    quint32 dpi,
    bool primary = false)
{
    return MonitorGeometry{id, geometry, geometry, dpi, dpi, primary};
}

}  // namespace

class M1CaptureGeometryTest final : public QObject {
    Q_OBJECT

private slots:
    void nativeEdgesUseHalfOpenDimensions();
    void negativeDesktopCoordinatesRoundTrip();
    void portraitAndMixedDpiMonitorLookup();
    void rectanglesClampWithoutChangingSmallSelectionSize();
    void cropProducesMatchingPixels();
    void cropOutsideDesktopFails();
    void gdiCaptureReturnsPhysicalVirtualDesktopFrame();
};

void M1CaptureGeometryTest::nativeEdgesUseHalfOpenDimensions()
{
    using snapask::platform::windows::MonitorCoordinateMapper;
    QCOMPARE(MonitorCoordinateMapper::fromNativeEdges(-1920, -200, 0, 880),
             QRect(-1920, -200, 1920, 1080));
    QVERIFY(MonitorCoordinateMapper::fromNativeEdges(10, 10, 10, 20).isNull());
    QVERIFY(MonitorCoordinateMapper::fromNativeEdges(20, 10, 10, 20).isNull());
}

void M1CaptureGeometryTest::negativeDesktopCoordinatesRoundTrip()
{
    const QRect desktop(-1920, -200, 4480, 1640);
    MonitorLayout layout(
        desktop,
        {
            monitor(QStringLiteral("left"), QRect(-1920, 0, 1920, 1080), 120),
            monitor(QStringLiteral("primary"), QRect(0, -200, 2560, 1440), 144, true),
        });

    QVERIFY(layout.isValid());
    QCOMPARE(layout.desktopToImage(QPoint(-1920, -200)), QPoint(0, 0));
    QCOMPARE(layout.desktopToImage(QPoint(0, 0)), QPoint(1920, 200));
    QCOMPARE(layout.imageToDesktop(QPoint(1920, 200)), QPoint(0, 0));

    const QRect physicalSelection(-100, -50, 500, 300);
    const QRect imageSelection(1820, 150, 500, 300);
    QCOMPARE(layout.desktopToImage(physicalSelection), imageSelection);
    QCOMPARE(layout.imageToDesktop(imageSelection), physicalSelection);
}

void M1CaptureGeometryTest::portraitAndMixedDpiMonitorLookup()
{
    const MonitorGeometry portrait = monitor(
        QStringLiteral("portrait-150"),
        QRect(-1080, -480, 1080, 1920),
        144);
    const MonitorGeometry primary = monitor(
        QStringLiteral("landscape-100"),
        QRect(0, 0, 2560, 1440),
        96,
        true);
    MonitorLayout layout(QRect(-1080, -480, 3640, 1920), {portrait, primary});

    const MonitorGeometry* atPortrait = layout.monitorAt(QPoint(-500, 1300));
    QVERIFY(atPortrait != nullptr);
    QCOMPARE(atPortrait->deviceId, QStringLiteral("portrait-150"));
    QCOMPARE(atPortrait->scaleX(), 1.5);

    const MonitorGeometry* atPrimary = layout.monitorAt(QPoint(2000, 500));
    QVERIFY(atPrimary != nullptr);
    QVERIFY(atPrimary->primary);

    // 300 px overlap on portrait and 700 px overlap on landscape.
    const MonitorGeometry* dominant = layout.monitorForRect(QRect(-300, 100, 1000, 200));
    QVERIFY(dominant != nullptr);
    QCOMPARE(dominant->deviceId, QStringLiteral("landscape-100"));
    QVERIFY(layout.monitorAt(QPoint(3000, 3000)) == nullptr);
}

void M1CaptureGeometryTest::rectanglesClampWithoutChangingSmallSelectionSize()
{
    MonitorLayout layout(
        QRect(-100, -50, 300, 200),
        {monitor(QStringLiteral("only"), QRect(-100, -50, 300, 200), 96, true)});

    QCOMPARE(layout.clampToDesktop(QRect(-150, -90, 40, 30)), QRect(-100, -50, 40, 30));
    QCOMPARE(layout.clampToDesktop(QRect(190, 140, 40, 30)), QRect(160, 120, 40, 30));
    QCOMPARE(layout.intersectWithDesktop(QRect(-120, -60, 40, 30)), QRect(-100, -50, 20, 20));
    QCOMPARE(layout.clampToDesktop(QRect(-500, -500, 1000, 1000)),
             QRect(-100, -50, 300, 200));
}

void M1CaptureGeometryTest::cropProducesMatchingPixels()
{
    const QRect desktop(-3, -2, 6, 4);
    MonitorLayout layout(
        desktop,
        {monitor(QStringLiteral("only"), desktop, 96, true)});

    QImage image(desktop.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(20 * x, 40 * y, x + y, 255));
        }
    }

    const CaptureFrame frame(image, desktop, layout);
    QVERIFY(frame.isValid());

    QString error;
    const auto crop = frame.cropPixels(QRect(-2, -1, 3, 2), &error);
    QVERIFY2(crop.has_value(), qPrintable(error));
    QVERIFY(crop->isValid());
    QCOMPARE(crop->desktopRectPx, QRect(-2, -1, 3, 2));
    QCOMPARE(crop->image.size(), QSize(3, 2));
    QCOMPARE(crop->image.pixelColor(0, 0), image.pixelColor(1, 1));
    QCOMPARE(crop->image.pixelColor(2, 1), image.pixelColor(3, 2));
}

void M1CaptureGeometryTest::cropOutsideDesktopFails()
{
    const QRect desktop(-100, 50, 10, 10);
    const MonitorLayout layout(
        desktop,
        {monitor(QStringLiteral("only"), desktop, 96, true)});
    QImage image(desktop.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    const CaptureFrame frame(image, desktop, layout);

    QString error;
    const auto crop = frame.cropPixels(QRect(500, 500, 20, 20), &error);
    QVERIFY(!crop.has_value());
    QVERIFY(!error.isEmpty());
}

void M1CaptureGeometryTest::gdiCaptureReturnsPhysicalVirtualDesktopFrame()
{
    snapask::platform::windows::GdiScreenCapture backend;
    QString error;
    const auto frame = backend.captureVirtualDesktop(&error);
    QVERIFY2(frame.has_value(), qPrintable(error));
    QVERIFY(frame->isValid());
    QCOMPARE(frame->desktopRectPx(),
             snapask::platform::windows::MonitorCoordinateMapper::virtualDesktopRectPx());
    QCOMPARE(frame->image().size(), frame->desktopRectPx().size());
    QCOMPARE(frame->image().format(), QImage::Format_ARGB32_Premultiplied);

    const QPoint center = frame->desktopRectPx().center();
    const auto onePixel = frame->cropPixels(QRect(center, QSize(1, 1)), &error);
    QVERIFY2(onePixel.has_value(), qPrintable(error));
    QCOMPARE(onePixel->image.size(), QSize(1, 1));
    QCOMPARE(onePixel->image.pixelColor(0, 0).alpha(), 255);
}

QTEST_GUILESS_MAIN(M1CaptureGeometryTest)

#include "m1_capture_geometry_test.moc"
