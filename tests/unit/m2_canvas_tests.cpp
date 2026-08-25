#include "domain/annotation/Annotation.h"
#include "domain/capture/ScreenshotSession.h"
#include "services/SnapshotRenderer.h"
#include "ui/canvas/CanvasWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QTest>

#include <cmath>

namespace snapask::ui::canvas {
namespace {

QImage sourceImage(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(245, 246, 248));
    return image;
}

void sendMouse(CanvasWidget* widget,
               QEvent::Type type,
               const QPointF& localPosition,
               Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    const QPointF globalPosition = widget->mapToGlobal(localPosition.toPoint());
    QMouseEvent event(type, localPosition, globalPosition, button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    QCoreApplication::processEvents();
}

void click(CanvasWidget* widget, const QPointF& position) {
    sendMouse(widget, QEvent::MouseButtonPress, position, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, position, Qt::LeftButton,
              Qt::NoButton);
}

void drag(CanvasWidget* widget, const QVector<QPointF>& positions) {
    QVERIFY(positions.size() >= 2);
    sendMouse(widget, QEvent::MouseButtonPress, positions.front(),
              Qt::LeftButton, Qt::LeftButton);
    for (qsizetype index = 1; index + 1 < positions.size(); ++index) {
        sendMouse(widget, QEvent::MouseMove, positions[index], Qt::NoButton,
                  Qt::LeftButton);
    }
    sendMouse(widget, QEvent::MouseMove, positions.back(), Qt::NoButton,
              Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, positions.back(),
              Qt::LeftButton, Qt::NoButton);
}

void verifyPointNear(const QPointF& actual,
                     const QPointF& expected,
                     qreal tolerance = 0.05) {
    QVERIFY2(QLineF(actual, expected).length() <= tolerance,
             qPrintable(QStringLiteral("actual=(%1,%2), expected=(%3,%4)")
                            .arg(actual.x())
                            .arg(actual.y())
                            .arg(expected.x())
                            .arg(expected.y())));
}

class M2CanvasTests final : public QObject {
    Q_OBJECT

private slots:
    void widgetCoordinatesMapToSourcePhysicalPixels();
    void rectangleCreationPushesOneCommand();
    void selectedAnnotationMovesResizesAndDeletesUndoably();
    void keyboardNudgesUsePhysicalPixelsAndMergeContinuously();
    void continuousMosaicStrokePushesOneCommand();
    void selectionChromeNeverEntersRenderedSnapshot();
};

void M2CanvasTests::widgetCoordinatesMapToSourcePhysicalPixels() {
    ScreenshotSession session(sourceImage(QSize(400, 240)));
    QVERIFY(session.setCropRect(QRect(50, 40, 200, 100)));
    CanvasWidget canvas(&session);
    canvas.resize(500, 400);
    canvas.show();
    QCoreApplication::processEvents();

    QCOMPARE(canvas.imageDisplayRect(), QRectF(0.0, 75.0, 500.0, 250.0));
    const QPointF sourcePoint(130.0, 70.0);
    const QPointF widgetPoint = canvas.mapImageToWidget(sourcePoint);
    verifyPointNear(widgetPoint, QPointF(200.0, 150.0));

    const auto mappedBack = canvas.mapWidgetToImage(widgetPoint);
    QVERIFY(mappedBack.has_value());
    verifyPointNear(*mappedBack, sourcePoint);
    QVERIFY(!canvas.mapWidgetToImage(QPointF(250.0, 30.0)).has_value());
}

void M2CanvasTests::rectangleCreationPushesOneCommand() {
    ScreenshotSession session(sourceImage(QSize(320, 180)));
    CanvasWidget canvas(&session);
    canvas.resize(640, 360);
    canvas.setTool(CanvasTool::Rectangle);
    canvas.show();
    QCoreApplication::processEvents();

    const QPointF start = canvas.mapImageToWidget(QPointF(24.0, 30.0));
    const QPointF end = canvas.mapImageToWidget(QPointF(140.0, 96.0));
    drag(&canvas, {start, (start + end) * 0.5, end});

    QCOMPARE(session.annotations().size(), 1);
    QCOMPARE(session.undoStack().count(), 1);
    const Annotation& annotation = session.annotations().annotations().front();
    QCOMPARE(annotation.type, AnnotationType::Rectangle);
    const auto& geometry = std::get<RectangleGeometry>(annotation.geometry);
    verifyPointNear(geometry.rect.topLeft(), QPointF(24.0, 30.0));
    verifyPointNear(geometry.rect.bottomRight(), QPointF(140.0, 96.0));

    session.undoStack().undo();
    QVERIFY(session.annotations().isEmpty());
    session.undoStack().redo();
    QCOMPARE(session.annotations().size(), 1);
}

void M2CanvasTests::selectedAnnotationMovesResizesAndDeletesUndoably() {
    ScreenshotSession session(sourceImage(QSize(300, 200)));
    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(50.0, 45.0, 90.0, 60.0));
    QVERIFY(session.annotations().addAnnotation(rectangle));

    CanvasWidget canvas(&session);
    canvas.resize(600, 400);
    canvas.setTool(CanvasTool::Select);
    canvas.show();
    canvas.refreshFromSession();
    QCoreApplication::processEvents();

    const QPointF originalCenter = canvas.mapImageToWidget(QPointF(95.0, 75.0));
    click(&canvas, originalCenter);
    QCOMPARE(session.annotations().selectedAnnotationIds(), QSet<QUuid>{rectangle.id});
    QCOMPARE(session.undoStack().count(), 0);

    const QPointF movedCenter = canvas.mapImageToWidget(QPointF(125.0, 95.0));
    drag(&canvas, {originalCenter,
                   canvas.mapImageToWidget(QPointF(105.0, 82.0)),
                   canvas.mapImageToWidget(QPointF(115.0, 90.0)),
                   movedCenter});
    QCOMPARE(session.undoStack().count(), 1);  // One drag, one Transform command.

    const Annotation* moved = session.annotations().annotation(rectangle.id);
    QVERIFY(moved != nullptr);
    const QRectF movedRect = std::get<RectangleGeometry>(moved->geometry).rect;
    verifyPointNear(movedRect.topLeft(), QPointF(80.0, 65.0));
    verifyPointNear(movedRect.bottomRight(), QPointF(170.0, 125.0));

    const QPointF oldTopLeftHandle = canvas.mapImageToWidget(movedRect.topLeft());
    const QPointF newTopLeftHandle = canvas.mapImageToWidget(QPointF(65.0, 52.0));
    drag(&canvas, {oldTopLeftHandle, newTopLeftHandle});
    QCOMPARE(session.undoStack().count(), 2);
    const Annotation* resized = session.annotations().annotation(rectangle.id);
    QVERIFY(resized != nullptr);
    const QRectF resizedRect =
        std::get<RectangleGeometry>(resized->geometry).rect;
    verifyPointNear(resizedRect.topLeft(), QPointF(65.0, 52.0));
    verifyPointNear(resizedRect.bottomRight(), QPointF(170.0, 125.0));

    QTest::keyClick(&canvas, Qt::Key_Delete);
    QCOMPARE(session.undoStack().count(), 3);
    QVERIFY(session.annotations().isEmpty());
    session.undoStack().undo();
    QVERIFY(session.annotations().annotation(rectangle.id) != nullptr);
}

void M2CanvasTests::keyboardNudgesUsePhysicalPixelsAndMergeContinuously() {
    ScreenshotSession session(sourceImage(QSize(300, 200)));
    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(40.0, 35.0, 80.0, 50.0));
    QVERIFY(session.annotations().addAnnotation(rectangle));

