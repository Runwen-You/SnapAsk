#include "ai/AiTypes.h"
#include "domain/annotation/Annotation.h"
#include "domain/capture/ScreenshotSession.h"
#include "domain/conversation/ConversationSession.h"
#include "services/SnapshotRenderer.h"

#include <QtTest>

#include <utility>

namespace snapask {
namespace {

QImage solidImage(const QColor& color) {
    QImage image(QSize(48, 32), QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

ai::AiStreamEvent eventFor(ai::EventType type,
                           const QUuid& requestId,
                           QString text = {}) {
    ai::AiStreamEvent event;
    event.type = type;
    event.requestId = requestId;
    event.text = std::move(text);
    return event;
}

class M3ConversationTests final : public QObject {
    Q_OBJECT

private slots:
    void explicitSendsFreezeMonotonicImmutableVersions();
    void eventsRouteByRequestAndTerminalRejectsLateData();
    void failureKeepsPartialAnswerAndErrorDetails();
    void retryOriginalReusesExactFrozenRevision();
    void retryLatestCurrentCreatesNextVersion();
    void invalidOrDuplicateSendsDoNotMutateSession();
    void releasedSnapshotStaysValidAndLazilyDecodesExactPixels();
    void compactionReleasesMultipleOldVersionsAndKeepsExactRetryBytes();
    void sameHashReusesEncodedStorageWithoutDuplicateDecodedImage();
};

void M3ConversationTests::explicitSendsFreezeMonotonicImmutableVersions() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    const RenderedSnapshot renderedV1 =
        SnapshotRenderer::renderCurrent(screenshot);
    QVERIFY(renderedV1.isValid());
    const QByteArray v1Bytes = renderedV1.pngBytes();
    const QImage v1Image = renderedV1.image();

    const QUuid sessionId = QUuid::createUuid();
    const QUuid profileId = QUuid::createUuid();
    const QUuid requestV1 = QUuid::createUuid();
    const QDateTime createdV1(
        QDate(2026, 8, 25), QTime(1, 2, 3), QTimeZone::UTC);
    ConversationSession conversation(sessionId);
    const ConversationTurn* turnV1 = conversation.beginExplicitSend(
        renderedV1, QStringLiteral("first"), profileId,
        QStringLiteral("model-a"), requestV1, createdV1);
    QVERIFY(turnV1 != nullptr);
    QCOMPARE(conversation.sessionId(), sessionId);
    QCOMPARE(turnV1->status(), ConversationTurn::Status::Queued);
    QCOMPARE(turnV1->snapshotRevision()->revisionNumber(), quint64{1});
    QCOMPARE(turnV1->snapshotRevision()->createdAt(), createdV1);
    QCOMPARE(turnV1->snapshotRevision()->renderedHash(), renderedV1.sha256());
    QCOMPARE(turnV1->snapshotRevision()->renderedSnapshot().pngBytes(),
             renderedV1.pngBytes());

    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeRectangle(QRectF(4, 4, 24, 16))));
    const RenderedSnapshot renderedV2 =
        SnapshotRenderer::renderCurrent(screenshot);
    QVERIFY(renderedV2.isValid());
    QVERIFY(renderedV2.sha256() != renderedV1.sha256());

    const ConversationTurn* turnV2 = conversation.beginExplicitSend(
        renderedV2, QStringLiteral("second"), profileId,
        QStringLiteral("model-a"), QUuid::createUuid());
    QVERIFY(turnV2 != nullptr);
    QCOMPARE(conversation.revisionCount(), qsizetype{2});
    QCOMPARE(conversation.turnCount(), qsizetype{2});
    QCOMPARE(turnV2->snapshotRevision()->revisionNumber(), quint64{2});
    QVERIFY(turnV1->snapshotRevision() != turnV2->snapshotRevision());
    QVERIFY(turnV1->snapshotRevision()->snapshotId() !=
            turnV2->snapshotRevision()->snapshotId());

