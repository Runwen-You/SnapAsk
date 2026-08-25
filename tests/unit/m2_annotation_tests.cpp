#include "domain/annotation/AnnotationDocument.h"
#include "domain/annotation/commands/AnnotationCommands.h"

#include <QRandomGenerator>
#include <QUndoStack>
#include <QtTest>

#include <memory>

namespace snapask {
namespace {

class M2AnnotationTests final : public QObject {
    Q_OBJECT

private slots:
    void createsAndHitTestsAllAnnotationTypes();
    void everyCommandUndoesAndRedoes();
    void clearAllRestoresAnnotationsAndSelection();
    void mergedTransformsAreOneUndoStep();
    void randomSequenceRoundTripsExactly();
    void newEditDropsRedoBranch();
};

void M2AnnotationTests::createsAndHitTestsAllAnnotationTypes() {
    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(10, 20, 100, 60));
    const Annotation arrow =
        Annotation::makeArrow(QPointF(0, 0), QPointF(100, 0));
    const Annotation text =
        Annotation::makeText(QRectF(5, 5, 120, 80), QStringLiteral("中文\ntext"));
    const Annotation mosaic = Annotation::makeMosaic(
        {QPointF(20, 20), QPointF(80, 20), QPointF(80, 60)}, 20.0);

    QVERIFY(rectangle.isValid());
    QVERIFY(arrow.isValid());
    QVERIFY(text.isValid());
    QVERIFY(mosaic.isValid());
    QVERIFY(rectangle.hitTest(QPointF(50, 40)));
    QVERIFY(arrow.hitTest(QPointF(50, 2)));
    QVERIFY(text.hitTest(QPointF(20, 20)));
    QVERIFY(mosaic.hitTest(QPointF(50, 25)));
    QVERIFY(!arrow.hitTest(QPointF(50, 40), 2.0));

    const auto translated = std::get<ArrowGeometry>(
        translatedGeometry(arrow.geometry, QPointF(10, 15)));
    QCOMPARE(translated.start, QPointF(10, 15));
    QCOMPARE(translated.end, QPointF(110, 15));
}

void M2AnnotationTests::everyCommandUndoesAndRedoes() {
    AnnotationDocument document;
    QUndoStack stack;

    AnnotationStyle rectangleStyle;
    const Annotation rectangle = Annotation::makeRectangle(
        QRectF(0, 0, 30, 20), rectangleStyle, 1);
    stack.push(new AddAnnotationCommand(&document, rectangle));
    QCOMPARE(document.size(), 1);

    const AnnotationGeometry moved =
        translatedGeometry(rectangle.geometry, QPointF(8, 6));
    stack.push(new TransformAnnotationCommand(&document, rectangle.id, moved));
    QCOMPARE(document.annotation(rectangle.id)->geometry, moved);

    AnnotationStyle blueStyle = rectangleStyle;
    blueStyle.strokeColor = QColor(Qt::blue);
    blueStyle.strokeWidth = 5.0;
    stack.push(new ChangeStyleCommand(&document, rectangle.id, blueStyle));
    QCOMPARE(document.annotation(rectangle.id)->style, blueStyle);

    const Annotation text = Annotation::makeText(
        QRectF(40, 0, 120, 50), QStringLiteral("before"), {}, 2);
    stack.push(new AddAnnotationCommand(&document, text));
    stack.push(new EditTextCommand(&document, text.id, QStringLiteral("after\n中文")));
    QCOMPARE(std::get<TextGeometry>(document.annotation(text.id)->geometry).text,
             QStringLiteral("after\n中文"));

    stack.push(new AddMosaicStrokeCommand(
        &document, {QPointF(0, 40), QPointF(80, 40)}, 18.0, {}, 3));
    QCOMPARE(document.size(), 3);
    QCOMPARE(document.annotations().back().type, AnnotationType::Mosaic);

    stack.push(new RemoveAnnotationCommand(&document, rectangle.id));
    QVERIFY(document.annotation(rectangle.id) == nullptr);

    stack.undo();
    QVERIFY(document.annotation(rectangle.id) != nullptr);
    stack.undo();
    QCOMPARE(document.size(), 2);
    stack.undo();
    QCOMPARE(std::get<TextGeometry>(document.annotation(text.id)->geometry).text,
             QStringLiteral("before"));
    stack.undo();
    QVERIFY(document.annotation(text.id) == nullptr);
    stack.undo();
    QCOMPARE(document.annotation(rectangle.id)->style, rectangleStyle);
    stack.undo();
    QCOMPARE(document.annotation(rectangle.id)->geometry, rectangle.geometry);
    stack.undo();
    QVERIFY(document.isEmpty());

    while (stack.canRedo()) {
        stack.redo();
    }
    QCOMPARE(document.size(), 2);
    QVERIFY(document.annotation(rectangle.id) == nullptr);
    QCOMPARE(std::get<TextGeometry>(document.annotation(text.id)->geometry).text,
             QStringLiteral("after\n中文"));
}

void M2AnnotationTests::clearAllRestoresAnnotationsAndSelection() {
    AnnotationDocument document;
    QUndoStack stack;
    const Annotation first =
        Annotation::makeRectangle(QRectF(0, 0, 20, 20), {}, 0);
    const Annotation second =
        Annotation::makeArrow(QPointF(0, 30), QPointF(40, 30), {}, 1);
    stack.push(new AddAnnotationCommand(&document, first));
    stack.push(new AddAnnotationCommand(&document, second));
    document.setSelectedAnnotationIds({first.id});

    stack.push(new ClearAnnotationsCommand(&document));
    QVERIFY(document.isEmpty());
    QVERIFY(document.selectedAnnotationIds().isEmpty());

    stack.undo();
    QCOMPARE(document.annotations(),
             AnnotationDocument::Container({first, second}));
    QCOMPARE(document.selectedAnnotationIds(), QSet<QUuid>({first.id}));

    stack.redo();
    QVERIFY(document.isEmpty());
}

void M2AnnotationTests::mergedTransformsAreOneUndoStep() {
    AnnotationDocument document;
    QUndoStack stack;
    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(0, 0, 20, 20));
    stack.push(new AddAnnotationCommand(&document, rectangle));