    CanvasWidget canvas(&session);
    canvas.resize(600, 400);
    canvas.setTool(CanvasTool::Select);
    canvas.show();
    canvas.refreshFromSession();
    QCoreApplication::processEvents();

    click(&canvas, canvas.mapImageToWidget(QPointF(80.0, 60.0)));
    QCOMPARE(session.annotations().selectedAnnotationIds(),
             QSet<QUuid>{rectangle.id});

    QTest::keyClick(&canvas, Qt::Key_Right);
    QTest::keyClick(&canvas, Qt::Key_Right);
    QTest::keyClick(&canvas, Qt::Key_Right);
    QTest::keyClick(&canvas, Qt::Key_Down, Qt::ShiftModifier);

    // Every arrow key uses source-image physical pixels. One uninterrupted
    // keyboard sequence is represented by a single undo command.
    QCOMPARE(session.undoStack().count(), 1);
    const Annotation* nudged = session.annotations().annotation(rectangle.id);
    QVERIFY(nudged != nullptr);
    QCOMPARE(std::get<RectangleGeometry>(nudged->geometry).rect,
             QRectF(43.0, 45.0, 80.0, 50.0));

    session.undoStack().undo();
    QCOMPARE(session.annotations().annotation(rectangle.id)->geometry,
             rectangle.geometry);
    session.undoStack().redo();