    QCOMPARE(turnV1->snapshotRevision()->renderedSnapshot().pngBytes(),
             v1Bytes);
    QCOMPARE(turnV1->snapshotRevision()->renderedSnapshot().image(), v1Image);
    QCOMPARE(turnV1->snapshotRevision()->renderedHash(), renderedV1.sha256());
    QCOMPARE(conversation.revisionAt(0), turnV1->snapshotRevision().get());
    QCOMPARE(conversation.latestRevision(), turnV2->snapshotRevision().get());
}

void M3ConversationTests::eventsRouteByRequestAndTerminalRejectsLateData() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    const RenderedSnapshot snapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    ConversationSession conversation;
    const QUuid profileId = QUuid::createUuid();
    const QUuid firstRequest = QUuid::createUuid();
    const QUuid secondRequest = QUuid::createUuid();
    const ConversationTurn* first = conversation.beginExplicitSend(
        snapshot, QStringLiteral("one"), profileId, QStringLiteral("model"),
        firstRequest);
    const ConversationTurn* second = conversation.beginExplicitSend(
        snapshot, QStringLiteral("two"), profileId, QStringLiteral("model"),
        secondRequest);
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);

    QVERIFY(!conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, QUuid::createUuid(), QStringLiteral("lost"))));
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Started, firstRequest)));
    QCOMPARE(first->status(), ConversationTurn::Status::Streaming);
    QVERIFY(!conversation.acceptEvent(
        eventFor(ai::EventType::Started, firstRequest)));
    QVERIFY(conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, firstRequest, QStringLiteral("hello "))));

    ai::AiStreamEvent usage =
        eventFor(ai::EventType::UsageUpdated, firstRequest);
    usage.inputTokens = 12;
    usage.outputTokens = 7;
    QVERIFY(conversation.acceptEvent(usage));
    QCOMPARE(first->inputTokens(), qint64{12});
    QCOMPARE(first->outputTokens(), qint64{7});

    QVERIFY(conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, firstRequest, QStringLiteral("world"))));
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, firstRequest)));
    QCOMPARE(first->status(), ConversationTurn::Status::Completed);
    QCOMPARE(first->answer(), QStringLiteral("hello world"));
    QVERIFY(first->isTerminal());

    QVERIFY(!conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, firstRequest, QStringLiteral(" late"))));
    QVERIFY(!conversation.acceptEvent(
        eventFor(ai::EventType::Failed, firstRequest)));
    QCOMPARE(first->answer(), QStringLiteral("hello world"));
    QCOMPARE(first->status(), ConversationTurn::Status::Completed);
    QCOMPARE(second->status(), ConversationTurn::Status::Queued);
    QVERIFY(second->answer().isEmpty());
}

void M3ConversationTests::failureKeepsPartialAnswerAndErrorDetails() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    ConversationSession conversation;
    const QUuid requestId = QUuid::createUuid();
    const ConversationTurn* turn = conversation.beginExplicitSend(
        SnapshotRenderer::renderCurrent(screenshot), QStringLiteral("question"),
        QUuid::createUuid(), QStringLiteral("model"), requestId);
    QVERIFY(turn != nullptr);

    QVERIFY(conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, requestId, QStringLiteral("partial answer"))));
    ai::AiStreamEvent failed = eventFor(ai::EventType::Failed, requestId);
    failed.errorKind = ai::ErrorKind::RateLimited;
    failed.errorMessage = QStringLiteral("try later");
    failed.httpStatus = 429;
    QVERIFY(conversation.acceptEvent(failed));

    QCOMPARE(turn->status(), ConversationTurn::Status::Failed);
    QCOMPARE(turn->answer(), QStringLiteral("partial answer"));
    QCOMPARE(turn->error(), QStringLiteral("try later"));
    QCOMPARE(turn->errorKind(), ai::ErrorKind::RateLimited);
    QCOMPARE(turn->httpStatus(), 429);
    QVERIFY(!conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, requestId, QStringLiteral(" discarded"))));
    QCOMPARE(turn->answer(), QStringLiteral("partial answer"));
}

