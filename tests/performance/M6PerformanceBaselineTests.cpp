#include "domain/annotation/Annotation.h"
#include "domain/annotation/AnnotationDocument.h"
#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"
#include "services/SnapshotRenderer.h"
#include "ui/canvas/CanvasWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace snapask {
namespace {

constexpr int kTimedSampleCount = 30;
constexpr int kWarmupSampleCount = 3;
constexpr int kFourKWidth = 3840;
constexpr int kFourKHeight = 2160;
constexpr qint64 kRenderDeadlockGuardNanoseconds = 10'000'000'000LL;
constexpr qint64 kHistoryDeadlockGuardNanoseconds = 2'000'000'000LL;
constexpr int kBaseAnnotationCount = 128;
constexpr int kAnnotationsPerType = kBaseAnnotationCount / 4;
constexpr int kExpectedHistoryCommandCount =
    kBaseAnnotationCount + (3 * kAnnotationsPerType);

struct TimingSummary final {
    qint64 p50Nanoseconds{0};
    qint64 p95Nanoseconds{0};
    qint64 maximumNanoseconds{0};
};

[[nodiscard]] QImage makeFourKSource()
{
    QImage image(kFourKWidth, kFourKHeight,
                 QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int checker = ((x / 24) + (y / 24)) % 2;
            const int red = (x * 7 + y * 3 + checker * 47) & 0xff;
            const int green = (x * 2 + y * 11 + checker * 29) & 0xff;
            const int blue = (x * 13 + y * 5 + checker * 61) & 0xff;
            row[x] = qRgb(red, green, blue);
        }
    }
    image.detach();
    return image;
}

[[nodiscard]] std::unique_ptr<ScreenshotSession> makeFourKAnnotatedSession()
{
    auto session = std::make_unique<ScreenshotSession>(makeFourKSource());

    AnnotationStyle rectangleStyle;
    rectangleStyle.strokeColor = QColor(244, 42, 38);
    rectangleStyle.strokeWidth = 10.0;
    session->undoStack().push(new AddAnnotationCommand(
        &session->annotations(),
        Annotation::makeRectangle(QRectF(120.0, 120.0, 620.0, 380.0),
                                  rectangleStyle, 0)));

    AnnotationStyle arrowStyle;
    arrowStyle.strokeColor = QColor(255, 206, 32);
    arrowStyle.strokeWidth = 12.0;
    session->undoStack().push(new AddAnnotationCommand(
        &session->annotations(),
        Annotation::makeArrow(QPointF(880.0, 520.0),
                              QPointF(1510.0, 150.0), arrowStyle, 1)));

    AnnotationStyle textStyle;
    textStyle.strokeColor = QColor(38, 132, 255);
    textStyle.font.setPixelSize(54);
    textStyle.font.setBold(true);
    session->undoStack().push(new AddAnnotationCommand(
        &session->annotations(),
        Annotation::makeText(QRectF(1740.0, 130.0, 1120.0, 330.0),
                             QStringLiteral(
                                 "SnapAsk 4K performance baseline\n"
                                 "真实像素 / PNG / SHA-256"),
                             textStyle, 2)));

    AnnotationStyle mosaicStyle;
    mosaicStyle.mosaicBlockSize = 24;
    session->undoStack().push(new AddMosaicStrokeCommand(
        &session->annotations(),
        QVector<QPointF>{QPointF(760.0, 1260.0), QPointF(1040.0, 1370.0),
                         QPointF(1320.0, 1230.0), QPointF(1550.0, 1410.0)},
        170.0, mosaicStyle, 3));

    return session;
}

[[nodiscard]] qint64 percentileNanoseconds(QList<qint64> samples,
                                           const double percentile)
{
    if (samples.isEmpty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double rank = std::ceil(percentile
                                  * static_cast<double>(samples.size()));
    const qsizetype index = std::clamp<qsizetype>(
        static_cast<qsizetype>(rank) - 1, 0, samples.size() - 1);
    return samples.at(index);
}

[[nodiscard]] TimingSummary summarize(const QList<qint64>& samples)
{
    TimingSummary result;
    result.p50Nanoseconds = percentileNanoseconds(samples, 0.50);
    result.p95Nanoseconds = percentileNanoseconds(samples, 0.95);
    if (!samples.isEmpty()) {
        result.maximumNanoseconds =
            *std::max_element(samples.cbegin(), samples.cend());
    }
    return result;
}

[[nodiscard]] double milliseconds(const qint64 nanoseconds)
{
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

[[nodiscard]] QString sampleListMilliseconds(const QList<qint64>& samples)
{
    QStringList values;
    values.reserve(samples.size());
    for (const qint64 sample : samples) {
        values.append(QString::number(milliseconds(sample), 'f', 3));
    }
    return values.join(QLatin1Char(','));
}

[[nodiscard]] qsizetype changedPixelCount(const QImage& before,
                                          const QImage& after,
                                          const QRect& region)
{
    const QRect bounded = region.normalized().intersected(before.rect())
                              .intersected(after.rect());
    qsizetype changed = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        const auto* beforeRow =
            reinterpret_cast<const QRgb*>(before.constScanLine(y));
        const auto* afterRow =
            reinterpret_cast<const QRgb*>(after.constScanLine(y));
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            if (beforeRow[x] != afterRow[x]) {
                ++changed;
            }
        }
    }
    return changed;
}

