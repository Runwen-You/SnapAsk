#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "ai/AiTypes.h"
#include "domain/annotation/Annotation.h"
#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"
#include "domain/conversation/ConversationSession.h"
#include "services/SnapshotRenderer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QPainter>
#include <QTest>

#include <algorithm>

using snapask::AddAnnotationCommand;
using snapask::AddMosaicStrokeCommand;
using snapask::Annotation;
using snapask::AnnotationStyle;
using snapask::ConversationSession;
using snapask::ConversationTurn;
using snapask::RenderedSnapshot;
using snapask::ScreenshotSession;
using snapask::SnapshotRenderer;
using snapask::ai::AiStreamEvent;
using snapask::ai::EventType;

namespace {

constexpr int kStressIterations = 100;
constexpr quint64 kMiB = 1024ULL * 1024ULL;

struct FlowResult {
    QString error;
    quint64 activeWorkingSetBytes{0};
};

quint64 processWorkingSetBytes()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    const BOOL success = GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        static_cast<DWORD>(sizeof(counters)));
    return success != FALSE
        ? static_cast<quint64>(counters.WorkingSetSize)
        : 0ULL;
}

QImage capturedFrame(const int iteration)
{
    QImage image(QSize(640, 360), QImage::Format_ARGB32_Premultiplied);
    const int hue = (iteration * 29) % 360;
    image.fill(QColor::fromHsv(hue, 92, 224));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(QRect(24, 22, 250, 118), QColor(245, 247, 250, 235));
    painter.fillRect(QRect(302, 62, 304, 220), QColor(25, 31, 42, 230));
    QPen gridPen(QColor(255, 255, 255, 90), 2);
    painter.setPen(gridPen);
    for (int x = 0; x < image.width(); x += 40) {
        painter.drawLine(x, 0, x, image.height());
    }
    for (int y = 0; y < image.height(); y += 40) {
        painter.drawLine(0, y, image.width(), y);
    }
    painter.setPen(QColor(16, 24, 40));
    painter.drawText(QRect(38, 38, 220, 84),
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     QStringLiteral("SnapAsk stability cycle %1\n中文 / Emoji ✓")
                         .arg(iteration + 1));
    painter.end();
    image.detach();
    return image;
}

bool completeTurn(ConversationSession& conversation,
                  const ConversationTurn* turn,
                  const QString& answer)
{
    if (turn == nullptr) {
        return false;
    }

    AiStreamEvent event;
    event.requestId = turn->requestId();
    event.type = EventType::Started;
    if (!conversation.acceptEvent(event)) {
        return false;
    }
    event.type = EventType::TextDelta;
    event.text = answer;
    if (!conversation.acceptEvent(event)) {
        return false;
    }
    event.type = EventType::Completed;
    event.text.clear();
    return conversation.acceptEvent(event);
}