void M3ConversationTests::retryOriginalReusesExactFrozenRevision() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    ConversationSession conversation;
    const QUuid requestId = QUuid::createUuid();
    const ConversationTurn* original = conversation.beginExplicitSend(
        SnapshotRenderer::renderCurrent(screenshot), QStringLiteral("retry me"),
        QUuid::createUuid(), QStringLiteral("model-a"), requestId);
    QVERIFY(original != nullptr);
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, requestId)));

    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeArrow(QPointF(2, 2), QPointF(40, 24))));
    const RenderedSnapshot edited = SnapshotRenderer::renderCurrent(screenshot);
    QVERIFY(edited.sha256() != original->snapshotRevision()->renderedHash());

    const QUuid retryRequest = QUuid::createUuid();
    const ConversationTurn* retry =
        conversation.retryOriginal(requestId, retryRequest);
    QVERIFY(retry != nullptr);
    QCOMPARE(conversation.revisionCount(), qsizetype{1});
    QCOMPARE(conversation.turnCount(), qsizetype{2});
    QCOMPARE(retry->snapshotRevision(), original->snapshotRevision());
    QCOMPARE(retry->snapshotRevision()->revisionNumber(), quint64{1});
    QCOMPARE(retry->question(), original->question());
    QCOMPARE(retry->providerProfileId(), original->providerProfileId());
    QCOMPARE(retry->modelId(), original->modelId());
    QCOMPARE(retry->status(), ConversationTurn::Status::Queued);
    QVERIFY(retry->answer().isEmpty());
    QVERIFY(retry->error().isEmpty());
}

void M3ConversationTests::retryLatestCurrentCreatesNextVersion() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    ConversationSession conversation;
    const QUuid originalRequest = QUuid::createUuid();
    const ConversationTurn* original = conversation.beginExplicitSend(
        SnapshotRenderer::renderCurrent(screenshot), QStringLiteral("explain"),
        QUuid::createUuid(), QStringLiteral("model-a"), originalRequest);
    QVERIFY(original != nullptr);
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, originalRequest)));

    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeRectangle(QRectF(5, 5, 30, 20))));
    const RenderedSnapshot latest = SnapshotRenderer::renderCurrent(screenshot);
    const QDateTime createdV2(
        QDate(2026, 8, 25), QTime(5, 6, 7), QTimeZone::UTC);
    const ConversationTurn* retry = conversation.retryWithLatestCurrent(
        originalRequest, latest, QUuid::createUuid(), createdV2);
    QVERIFY(retry != nullptr);

    QCOMPARE(conversation.revisionCount(), qsizetype{2});
    QCOMPARE(retry->snapshotRevision()->revisionNumber(), quint64{2});
    QCOMPARE(retry->snapshotRevision()->createdAt(), createdV2);
    QCOMPARE(retry->snapshotRevision()->renderedHash(), latest.sha256());
    QCOMPARE(retry->question(), original->question());
    QCOMPARE(retry->providerProfileId(), original->providerProfileId());
    QCOMPARE(retry->modelId(), original->modelId());
    QVERIFY(retry->snapshotRevision() != original->snapshotRevision());
    QCOMPARE(original->snapshotRevision()->revisionNumber(), quint64{1});
    QVERIFY(original->snapshotRevision()->renderedHash() !=
            retry->snapshotRevision()->renderedHash());

    QVERIFY(!conversation.retryOriginal(retry->requestId(), QUuid::createUuid()));
    ai::AiStreamEvent cancelled =
        eventFor(ai::EventType::Cancelled, retry->requestId());
    cancelled.errorMessage = QStringLiteral("stopped");
    QVERIFY(conversation.acceptEvent(cancelled));
    QCOMPARE(retry->status(), ConversationTurn::Status::Cancelled);
    QCOMPARE(retry->errorKind(), ai::ErrorKind::Cancelled);
    QVERIFY(!conversation.acceptEvent(eventFor(
        ai::EventType::TextDelta, retry->requestId(), QStringLiteral("late"))));
    QVERIFY(conversation.retryOriginal(retry->requestId(),
                                       QUuid::createUuid()) != nullptr);
}