void appendHistoryBatch(AnnotationDocument* document, QUndoStack* stack)
{
    if (document == nullptr || stack == nullptr) {
        return;
    }

    QVector<QUuid> rectangleIds;
    QVector<QUuid> arrowIds;
    QVector<QUuid> textIds;
    rectangleIds.reserve(kAnnotationsPerType);
    arrowIds.reserve(kAnnotationsPerType);
    textIds.reserve(kAnnotationsPerType);

    for (int index = 0; index < kBaseAnnotationCount; ++index) {
        const int column = index % 16;
        const int row = index / 16;
        const qreal left = 20.0 + static_cast<qreal>(column * 72);
        const qreal top = 20.0 + static_cast<qreal>(row * 58);
        Annotation annotation;
        switch (index % 4) {
        case 0:
            annotation = Annotation::makeRectangle(
                QRectF(left, top, 48.0, 34.0), {}, index);
            rectangleIds.append(annotation.id);
            break;
        case 1:
            annotation = Annotation::makeArrow(
                QPointF(left, top + 34.0), QPointF(left + 48.0, top), {},
                index);
            arrowIds.append(annotation.id);
            break;
        case 2:
            annotation = Annotation::makeText(
                QRectF(left, top, 92.0, 42.0),
                QStringLiteral("annotation-%1").arg(index), {}, index);
            textIds.append(annotation.id);
            break;
        case 3:
            annotation = Annotation::makeMosaic(
                QVector<QPointF>{QPointF(left, top),
                                 QPointF(left + 24.0, top + 16.0),
                                 QPointF(left + 48.0, top + 5.0)},
                18.0, {}, index);
            break;
        default:
            Q_UNREACHABLE();
        }
        stack->push(new AddAnnotationCommand(document, annotation));
    }

    for (qsizetype index = 0; index < rectangleIds.size(); ++index) {
        const Annotation* annotation = document->annotation(
            rectangleIds.at(index));
        if (annotation == nullptr) {
            continue;
        }
        stack->push(new TransformAnnotationCommand(
            document, annotation->id,
            translatedGeometry(annotation->geometry,
                               QPointF(3.0 + static_cast<qreal>(index), 5.0)),
            0));
    }

    for (qsizetype index = 0; index < arrowIds.size(); ++index) {
        const Annotation* annotation = document->annotation(arrowIds.at(index));
        if (annotation == nullptr) {
            continue;
        }
        AnnotationStyle changedStyle = annotation->style;
        changedStyle.strokeColor = QColor::fromHsv(
            static_cast<int>((index * 19) % 360), 210, 240);
        changedStyle.strokeWidth = 4.0 + static_cast<qreal>(index % 5);
        stack->push(new ChangeStyleCommand(document, annotation->id,
                                           changedStyle, 0));
    }

    for (qsizetype index = 0; index < textIds.size(); ++index) {
        stack->push(new EditTextCommand(
            document, textIds.at(index),
            QStringLiteral("edited annotation %1 / 撤销重做").arg(index)));
    }
}

struct HistoryTimings final {
    qint64 replayNanoseconds{0};
    qint64 undoNanoseconds{0};
    qint64 redoNanoseconds{0};
    bool valid{false};
    QString error;
};