FlowResult runCaptureEditQuestionCloseFlow(const int iteration)
{
    FlowResult result;
    const QUuid providerId = QUuid::createUuid();
    ScreenshotSession screenshot(capturedFrame(iteration));
    if (!screenshot.hasSourceImage()) {
        result.error = QStringLiteral("capture did not create a source image");
        return result;
    }
    if (!screenshot.setCropRect(QRect(8, 8, 624, 344))) {
        result.error = QStringLiteral("physical-pixel crop was not applied");
        return result;
    }

    AnnotationStyle style;
    style.strokeColor = QColor(255, 64, 52);
    style.strokeWidth = 4.0;
    screenshot.undoStack().push(new AddAnnotationCommand(
        &screenshot.annotations(),
        Annotation::makeRectangle(QRectF(42.0, 40.0, 226.0, 108.0),
                                  style, 0)));
    screenshot.undoStack().push(new AddAnnotationCommand(
        &screenshot.annotations(),
        Annotation::makeArrow(QPointF(286.0, 182.0), QPointF(472.0, 98.0),
                              style, 1)));
    screenshot.undoStack().push(new AddAnnotationCommand(
        &screenshot.annotations(),
        Annotation::makeText(QRectF(314.0, 250.0, 250.0, 64.0),
                             QStringLiteral("第 %1 次标注").arg(iteration + 1),
                             style, 2)));
    if (screenshot.annotations().size() != 3) {
        result.error = QStringLiteral("first edit set was not committed");
        return result;
    }

    const RenderedSnapshot first = SnapshotRenderer::renderCurrent(screenshot);
    if (!first.isValid()
        || first.sha256() != QCryptographicHash::hash(
               first.pngBytes(), QCryptographicHash::Sha256)) {
        result.error = QStringLiteral("first canonical snapshot was invalid");
        return result;
    }

    ConversationSession conversation;
    const ConversationTurn* firstTurn = conversation.beginExplicitSend(
        first, QStringLiteral("请解释第 %1 次截图").arg(iteration + 1),
        providerId, QStringLiteral("stress-vision-model"));
    if (!completeTurn(conversation, firstTurn, QStringLiteral("第一轮完成"))) {
        result.error = QStringLiteral("first conversation turn did not complete");
        return result;
    }
    screenshot.markSentHash(first.sha256());
    if (screenshot.hasUnsentChanges(first.sha256())) {
        result.error = QStringLiteral("sent hash did not bind to first snapshot");
        return result;
    }

    AnnotationStyle mosaicStyle;
    mosaicStyle.mosaicBlockSize = 10;
    screenshot.undoStack().push(new AddMosaicStrokeCommand(
        &screenshot.annotations(),
        QVector<QPointF>{QPointF(328.0, 112.0), QPointF(384.0, 132.0),
                         QPointF(446.0, 116.0)},
        34.0, mosaicStyle, 3));
    if (screenshot.annotations().size() != 4) {
        result.error = QStringLiteral("second edit was not committed");
        return result;
    }

    const RenderedSnapshot second = SnapshotRenderer::renderCurrent(screenshot);
    if (!second.isValid() || second.sha256() == first.sha256()
        || !screenshot.hasUnsentChanges(second.sha256())) {
        result.error = QStringLiteral("second snapshot was not a new frozen version");
        return result;
    }
    const ConversationTurn* secondTurn = conversation.beginExplicitSend(
        second, QStringLiteral("基于新标注继续分析"), providerId,
        QStringLiteral("stress-vision-model"));
    if (!completeTurn(conversation, secondTurn, QStringLiteral("第二轮完成"))) {
        result.error = QStringLiteral("second conversation turn did not complete");
        return result;
    }
    screenshot.markSentHash(second.sha256());

    if (conversation.revisionCount() != 2 || conversation.turnCount() != 2
        || firstTurn == nullptr || secondTurn == nullptr
        || firstTurn->status() != ConversationTurn::Status::Completed
        || secondTurn->status() != ConversationTurn::Status::Completed
        || firstTurn->snapshotRevision()->revisionNumber() != 1
        || secondTurn->snapshotRevision()->revisionNumber() != 2
        || firstTurn->snapshotRevision()->renderedHash() != first.sha256()
        || secondTurn->snapshotRevision()->renderedHash() != second.sha256()
        || firstTurn->answer() != QStringLiteral("第一轮完成")
        || secondTurn->answer() != QStringLiteral("第二轮完成")) {
        result.error = QStringLiteral("conversation versions or bindings diverged");
        return result;
    }

    screenshot.undoStack().undo();
    if (screenshot.annotations().size() != 3) {
        result.error = QStringLiteral("undo did not revert the second edit");
        return result;
    }
    screenshot.undoStack().redo();
    if (screenshot.annotations().size() != 4) {
        result.error = QStringLiteral("redo did not restore the second edit");
        return result;
    }

    result.activeWorkingSetBytes = processWorkingSetBytes();
    return result;
}