    constexpr quint64 mergeGroup = 77;
    const AnnotationGeometry first =
        translatedGeometry(rectangle.geometry, QPointF(1, 0));
    const AnnotationGeometry second =
        translatedGeometry(rectangle.geometry, QPointF(2, 0));
    const AnnotationGeometry third =
        translatedGeometry(rectangle.geometry, QPointF(3, 0));
    stack.push(new TransformAnnotationCommand(&document, rectangle.id, first,
                                              mergeGroup));
    stack.push(new TransformAnnotationCommand(&document, rectangle.id, second,
                                              mergeGroup));
    stack.push(new TransformAnnotationCommand(&document, rectangle.id, third,
                                              mergeGroup));

    QCOMPARE(stack.count(), 2);
    QCOMPARE(document.annotation(rectangle.id)->geometry, third);
    stack.undo();
    QCOMPARE(document.annotation(rectangle.id)->geometry, rectangle.geometry);
    stack.redo();
    QCOMPARE(document.annotation(rectangle.id)->geometry, third);
}

void M2AnnotationTests::randomSequenceRoundTripsExactly() {
    AnnotationDocument document;
    QUndoStack stack;
    QRandomGenerator random(0x5A17C0DEu);

    QVector<QUuid> liveIds;
    for (int step = 0; step < 150; ++step) {
        const bool shouldAdd = liveIds.isEmpty() || random.bounded(100) < 58;
        if (shouldAdd) {
            const int x = random.bounded(300);
            const int y = random.bounded(200);
            const int width = 5 + random.bounded(80);
            const int height = 5 + random.bounded(60);
            Annotation annotation = Annotation::makeRectangle(
                QRectF(x, y, width, height), {}, step);
            liveIds.push_back(annotation.id);
            stack.push(new AddAnnotationCommand(&document, annotation));
        } else {
            const int liveIndex = random.bounded(liveIds.size());
            const QUuid id = liveIds[liveIndex];
            const int action = random.bounded(3);
            if (action == 0) {
                const Annotation* annotation = document.annotation(id);
                QVERIFY(annotation != nullptr);
                const QPointF delta(random.bounded(9) - 4,
                                    random.bounded(9) - 4);
                if (!delta.isNull()) {
                    stack.push(new TransformAnnotationCommand(
                        &document, id,
                        translatedGeometry(annotation->geometry, delta)));
                }
            } else if (action == 1) {
                AnnotationStyle style = document.annotation(id)->style;
                style.strokeWidth = 1.0 + random.bounded(8);
                style.strokeColor = QColor::fromHsv(random.bounded(360), 220, 240);
                stack.push(new ChangeStyleCommand(&document, id, style));
            } else {
                stack.push(new RemoveAnnotationCommand(&document, id));
                liveIds.removeAt(liveIndex);
            }
        }
    }

    const AnnotationDocument::Container finalState = document.annotations();
    const quint64 finalRevision = document.revision();
    while (stack.canUndo()) {
        stack.undo();
    }
    QVERIFY(document.isEmpty());
    QVERIFY(document.revision() > finalRevision);

    while (stack.canRedo()) {
        stack.redo();
    }
    QCOMPARE(document.annotations(), finalState);
}

void M2AnnotationTests::newEditDropsRedoBranch() {
    AnnotationDocument document;
    QUndoStack stack;
    const Annotation first =
        Annotation::makeRectangle(QRectF(0, 0, 20, 20));
    const Annotation second =
        Annotation::makeRectangle(QRectF(30, 0, 20, 20));
    stack.push(new AddAnnotationCommand(&document, first));
    stack.push(new AddAnnotationCommand(&document, second));
    stack.undo();
    QVERIFY(stack.canRedo());

    const Annotation replacement =
        Annotation::makeArrow(QPointF(0, 30), QPointF(40, 30));
    stack.push(new AddAnnotationCommand(&document, replacement));
    QVERIFY(!stack.canRedo());
    QVERIFY(document.annotation(second.id) == nullptr);
    QVERIFY(document.annotation(replacement.id) != nullptr);
}

}  // namespace
}  // namespace snapask

QTEST_APPLESS_MAIN(snapask::M2AnnotationTests)

#include "m2_annotation_tests.moc"
