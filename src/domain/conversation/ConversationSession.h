#pragma once

#include "ai/AiTypes.h"
#include "services/SnapshotRenderer.h"

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QUuid>

#include <memory>

namespace snapask {

class SnapshotRevision final {
public:
    [[nodiscard]] const QUuid& snapshotId() const noexcept;
    [[nodiscard]] quint64 revisionNumber() const noexcept;
    [[nodiscard]] const RenderedSnapshot& renderedSnapshot() const noexcept;
    [[nodiscard]] const QByteArray& renderedHash() const noexcept;
    [[nodiscard]] const QDateTime& createdAt() const noexcept;

private:
    friend class ConversationSession;

    SnapshotRevision(QUuid snapshotId,
                     quint64 revisionNumber,
                     RenderedSnapshot renderedSnapshot,
                     QDateTime createdAt);

    QUuid snapshotId_;
    quint64 revisionNumber_{0};
    RenderedSnapshot renderedSnapshot_;
    QDateTime createdAt_;
};

struct SnapshotCacheCompactionResult final {
    qsizetype scannedOldRevisionCount{0};
    qsizetype releasedDecodedImageCount{0};
    // Logical cache bytes whose owning references were dropped. Existing
    // QImage values held by previews can delay the corresponding physical free.
    quint64 releasedDecodedImageBytes{0};
};

class ConversationTurn final {
public:
    enum class Status {
        Draft,
        Queued,
        Streaming,
        Completed,
        Failed,
        Cancelled,
    };

    [[nodiscard]] const QUuid& turnId() const noexcept;
    [[nodiscard]] const QUuid& requestId() const noexcept;
    [[nodiscard]] const QString& question() const noexcept;
    [[nodiscard]] const QUuid& providerProfileId() const noexcept;
    [[nodiscard]] const QString& modelId() const noexcept;
    [[nodiscard]] const std::shared_ptr<const SnapshotRevision>&
    snapshotRevision() const noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool isTerminal() const noexcept;
    [[nodiscard]] const QString& answer() const noexcept;
    [[nodiscard]] const QString& error() const noexcept;
    [[nodiscard]] ai::ErrorKind errorKind() const noexcept;
    [[nodiscard]] int httpStatus() const noexcept;
    [[nodiscard]] qint64 inputTokens() const noexcept;
    [[nodiscard]] qint64 outputTokens() const noexcept;

private:
    friend class ConversationSession;

    ConversationTurn(QUuid turnId,
                     QUuid requestId,
                     QString question,
                     QUuid providerProfileId,
                     QString modelId,
                     std::shared_ptr<const SnapshotRevision> snapshotRevision);

    QUuid turnId_;
    QUuid requestId_;
    QString question_;
    QUuid providerProfileId_;
    QString modelId_;
    std::shared_ptr<const SnapshotRevision> snapshotRevision_;
    Status status_{Status::Draft};
    QString answer_;
    QString error_;
    ai::ErrorKind errorKind_{ai::ErrorKind::None};
    int httpStatus_{0};
    qint64 inputTokens_{-1};
    qint64 outputTokens_{-1};
};

class ConversationSession final {
public:
    explicit ConversationSession(QUuid sessionId = QUuid::createUuid());

    ConversationSession(const ConversationSession&) = delete;
    ConversationSession& operator=(const ConversationSession&) = delete;

    [[nodiscard]] const QUuid& sessionId() const noexcept;

    // This is the explicit-send boundary. It freezes the supplied canonical
    // RenderedSnapshot into a new, monotonically numbered revision and creates
    // a queued turn. It performs no network operation.
    [[nodiscard]] const ConversationTurn* beginExplicitSend(
        const RenderedSnapshot& renderedSnapshot,
        QString question,
        QUuid providerProfileId,
        QString modelId,
        QUuid requestId = {},
        QDateTime createdAt = {});

    // Retry with exactly the bytes/version used by the original turn. No new
    // SnapshotRevision is created.
    [[nodiscard]] const ConversationTurn* retryOriginal(
        const QUuid& originalRequestId,
        QUuid newRequestId = {});

    // Retry the same question/profile/model with the caller's latest canonical
    // snapshot. This creates the next SnapshotRevision.
    [[nodiscard]] const ConversationTurn* retryWithLatestCurrent(
        const QUuid& originalRequestId,
        const RenderedSnapshot& latestSnapshot,
        QUuid newRequestId = {},
        QDateTime createdAt = {});

    // Events are accepted only for a known, non-terminal request. A terminal
    // event seals the turn, so late deltas and duplicate terminal events fail.
    [[nodiscard]] bool acceptEvent(const ai::AiStreamEvent& event);

    [[nodiscard]] qsizetype revisionCount() const noexcept;
    [[nodiscard]] qsizetype turnCount() const noexcept;
    [[nodiscard]] const SnapshotRevision* revisionAt(qsizetype index) const
        noexcept;
    [[nodiscard]] const SnapshotRevision* latestRevision() const noexcept;
    [[nodiscard]] const ConversationTurn* turnAt(qsizetype index) const
        noexcept;
    [[nodiscard]] const ConversationTurn* turnForRequest(
        const QUuid& requestId) const noexcept;
    [[nodiscard]] QList<const SnapshotRevision*> revisions() const;
    [[nodiscard]] QList<const ConversationTurn*> turns() const;

    // Releases only rebuildable decoded-image caches from old revisions. The
    // newest revision and any revision whose PNG hash is in retainedHashes stay
    // decoded. PNG bytes, hashes, dimensions, revision numbers and turn
    // bindings are never removed, so original-snapshot retry remains exact.
    // ConversationSession is UI-thread confined; RenderedSnapshot protects its
    // cache in case a preview value overlaps a release on another thread.
    [[nodiscard]] SnapshotCacheCompactionResult compactOldSnapshotImages(
        const QSet<QByteArray>& retainedHashes = {});

private:
    [[nodiscard]] ConversationTurn* mutableTurnForRequest(
        const QUuid& requestId) noexcept;
    [[nodiscard]] bool containsRequestId(const QUuid& requestId) const
        noexcept;
    [[nodiscard]] QUuid uniqueRequestId() const;
    [[nodiscard]] const ConversationTurn* appendQueuedTurn(
        std::shared_ptr<const SnapshotRevision> revision,
        QString question,
        QUuid providerProfileId,
        QString modelId,
        QUuid requestId);
    [[nodiscard]] static QDateTime normalizedCreatedAt(QDateTime createdAt);

    QUuid sessionId_;
    QList<std::shared_ptr<const SnapshotRevision>> revisions_;
    QList<std::shared_ptr<ConversationTurn>> turns_;
};

}  // namespace snapask