quint64 median(QList<quint64> values)
{
    if (values.isEmpty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

qint64 percentile(QList<qint64> values, const int percentileValue)
{
    if (values.isEmpty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const qsizetype index = static_cast<qsizetype>(
        (static_cast<qint64>(values.size() - 1) * percentileValue) / 100);
    return values.at(index);
}

double toMiB(const quint64 bytes)
{
    return static_cast<double>(bytes) / static_cast<double>(kMiB);
}

double projectedLinearGrowth(const QList<quint64>& samples)
{
    if (samples.size() < 2) {
        return 0.0;
    }
    const double count = static_cast<double>(samples.size());
    const double meanX = (count - 1.0) / 2.0;
    double meanY = 0.0;
    for (const quint64 sample : samples) {
        meanY += static_cast<double>(sample);
    }
    meanY /= count;

    double covariance = 0.0;
    double variance = 0.0;
    for (qsizetype index = 0; index < samples.size(); ++index) {
        const double centeredX = static_cast<double>(index) - meanX;
        covariance += centeredX
            * (static_cast<double>(samples.at(index)) - meanY);
        variance += centeredX * centeredX;
    }
    if (variance <= 0.0) {
        return 0.0;
    }
    const double slopeBytesPerCycle = covariance / variance;
    return std::max(0.0, slopeBytesPerCycle * (count - 1.0));
}

}  // namespace

class M6StressTests final : public QObject {
    Q_OBJECT

private slots:
    void oneHundredSyntheticDomainCyclesStayStable();
};

void M6StressTests::oneHundredSyntheticDomainCyclesStayStable()
{
    // Warm the image codecs, font engine and Qt allocators before measuring
    // sustained growth. These are process-wide caches, not per-session leaks.
    for (int warmup = 0; warmup < 3; ++warmup) {
        const FlowResult result = runCaptureEditQuestionCloseFlow(
            kStressIterations + warmup);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();

    const quint64 startingWorkingSet = processWorkingSetBytes();
    QVERIFY(startingWorkingSet > 0);
    quint64 peakWorkingSet = startingWorkingSet;
    QList<quint64> releasedWorkingSets;
    QList<qint64> cycleNanoseconds;
    releasedWorkingSets.reserve(kStressIterations);
    cycleNanoseconds.reserve(kStressIterations);

    for (int iteration = 0; iteration < kStressIterations; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        const FlowResult result = runCaptureEditQuestionCloseFlow(iteration);
        QVERIFY2(result.error.isEmpty(),
                 qPrintable(QStringLiteral("cycle %1: %2")
                                .arg(iteration + 1)
                                .arg(result.error)));
        QVERIFY(result.activeWorkingSetBytes > 0);
        peakWorkingSet = std::max(peakWorkingSet,
                                  result.activeWorkingSetBytes);
        cycleNanoseconds.append(timer.nsecsElapsed());

        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if ((iteration + 1) % 10 == 0) {
            QCoreApplication::processEvents();
        }
        const quint64 releasedWorkingSet = processWorkingSetBytes();
        QVERIFY(releasedWorkingSet > 0);
        releasedWorkingSets.append(releasedWorkingSet);
        peakWorkingSet = std::max(peakWorkingSet, releasedWorkingSet);
    }

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    const quint64 endingWorkingSet = processWorkingSetBytes();
    QVERIFY(endingWorkingSet > 0);
    peakWorkingSet = std::max(peakWorkingSet, endingWorkingSet);

    const QList<quint64> earlySamples = releasedWorkingSets.mid(0, 20);
    const QList<quint64> lateSamples = releasedWorkingSets.mid(
        releasedWorkingSets.size() - 20, 20);
    const quint64 earlyMedian = median(earlySamples);
    const quint64 lateMedian = median(lateSamples);
    const quint64 trendTolerance = std::max(16ULL * kMiB,
                                            earlyMedian / 4ULL);
    const quint64 endTolerance = std::max(24ULL * kMiB,
                                          startingWorkingSet / 3ULL);
    const double linearGrowth = projectedLinearGrowth(releasedWorkingSets);

    const QString memoryReport = QStringLiteral(
        "working set start=%1 MiB, peak=%2 MiB, end=%3 MiB, "
        "early-median=%4 MiB, late-median=%5 MiB, linear-growth=%6 MiB")
        .arg(toMiB(startingWorkingSet), 0, 'f', 1)
        .arg(toMiB(peakWorkingSet), 0, 'f', 1)
        .arg(toMiB(endingWorkingSet), 0, 'f', 1)
        .arg(toMiB(earlyMedian), 0, 'f', 1)
        .arg(toMiB(lateMedian), 0, 'f', 1)
        .arg(linearGrowth / static_cast<double>(kMiB), 0, 'f', 1);
    qInfo().noquote() << memoryReport;
    qInfo().noquote()
        << QStringLiteral("cycle duration p50=%1 ms, p95=%2 ms over %3 cycles")
               .arg(static_cast<double>(percentile(cycleNanoseconds, 50))
                        / 1'000'000.0,
                    0, 'f', 2)
               .arg(static_cast<double>(percentile(cycleNanoseconds, 95))
                        / 1'000'000.0,
                    0, 'f', 2)
               .arg(kStressIterations);

    QVERIFY2(lateMedian <= earlyMedian + trendTolerance,
             qPrintable(QStringLiteral("sustained working-set trend detected: ")
                        + memoryReport));
    QVERIFY2(linearGrowth <= static_cast<double>(trendTolerance),
             qPrintable(QStringLiteral("linear working-set growth detected: ")
                        + memoryReport));
    QVERIFY2(endingWorkingSet <= startingWorkingSet + endTolerance,
             qPrintable(QStringLiteral("ending working set did not recover: ")
                        + memoryReport));
}

QTEST_MAIN(M6StressTests)
#include "M6StressTests.moc"