void M3ConversationTests::invalidOrDuplicateSendsDoNotMutateSession() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    const RenderedSnapshot snapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    ConversationSession conversation;
    const QUuid profileId = QUuid::createUuid();
    const QUuid requestId = QUuid::createUuid();

    QVERIFY(conversation.beginExplicitSend(
                RenderedSnapshot{}, QStringLiteral("question"), profileId,
                QStringLiteral("model"), QUuid::createUuid()) == nullptr);
    QVERIFY(conversation.beginExplicitSend(
                snapshot, QStringLiteral("   "), profileId,
                QStringLiteral("model"), QUuid::createUuid()) == nullptr);
    QVERIFY(conversation.beginExplicitSend(
                snapshot, QStringLiteral("question"), QUuid{},
                QStringLiteral("model"), QUuid::createUuid()) == nullptr);
    QVERIFY(conversation.beginExplicitSend(
                snapshot, QStringLiteral("question"), profileId,
                QStringLiteral("  "), QUuid::createUuid()) == nullptr);
    QCOMPARE(conversation.revisionCount(), qsizetype{0});
    QCOMPARE(conversation.turnCount(), qsizetype{0});

    QVERIFY(conversation.beginExplicitSend(
                snapshot, QStringLiteral("question"), profileId,
                QStringLiteral("model"), requestId) != nullptr);
    QVERIFY(conversation.beginExplicitSend(
                snapshot, QStringLiteral("duplicate"), profileId,
                QStringLiteral("model"), requestId) == nullptr);
    QCOMPARE(conversation.revisionCount(), qsizetype{1});
    QCOMPARE(conversation.turnCount(), qsizetype{1});
    QVERIFY(conversation.revisionAt(-1) == nullptr);
    QVERIFY(conversation.revisionAt(1) == nullptr);
    QVERIFY(conversation.turnAt(-1) == nullptr);
    QVERIFY(conversation.turnAt(1) == nullptr);
    QCOMPARE(conversation.revisions().size(), qsizetype{1});
    QCOMPARE(conversation.turns().size(), qsizetype{1});
}

void M3ConversationTests::releasedSnapshotStaysValidAndLazilyDecodesExactPixels() {
    ScreenshotSession screenshot(solidImage(QColor(25, 80, 170, 230)));
    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeArrow(QPointF(3, 4), QPointF(42, 27))));

    const RenderedSnapshot snapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    QVERIFY(snapshot.isValid());
    QVERIFY(snapshot.hasDecodedImage());
    const QImage expectedPixels = snapshot.image();
    const QByteArray expectedPng = snapshot.pngBytes();
    const QByteArray expectedHash = snapshot.sha256();
    const QSize expectedSize = snapshot.pixelSize();
    const quint64 expectedRevision = snapshot.revision();
    const quint64 decodedBytes = snapshot.decodedImageByteSize();
    QVERIFY(decodedBytes > 0);

    RenderedSnapshot valueCopy = snapshot;
    QCOMPARE(valueCopy.releaseDecodedImage(), decodedBytes);
    QVERIFY(!valueCopy.hasDecodedImage());
    QVERIFY(snapshot.hasDecodedImage());
    QVERIFY(valueCopy.isValid());
    QCOMPARE(valueCopy.pngBytes(), expectedPng);
    QCOMPARE(valueCopy.sha256(), expectedHash);
    QCOMPARE(valueCopy.pixelSize(), expectedSize);
    QCOMPARE(valueCopy.revision(), expectedRevision);

    const QImage lazilyDecoded = valueCopy.image();
    QVERIFY(!lazilyDecoded.isNull());
    QCOMPARE(lazilyDecoded, expectedPixels);
    QVERIFY(valueCopy.hasDecodedImage());
    QCOMPARE(valueCopy.pngBytes(), expectedPng);
    QCOMPARE(valueCopy.sha256(), expectedHash);
}