    // A mouse interaction ends the keyboard merge sequence, so the next
    // nudge remains a separate and independently undoable edit.
    click(&canvas, canvas.mapImageToWidget(QPointF(83.0, 70.0)));
    QTest::keyClick(&canvas, Qt::Key_Left);
    QCOMPARE(session.undoStack().count(), 2);
    QCOMPARE(std::get<RectangleGeometry>(
                 session.annotations().annotation(rectangle.id)->geometry).rect,
             QRectF(42.0, 45.0, 80.0, 50.0));
    session.undoStack().undo();
    QCOMPARE(std::get<RectangleGeometry>(
                 session.annotations().annotation(rectangle.id)->geometry).rect,
             QRectF(43.0, 45.0, 80.0, 50.0));
}

void M2CanvasTests::continuousMosaicStrokePushesOneCommand() {
    ScreenshotSession session(sourceImage(QSize(240, 160)));
    CanvasWidget canvas(&session);
    canvas.resize(480, 320);
    canvas.setTool(CanvasTool::Mosaic);
    canvas.setMosaicBrushWidth(24.0);
    canvas.show();
    QCoreApplication::processEvents();

    QVector<QPointF> positions;
    for (int index = 0; index < 12; ++index) {
        positions.push_back(canvas.mapImageToWidget(
            QPointF(20.0 + index * 10.0, 25.0 + index * 4.0)));
    }
    drag(&canvas, positions);

    QCOMPARE(session.undoStack().count(), 1);
    QCOMPARE(session.annotations().size(), 1);
    const Annotation& annotation = session.annotations().annotations().front();
    QCOMPARE(annotation.type, AnnotationType::Mosaic);
    const auto& geometry = std::get<MosaicGeometry>(annotation.geometry);
    QVERIFY(geometry.points.size() >= 10);
    QCOMPARE(geometry.brushWidth, 24.0);

    session.undoStack().undo();
    QVERIFY(session.annotations().isEmpty());
}

void M2CanvasTests::selectionChromeNeverEntersRenderedSnapshot() {
    ScreenshotSession session(sourceImage(QSize(220, 140)));
    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(30.0, 25.0, 100.0, 70.0));
    QVERIFY(session.annotations().addAnnotation(rectangle));
    const RenderedSnapshot beforeSelection =
        SnapshotRenderer::renderCurrent(session);
    QVERIFY(beforeSelection.isValid());

    CanvasWidget canvas(&session);
    canvas.resize(440, 280);
    canvas.setTool(CanvasTool::Select);
    canvas.show();
    canvas.refreshFromSession();
    QCoreApplication::processEvents();
    click(&canvas, canvas.mapImageToWidget(QPointF(70.0, 60.0)));
    QCOMPARE(session.annotations().selectedAnnotationIds(), QSet<QUuid>{rectangle.id});

    // Canvas paintEvent now includes a dashed selection border and eight blue
    // handles, while the canonical renderer consumes only document content.
    const RenderedSnapshot afterSelection =
        SnapshotRenderer::renderCurrent(session);
    QVERIFY(afterSelection.isValid());
    QCOMPARE(afterSelection.sha256(), beforeSelection.sha256());
    QCOMPARE(afterSelection.pngBytes(), beforeSelection.pngBytes());
    QCOMPARE(afterSelection.image(), beforeSelection.image());
}

}  // namespace
}  // namespace snapask::ui::canvas

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    snapask::ui::canvas::M2CanvasTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "m2_canvas_tests.moc"