[[nodiscard]] HistoryTimings exerciseHistoryBatch()
{
    AnnotationDocument document;
    QUndoStack stack;
    HistoryTimings result;

    QElapsedTimer timer;
    timer.start();
    appendHistoryBatch(&document, &stack);
    result.replayNanoseconds = timer.nsecsElapsed();

    if (document.size() != kBaseAnnotationCount
        || stack.count() != kExpectedHistoryCommandCount
        || stack.index() != kExpectedHistoryCommandCount) {
        result.error = QStringLiteral(
            "history replay produced the wrong document or stack size");
        return result;
    }
    const AnnotationDocument::Container expectedFinal = document.annotations();
    const quint64 replayRevision = document.revision();

    timer.restart();
    while (stack.canUndo()) {
        stack.undo();
    }
    result.undoNanoseconds = timer.nsecsElapsed();
    if (!document.isEmpty() || stack.index() != 0
        || document.revision() <= replayRevision) {
        result.error = QStringLiteral(
            "full undo did not restore the empty document");
        return result;
    }

    const quint64 undoRevision = document.revision();
    timer.restart();
    while (stack.canRedo()) {
        stack.redo();
    }
    result.redoNanoseconds = timer.nsecsElapsed();
    if (document.annotations() != expectedFinal
        || stack.index() != kExpectedHistoryCommandCount
        || document.revision() <= undoRevision) {
        result.error = QStringLiteral(
            "full redo did not replay the exact final document");
        return result;
    }
    result.valid = true;

    return result;
}

void reportTiming(const QString& metric,
                  const QList<qint64>& samples,
                  const qint64 informationalTargetNanoseconds = 0)
{
    const TimingSummary timing = summarize(samples);
    qInfo().noquote()
        << QStringLiteral(
               "M6_PERF metric=%1 samples=%2 p50_ms=%3 p95_ms=%4 "
               "max_ms=%5 raw_ms=%6")
               .arg(metric)
               .arg(samples.size())
               .arg(milliseconds(timing.p50Nanoseconds), 0, 'f', 3)
               .arg(milliseconds(timing.p95Nanoseconds), 0, 'f', 3)
               .arg(milliseconds(timing.maximumNanoseconds), 0, 'f', 3)
               .arg(sampleListMilliseconds(samples));
    if (informationalTargetNanoseconds > 0
        && timing.p95Nanoseconds > informationalTargetNanoseconds) {
        qWarning().noquote()
            << QStringLiteral(
                   "M6_PERF_TARGET_MISS metric=%1 measured_p95_ms=%2 "
                   "target_ms=%3 (informational; correctness is not relaxed)")
                   .arg(metric)
                   .arg(milliseconds(timing.p95Nanoseconds), 0, 'f', 3)
                   .arg(milliseconds(informationalTargetNanoseconds),
                        0, 'f', 3);
    }
}

[[nodiscard]] bool sendDragMove(
    snapask::ui::canvas::CanvasWidget* canvas,
    const QPoint& localPosition)
{
    if (canvas == nullptr) {
        return false;
    }
    const QPointF local(localPosition);
    const QPointF global(canvas->mapToGlobal(localPosition));
    QMouseEvent event(
        QEvent::MouseMove,
        local,
        global,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    return QCoreApplication::sendEvent(canvas, &event);
}

}  // namespace

class M6PerformanceBaselineTests final : public QObject {
    Q_OBJECT

private slots:
    void fourKSnapshotRenderHasCorrectPixelsAndMeasuredDistribution();
    void fourKCanvasDragFramesHaveMeasuredDistribution();
    void annotationHistoryReplayUndoRedoHasMeasuredDistribution();
};