void M3ConversationTests::compactionReleasesMultipleOldVersionsAndKeepsExactRetryBytes() {
    ScreenshotSession screenshot(solidImage(Qt::white));
    ConversationSession conversation;
    const QUuid profileId = QUuid::createUuid();

    const RenderedSnapshot firstSnapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    const QByteArray firstPng = firstSnapshot.pngBytes();
    const QByteArray firstHash = firstSnapshot.sha256();
    const QImage firstPixels = firstSnapshot.image();
    const QUuid firstRequest = QUuid::createUuid();
    const ConversationTurn* firstTurn = conversation.beginExplicitSend(
        firstSnapshot, QStringLiteral("first"), profileId,
        QStringLiteral("model"), firstRequest);
    QVERIFY(firstTurn != nullptr);
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, firstRequest)));

    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeRectangle(QRectF(2, 2, 12, 10))));
    const RenderedSnapshot secondSnapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    const QUuid secondRequest = QUuid::createUuid();
    QVERIFY(conversation.beginExplicitSend(
                secondSnapshot, QStringLiteral("second"), profileId,
                QStringLiteral("model"), secondRequest) != nullptr);
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, secondRequest)));

    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeArrow(QPointF(1, 30), QPointF(45, 5))));
    const RenderedSnapshot thirdSnapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    const QUuid thirdRequest = QUuid::createUuid();
    QVERIFY(conversation.beginExplicitSend(
                thirdSnapshot, QStringLiteral("third"), profileId,
                QStringLiteral("model"), thirdRequest) != nullptr);
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, thirdRequest)));

    QVERIFY(screenshot.annotations().addAnnotation(
        Annotation::makeRectangle(QRectF(28, 14, 16, 14))));
    const RenderedSnapshot fourthSnapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    QVERIFY(conversation.beginExplicitSend(
                fourthSnapshot, QStringLiteral("fourth"), profileId,
                QStringLiteral("model"), QUuid::createUuid()) != nullptr);
    QCOMPARE(conversation.revisionCount(), qsizetype{4});

    const SnapshotRevision* firstRevision = conversation.revisionAt(0);
    const SnapshotRevision* secondRevision = conversation.revisionAt(1);
    const SnapshotRevision* thirdRevision = conversation.revisionAt(2);
    const SnapshotRevision* latestRevision = conversation.revisionAt(3);
    QVERIFY(firstRevision != nullptr);
    QVERIFY(secondRevision != nullptr);
    QVERIFY(thirdRevision != nullptr);
    QVERIFY(latestRevision != nullptr);
    const quint64 expectedReleasedBytes =
        firstRevision->renderedSnapshot().decodedImageByteSize() +
        thirdRevision->renderedSnapshot().decodedImageByteSize();

    const SnapshotCacheCompactionResult compacted =
        conversation.compactOldSnapshotImages(
            QSet<QByteArray>{secondRevision->renderedHash()});
    QCOMPARE(compacted.scannedOldRevisionCount, qsizetype{3});
    QCOMPARE(compacted.releasedDecodedImageCount, qsizetype{2});
    QCOMPARE(compacted.releasedDecodedImageBytes, expectedReleasedBytes);
    QVERIFY(!firstRevision->renderedSnapshot().hasDecodedImage());
    QVERIFY(secondRevision->renderedSnapshot().hasDecodedImage());
    QVERIFY(!thirdRevision->renderedSnapshot().hasDecodedImage());
    QVERIFY(latestRevision->renderedSnapshot().hasDecodedImage());

    QVERIFY(firstRevision->renderedSnapshot().isValid());
    QCOMPARE(firstRevision->renderedSnapshot().pngBytes(), firstPng);
    QCOMPARE(firstRevision->renderedHash(), firstHash);
    const ConversationTurn* retry =
        conversation.retryOriginal(firstRequest, QUuid::createUuid());
    QVERIFY(retry != nullptr);
    QCOMPARE(retry->snapshotRevision().get(), firstRevision);
    QCOMPARE(retry->snapshotRevision()->renderedSnapshot().pngBytes(),
             firstPng);
    QCOMPARE(retry->snapshotRevision()->renderedHash(), firstHash);

    const QImage lazyRetryPreview =
        retry->snapshotRevision()->renderedSnapshot().image();
    QCOMPARE(lazyRetryPreview, firstPixels);
    QVERIFY(firstRevision->renderedSnapshot().hasDecodedImage());
    QCOMPARE(firstRevision->renderedSnapshot().pngBytes(), firstPng);
    QCOMPARE(firstRevision->renderedHash(), firstHash);
}

