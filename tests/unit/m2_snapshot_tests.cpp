#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"
#include "services/SnapshotRenderer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QtTest>

namespace snapask {
namespace {

QImage solidImage(const QSize& size, const QColor& color) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

class M2SnapshotTests final : public QObject {
    Q_OBJECT

private slots:
    void selectionChromeNeverEntersSnapshot();
    void mosaicPixelsMatchGoldenMatrix();
    void cropUsesSourcePhysicalPixels();
    void snapshotRemainsFrozenAfterFurtherEditing();
    void savedAndSentHashesAreIndependent();
    void canonicalPngAndHashAreStable();
};

void M2SnapshotTests::selectionChromeNeverEntersSnapshot() {
    ScreenshotSession session(solidImage(QSize(64, 48), Qt::white));
    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(8, 8, 30, 20));
    QVERIFY(session.annotations().addAnnotation(rectangle));
    const RenderedSnapshot beforeSelection =
        SnapshotRenderer::renderCurrent(session);
    QVERIFY(beforeSelection.isValid());

    session.annotations().setSelectedAnnotationIds({rectangle.id});
    const RenderedSnapshot afterSelection =
        SnapshotRenderer::renderCurrent(session);
    QVERIFY(afterSelection.isValid());
    QCOMPARE(afterSelection.sha256(), beforeSelection.sha256());
    QCOMPARE(afterSelection.pngBytes(), beforeSelection.pngBytes());
    QCOMPARE(afterSelection.image(), beforeSelection.image());
}

void M2SnapshotTests::mosaicPixelsMatchGoldenMatrix() {
    QImage source(4, 4, QImage::Format_ARGB32_Premultiplied);
    const int values[4][4] = {
        {0, 40, 80, 120},
        {20, 60, 100, 140},
        {160, 200, 240, 255},
        {180, 220, 250, 245},
    };
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            source.setPixelColor(x, y,
                                 QColor(values[y][x], values[y][x],
                                        values[y][x]));
        }
    }

    ScreenshotSession session(source);
    AnnotationStyle style;
    style.mosaicBlockSize = 2;
    const Annotation mosaic = Annotation::makeMosaic(
        {QPointF(1.5, 1.5)}, 100.0, style, 0);
    QVERIFY(session.annotations().addAnnotation(mosaic));

    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    QVERIFY(snapshot.isValid());
    const int expected[4][4] = {
        {30, 30, 110, 110},
        {30, 30, 110, 110},
        {190, 190, 247, 247},
        {190, 190, 247, 247},
    };
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const QColor pixel = snapshot.image().pixelColor(x, y);
            QCOMPARE(pixel.red(), expected[y][x]);
            QCOMPARE(pixel.green(), expected[y][x]);
            QCOMPARE(pixel.blue(), expected[y][x]);
            QCOMPARE(pixel.alpha(), 255);
        }
    }
    QVERIFY(snapshot.image() != source);
}

void M2SnapshotTests::cropUsesSourcePhysicalPixels() {
    QImage source(8, 6, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor(x * 20, y * 30, x + y));
        }
    }
    ScreenshotSession session(source);
    QVERIFY(session.setCropRect(QRect(2, 1, 3, 2)));

    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    QVERIFY(snapshot.isValid());
    QCOMPARE(snapshot.pixelSize(), QSize(3, 2));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            QCOMPARE(snapshot.image().pixelColor(x, y),
                     source.pixelColor(x + 2, y + 1));
        }
    }
}

void M2SnapshotTests::snapshotRemainsFrozenAfterFurtherEditing() {
    ScreenshotSession session(solidImage(QSize(48, 32), Qt::white));
    const RenderedSnapshot versionOne = SnapshotRenderer::renderCurrent(session);
    QVERIFY(versionOne.isValid());
    const QByteArray versionOneBytes = versionOne.pngBytes();
    const QImage versionOneImage = versionOne.image();

    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(4, 4, 30, 18));
    session.undoStack().push(
        new AddAnnotationCommand(&session.annotations(), rectangle));
    const RenderedSnapshot versionTwo = SnapshotRenderer::renderCurrent(session);
    QVERIFY(versionTwo.isValid());

    QVERIFY(versionOne.sha256() != versionTwo.sha256());
    QVERIFY(versionOne.revision() < versionTwo.revision());
    QCOMPARE(versionOne.pngBytes(), versionOneBytes);
    QCOMPARE(versionOne.image(), versionOneImage);
    QCOMPARE(versionOne.image().pixelColor(4, 4), QColor(Qt::white));
}

void M2SnapshotTests::savedAndSentHashesAreIndependent() {
    ScreenshotSession session(solidImage(QSize(48, 32), Qt::white));
    const RenderedSnapshot initial = SnapshotRenderer::renderCurrent(session);
    QVERIFY(initial.isValid());
    QVERIFY(session.hasUnsavedChanges(initial.sha256()));
    QVERIFY(session.hasUnsentChanges(initial.sha256()));

    session.markSavedHash(initial.sha256());
    QVERIFY(!session.hasUnsavedChanges(initial.sha256()));
    QVERIFY(session.hasUnsentChanges(initial.sha256()));
    session.markSentHash(initial.sha256());
    QVERIFY(!session.hasUnsentChanges(initial.sha256()));

    QVERIFY(session.annotations().addAnnotation(
        Annotation::makeArrow(QPointF(2, 2), QPointF(40, 20))));
    const RenderedSnapshot edited = SnapshotRenderer::renderCurrent(session);
    QVERIFY(edited.isValid());
    QVERIFY(session.hasUnsavedChanges(edited.sha256()));
    QVERIFY(session.hasUnsentChanges(edited.sha256()));
    QCOMPARE(session.lastSavedHash(), initial.sha256());
    QCOMPARE(session.lastSentHash(), initial.sha256());
}

void M2SnapshotTests::canonicalPngAndHashAreStable() {
    ScreenshotSession session(solidImage(QSize(32, 24), QColor(12, 34, 56)));
    QVERIFY(session.annotations().addAnnotation(Annotation::makeArrow(
        QPointF(2, 2), QPointF(28, 20), {}, 2)));
    const RenderedSnapshot first = SnapshotRenderer::renderCurrent(session);
    const RenderedSnapshot second = SnapshotRenderer::renderCurrent(session);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());

    QCOMPARE(first.pngBytes(), second.pngBytes());
    QCOMPARE(first.sha256(), second.sha256());
    QCOMPARE(first.sha256(),
             QCryptographicHash::hash(first.pngBytes(),
                                      QCryptographicHash::Sha256));

    QImage decoded;
    QVERIFY(decoded.loadFromData(first.pngBytes(), "PNG"));
    QCOMPARE(decoded.convertToFormat(QImage::Format_ARGB32_Premultiplied),
             first.image());

    const QByteArray saveConsumer = first.pngBytes();
    const QByteArray clipboardConsumer = first.pngBytes();
    const QByteArray pinConsumer = first.pngBytes();
    const QByteArray aiConsumer = first.pngBytes();
    QCOMPARE(QCryptographicHash::hash(saveConsumer, QCryptographicHash::Sha256),
             first.sha256());
    QCOMPARE(clipboardConsumer, saveConsumer);
    QCOMPARE(pinConsumer, saveConsumer);
    QCOMPARE(aiConsumer, saveConsumer);
}

}  // namespace
}  // namespace snapask

QTEST_GUILESS_MAIN(snapask::M2SnapshotTests)

#include "m2_snapshot_tests.moc"