void M6PerformanceBaselineTests::
    fourKSnapshotRenderHasCorrectPixelsAndMeasuredDistribution()
{
    const std::unique_ptr<ScreenshotSession> session =
        makeFourKAnnotatedSession();
    QVERIFY(session != nullptr);
    QCOMPARE(session->sourceImage().size(), QSize(kFourKWidth, kFourKHeight));
    QCOMPARE(session->annotations().size(), qsizetype{4});
    QCOMPARE(session->undoStack().count(), 4);

    const RenderedSnapshot correctnessSnapshot =
        SnapshotRenderer::renderCurrent(*session);
    QVERIFY(correctnessSnapshot.isValid());
    QCOMPARE(correctnessSnapshot.pixelSize(),
             QSize(kFourKWidth, kFourKHeight));
    QCOMPARE(correctnessSnapshot.revision(), session->currentRevision());
    QVERIFY(correctnessSnapshot.revision() > 0);
    QCOMPARE(correctnessSnapshot.sha256(),
             QCryptographicHash::hash(correctnessSnapshot.pngBytes(),
                                      QCryptographicHash::Sha256));
    QVERIFY(correctnessSnapshot.pngBytes().startsWith(
        QByteArray::fromHex("89504e470d0a1a0a")));

    const QImage rendered = correctnessSnapshot.image();
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.size(), QSize(kFourKWidth, kFourKHeight));
    const QImage decoded = QImage::fromData(correctnessSnapshot.pngBytes(),
                                            "PNG");
    QVERIFY(!decoded.isNull());
    QCOMPARE(decoded.size(), rendered.size());
    QCOMPARE(decoded.convertToFormat(QImage::Format_ARGB32_Premultiplied),
             rendered);

    const QImage& source = session->sourceImage();
    QVERIFY(changedPixelCount(source, rendered,
                              QRect(105, 105, 655, 415)) > 2'000);
    QVERIFY(changedPixelCount(source, rendered,
                              QRect(850, 120, 700, 440)) > 1'000);
    QVERIFY(changedPixelCount(source, rendered,
                              QRect(1730, 120, 1140, 350)) > 2'000);
    QVERIFY(changedPixelCount(source, rendered,
                              QRect(650, 1110, 1010, 430)) > 20'000);

    for (int warmup = 0; warmup < kWarmupSampleCount; ++warmup) {
        const RenderedSnapshot snapshot =
            SnapshotRenderer::renderCurrent(*session);
        QVERIFY(snapshot.isValid());
        QCOMPARE(snapshot.sha256(), correctnessSnapshot.sha256());
    }

    QList<qint64> samples;
    samples.reserve(kTimedSampleCount);
    for (int sampleIndex = 0; sampleIndex < kTimedSampleCount; ++sampleIndex) {
        QElapsedTimer timer;
        timer.start();
        const RenderedSnapshot snapshot =
            SnapshotRenderer::renderCurrent(*session);
        const qint64 elapsed = timer.nsecsElapsed();

        QVERIFY2(elapsed > 0,
                 "QElapsedTimer did not produce a positive render sample");
        QVERIFY2(elapsed < kRenderDeadlockGuardNanoseconds,
                 "one 4K render exceeded the 10 second deadlock guard");
        QVERIFY(snapshot.isValid());
        QCOMPARE(snapshot.pixelSize(), QSize(kFourKWidth, kFourKHeight));
        QCOMPARE(snapshot.revision(), session->currentRevision());
        QCOMPARE(snapshot.sha256(), correctnessSnapshot.sha256());
        QCOMPARE(snapshot.pngBytes(), correctnessSnapshot.pngBytes());
        QCOMPARE(snapshot.sha256(),
                 QCryptographicHash::hash(snapshot.pngBytes(),
                                          QCryptographicHash::Sha256));
        samples.append(elapsed);
    }

    QCOMPARE(samples.size(), kTimedSampleCount);
    qInfo().noquote()
        << QStringLiteral("M6_PERF snapshot_png_bytes=%1 snapshot_png_mib=%2")
               .arg(correctnessSnapshot.pngBytes().size())
               .arg(static_cast<double>(correctnessSnapshot.pngBytes().size())
                        / (1024.0 * 1024.0),
                    0, 'f', 2);
    reportTiming(QStringLiteral("snapshot_render_3840x2160_png"), samples,
                 300'000'000LL);
    const TimingSummary renderTiming = summarize(samples);
    QVERIFY2(
        renderTiming.p95Nanoseconds <= 300'000'000LL,
        "4K canonical PNG p95 exceeded the 300 ms release target");
}

void M6PerformanceBaselineTests::
    fourKCanvasDragFramesHaveMeasuredDistribution()
{
    std::unique_ptr<ScreenshotSession> session = makeFourKAnnotatedSession();
    QVERIFY(session != nullptr);
    const Annotation* rectangle = session->annotations().annotations().isEmpty()
        ? nullptr
        : &session->annotations().annotations().first();
    QVERIFY(rectangle != nullptr);
    const QUuid rectangleId = rectangle->id;
    const QRectF originalBounds = rectangle->bounds();

    snapask::ui::canvas::CanvasWidget canvas(session.get());
    canvas.setFixedSize(960, 720);
    canvas.show();
    QCoreApplication::processEvents();
    QVERIFY(canvas.isVisible());
    (void)canvas.grab();

    const QPoint start = canvas.mapImageToWidget(
        QPointF(originalBounds.left(), originalBounds.top() + 120.0))
                             .toPoint();
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, start);

    // The one-time background-without-selection frame is measured separately.
    QElapsedTimer startTimer;
    startTimer.start();
    QVERIFY(sendDragMove(&canvas, start + QPoint(1, 0)));
    canvas.repaint();
    const qint64 interactionStart = startTimer.nsecsElapsed();
    QVERIFY(interactionStart > 0);
    QVERIFY(interactionStart < kRenderDeadlockGuardNanoseconds);
    qInfo().noquote()
        << QStringLiteral("M6_PERF metric=canvas_drag_start_4k samples=1 "
                          "elapsed_ms=%1")
               .arg(milliseconds(interactionStart), 0, 'f', 3);
    QVERIFY2(
        interactionStart <= 33'000'000LL,
        "4K drag interaction start exceeded the 33 ms worst-frame target");

    QList<qint64> samples;
    samples.reserve(kTimedSampleCount);
    QPoint finalPosition = start;
    for (int index = 0; index < kTimedSampleCount; ++index) {
        finalPosition = start + QPoint(index + 2, (index % 5) - 2);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(sendDragMove(&canvas, finalPosition));
        canvas.repaint();
        const qint64 elapsed = timer.nsecsElapsed();
        QVERIFY(elapsed > 0);
        QVERIFY(elapsed < kHistoryDeadlockGuardNanoseconds);
        samples.append(elapsed);
    }
    QTest::mouseRelease(
        &canvas, Qt::LeftButton, Qt::NoModifier, finalPosition);
    QCoreApplication::processEvents();

    const Annotation* moved = session->annotations().annotation(rectangleId);
    QVERIFY(moved != nullptr);
    QVERIFY(moved->bounds() != originalBounds);
    QCOMPARE(samples.size(), kTimedSampleCount);
    reportTiming(QStringLiteral("canvas_drag_frame_4k_960x720"), samples,
                 16'700'000LL);
    const TimingSummary timing = summarize(samples);
    QVERIFY2(
        timing.p95Nanoseconds <= 16'700'000LL,
        "4K drag-frame p95 exceeded the 16.7 ms release target");
    QVERIFY2(
        timing.maximumNanoseconds <= 33'000'000LL,
        "4K drag worst frame exceeded the 33 ms release target");
}

void M6PerformanceBaselineTests::
    annotationHistoryReplayUndoRedoHasMeasuredDistribution()
{
    for (int warmup = 0; warmup < kWarmupSampleCount; ++warmup) {
        const HistoryTimings timings = exerciseHistoryBatch();
        QVERIFY2(timings.valid, qPrintable(timings.error));
        QVERIFY(timings.replayNanoseconds > 0);
        QVERIFY(timings.undoNanoseconds > 0);
        QVERIFY(timings.redoNanoseconds > 0);
    }

    QList<qint64> replaySamples;
    QList<qint64> undoSamples;
    QList<qint64> redoSamples;
    replaySamples.reserve(kTimedSampleCount);
    undoSamples.reserve(kTimedSampleCount);
    redoSamples.reserve(kTimedSampleCount);

    for (int sampleIndex = 0; sampleIndex < kTimedSampleCount; ++sampleIndex) {
        const HistoryTimings timings = exerciseHistoryBatch();
        QVERIFY2(timings.valid, qPrintable(timings.error));
        QVERIFY2(timings.replayNanoseconds > 0
                     && timings.replayNanoseconds
                            < kHistoryDeadlockGuardNanoseconds,
                 "history replay exceeded its two second deadlock guard");
        QVERIFY2(timings.undoNanoseconds > 0
                     && timings.undoNanoseconds
                            < kHistoryDeadlockGuardNanoseconds,
                 "history undo exceeded its two second deadlock guard");
        QVERIFY2(timings.redoNanoseconds > 0
                     && timings.redoNanoseconds
                            < kHistoryDeadlockGuardNanoseconds,
                 "history redo exceeded its two second deadlock guard");
        replaySamples.append(timings.replayNanoseconds);
        undoSamples.append(timings.undoNanoseconds);
        redoSamples.append(timings.redoNanoseconds);
    }

    QCOMPARE(replaySamples.size(), kTimedSampleCount);
    QCOMPARE(undoSamples.size(), kTimedSampleCount);
    QCOMPARE(redoSamples.size(), kTimedSampleCount);
    reportTiming(QStringLiteral("annotation_history_replay_224_commands"),
                 replaySamples);
    reportTiming(QStringLiteral("annotation_history_undo_224_commands"),
                 undoSamples);
    reportTiming(QStringLiteral("annotation_history_redo_224_commands"),
                 redoSamples);
}

}  // namespace snapask

QTEST_MAIN(snapask::M6PerformanceBaselineTests)

#include "M6PerformanceBaselineTests.moc"