void M3ConversationTests::sameHashReusesEncodedStorageWithoutDuplicateDecodedImage() {
    ScreenshotSession screenshot(solidImage(QColor(70, 120, 190)));
    ConversationSession conversation;
    const QUuid profileId = QUuid::createUuid();
    const QUuid firstRequest = QUuid::createUuid();

    const RenderedSnapshot firstSnapshot =
        SnapshotRenderer::renderCurrent(screenshot);
    const QImage expectedPixels = firstSnapshot.image();
    const ConversationTurn* firstTurn = conversation.beginExplicitSend(
        firstSnapshot, QStringLiteral("first"), profileId,
        QStringLiteral("model"), firstRequest);
    QVERIFY(firstTurn != nullptr);
    QVERIFY(conversation.acceptEvent(
        eventFor(ai::EventType::Completed, firstRequest)));

    // Render the unchanged canvas again to model a later explicit send. The
    // value is newly encoded but byte-identical and has its own decoded image.
    const RenderedSnapshot samePixels =
        SnapshotRenderer::renderCurrent(screenshot);
    QVERIFY(samePixels.isValid());
    QVERIFY(samePixels.hasDecodedImage());
    QCOMPARE(samePixels.sha256(), firstSnapshot.sha256());
    QCOMPARE(samePixels.pngBytes(), firstSnapshot.pngBytes());

    const ConversationTurn* secondTurn = conversation.beginExplicitSend(
        samePixels, QStringLiteral("second"), profileId,
        QStringLiteral("model"), QUuid::createUuid());
    QVERIFY(secondTurn != nullptr);
    QCOMPARE(conversation.revisionCount(), qsizetype{2});
    QVERIFY(firstTurn->snapshotRevision() != secondTurn->snapshotRevision());
    QVERIFY(firstTurn->snapshotRevision()->snapshotId() !=
            secondTurn->snapshotRevision()->snapshotId());
    QCOMPARE(firstTurn->snapshotRevision()->revisionNumber(), quint64{1});
    QCOMPARE(secondTurn->snapshotRevision()->revisionNumber(), quint64{2});
    QCOMPARE(secondTurn->snapshotRevision()->renderedSnapshot().revision(),
             samePixels.revision());

    const RenderedSnapshot& firstFrozen =
        firstTurn->snapshotRevision()->renderedSnapshot();
    const RenderedSnapshot& secondFrozen =
        secondTurn->snapshotRevision()->renderedSnapshot();
    QCOMPARE(firstFrozen.pngBytes(), secondFrozen.pngBytes());
    QCOMPARE(firstFrozen.sha256(), secondFrozen.sha256());
    QCOMPARE(static_cast<const void*>(firstFrozen.pngBytes().constData()),
             static_cast<const void*>(secondFrozen.pngBytes().constData()));
    QVERIFY(firstFrozen.hasDecodedImage());
    QVERIFY(!secondFrozen.hasDecodedImage());
    QVERIFY(secondFrozen.isValid());

    const ConversationTurn* exactRetry =
        conversation.retryOriginal(firstRequest, QUuid::createUuid());
    QVERIFY(exactRetry != nullptr);
    QCOMPARE(exactRetry->snapshotRevision(), firstTurn->snapshotRevision());
    QCOMPARE(exactRetry->snapshotRevision()->renderedSnapshot().pngBytes(),
             firstSnapshot.pngBytes());
    QCOMPARE(exactRetry->snapshotRevision()->renderedHash(),
             firstSnapshot.sha256());

    QCOMPARE(secondFrozen.image(), expectedPixels);
    QVERIFY(secondFrozen.hasDecodedImage());
    QCOMPARE(secondFrozen.pngBytes(), firstSnapshot.pngBytes());
}

}  // namespace
}  // namespace snapask

QTEST_GUILESS_MAIN(snapask::M3ConversationTests)

#include "m3_conversation_tests.moc"
