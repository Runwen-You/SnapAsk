#include "domain/conversation/ConversationSession.h"

#include <limits>
#include <utility>

namespace snapask {

SnapshotRevision::SnapshotRevision(QUuid snapshotId,
                                   quint64 revisionNumber,
                                   RenderedSnapshot renderedSnapshot,
                                   QDateTime createdAt)
    : snapshotId_(std::move(snapshotId)),
      revisionNumber_(revisionNumber),
      renderedSnapshot_(std::move(renderedSnapshot)),
      createdAt_(std::move(createdAt)) {}

const QUuid& SnapshotRevision::snapshotId() const noexcept {
    return snapshotId_;
}

quint64 SnapshotRevision::revisionNumber() const noexcept {
    return revisionNumber_;
}

const RenderedSnapshot& SnapshotRevision::renderedSnapshot() const noexcept {
    return renderedSnapshot_;
}

const QByteArray& SnapshotRevision::renderedHash() const noexcept {
    return renderedSnapshot_.sha256();
}

const QDateTime& SnapshotRevision::createdAt() const noexcept {
    return createdAt_;
}

ConversationTurn::ConversationTurn(
    QUuid turnId,
    QUuid requestId,
    QString question,
    QUuid providerProfileId,
    QString modelId,
    std::shared_ptr<const SnapshotRevision> snapshotRevision)
    : turnId_(std::move(turnId)),
      requestId_(std::move(requestId)),
      question_(std::move(question)),
      providerProfileId_(std::move(providerProfileId)),
      modelId_(std::move(modelId)),
      snapshotRevision_(std::move(snapshotRevision)) {}

const QUuid& ConversationTurn::turnId() const noexcept {
    return turnId_;
}

const QUuid& ConversationTurn::requestId() const noexcept {
    return requestId_;
}

const QString& ConversationTurn::question() const noexcept {
    return question_;
}

const QUuid& ConversationTurn::providerProfileId() const noexcept {
    return providerProfileId_;
}

const QString& ConversationTurn::modelId() const noexcept {
    return modelId_;
}

const std::shared_ptr<const SnapshotRevision>&
ConversationTurn::snapshotRevision() const noexcept {
    return snapshotRevision_;
}

ConversationTurn::Status ConversationTurn::status() const noexcept {
    return status_;
}

bool ConversationTurn::isTerminal() const noexcept {
    return status_ == Status::Completed || status_ == Status::Failed ||
           status_ == Status::Cancelled;
}

const QString& ConversationTurn::answer() const noexcept {
    return answer_;
}

const QString& ConversationTurn::error() const noexcept {
    return error_;
}

ai::ErrorKind ConversationTurn::errorKind() const noexcept {
    return errorKind_;
}

int ConversationTurn::httpStatus() const noexcept {
    return httpStatus_;
}

qint64 ConversationTurn::inputTokens() const noexcept {
    return inputTokens_;
}

qint64 ConversationTurn::outputTokens() const noexcept {
    return outputTokens_;
}

ConversationSession::ConversationSession(QUuid sessionId)
    : sessionId_(sessionId.isNull() ? QUuid::createUuid()
                                    : std::move(sessionId)) {}

const QUuid& ConversationSession::sessionId() const noexcept {
    return sessionId_;
}

const ConversationTurn* ConversationSession::beginExplicitSend(
    const RenderedSnapshot& renderedSnapshot,
    QString question,
    QUuid providerProfileId,
    QString modelId,
    QUuid requestId,
    QDateTime createdAt) {
    if (!renderedSnapshot.isValid() || question.trimmed().isEmpty() ||
        providerProfileId.isNull() || modelId.trimmed().isEmpty() ||
        (!requestId.isNull() && containsRequestId(requestId))) {
        return nullptr;
    }

    quint64 nextRevisionNumber = 1;
    if (!revisions_.isEmpty()) {
        const quint64 previous = revisions_.constLast()->revisionNumber();
        if (previous == std::numeric_limits<quint64>::max()) {
            return nullptr;
        }
        nextRevisionNumber = previous + 1;
    }

    RenderedSnapshot frozenSnapshot = renderedSnapshot;
    for (auto existing = revisions_.crbegin(); existing != revisions_.crend();
         ++existing) {
        const RenderedSnapshot& candidate = (*existing)->renderedSnapshot();
        if (candidate.sha256() != renderedSnapshot.sha256() ||
            candidate.pixelSize() != renderedSnapshot.pixelSize() ||
            candidate.pngBytes() != renderedSnapshot.pngBytes()) {
            continue;
        }

        // QByteArray copies are implicit-shared. The new revision therefore
        // owns the exact canonical PNG value without another encoded buffer;
        // its decoded image starts empty and remains lazily rebuildable.
        frozenSnapshot = RenderedSnapshot(
            candidate.pngBytes(), candidate.sha256(), candidate.pixelSize(),
            renderedSnapshot.revision());
        break;
    }

    const auto revision = std::shared_ptr<const SnapshotRevision>(
        new SnapshotRevision(QUuid::createUuid(), nextRevisionNumber,
                             std::move(frozenSnapshot),
                             normalizedCreatedAt(std::move(createdAt))));

    if (requestId.isNull()) {
        requestId = uniqueRequestId();
    }

    const ConversationTurn* turn = appendQueuedTurn(
        revision, std::move(question), std::move(providerProfileId),
        std::move(modelId), std::move(requestId));
    if (turn == nullptr) {
        return nullptr;
    }
    revisions_.append(revision);
    return turn;
}

const ConversationTurn* ConversationSession::retryOriginal(
    const QUuid& originalRequestId,
    QUuid newRequestId) {
    const ConversationTurn* original = turnForRequest(originalRequestId);
    if (original == nullptr || !original->isTerminal() ||
        (!newRequestId.isNull() && containsRequestId(newRequestId))) {
        return nullptr;
    }
    if (newRequestId.isNull()) {
        newRequestId = uniqueRequestId();
    }
    return appendQueuedTurn(original->snapshotRevision(), original->question(),
                            original->providerProfileId(), original->modelId(),
                            std::move(newRequestId));
}

const ConversationTurn* ConversationSession::retryWithLatestCurrent(
    const QUuid& originalRequestId,
    const RenderedSnapshot& latestSnapshot,
    QUuid newRequestId,
    QDateTime createdAt) {
    const ConversationTurn* original = turnForRequest(originalRequestId);
    if (original == nullptr || !original->isTerminal()) {
        return nullptr;
    }
    return beginExplicitSend(latestSnapshot, original->question(),
                             original->providerProfileId(), original->modelId(),
                             std::move(newRequestId), std::move(createdAt));
}

bool ConversationSession::acceptEvent(const ai::AiStreamEvent& event) {
    ConversationTurn* turn = mutableTurnForRequest(event.requestId);
    if (turn == nullptr || turn->isTerminal()) {
        return false;
    }

    switch (event.type) {
    case ai::EventType::Started:
        if (turn->status_ != ConversationTurn::Status::Queued) {
            return false;
        }
        turn->status_ = ConversationTurn::Status::Streaming;
        return true;

    case ai::EventType::TextDelta:
        if (turn->status_ != ConversationTurn::Status::Queued &&
            turn->status_ != ConversationTurn::Status::Streaming) {
            return false;
        }
        turn->answer_.append(event.text);
        turn->status_ = ConversationTurn::Status::Streaming;
        return true;

    case ai::EventType::UsageUpdated:
        if (turn->status_ != ConversationTurn::Status::Queued &&
            turn->status_ != ConversationTurn::Status::Streaming) {
            return false;
        }
        if (event.inputTokens >= 0) {
            turn->inputTokens_ = event.inputTokens;
        }
        if (event.outputTokens >= 0) {
            turn->outputTokens_ = event.outputTokens;
        }
        return true;

    case ai::EventType::Completed:
        turn->status_ = ConversationTurn::Status::Completed;
        return true;

    case ai::EventType::Cancelled:
        turn->status_ = ConversationTurn::Status::Cancelled;
        turn->error_ = event.errorMessage;
        turn->errorKind_ = event.errorKind == ai::ErrorKind::None
                               ? ai::ErrorKind::Cancelled
                               : event.errorKind;
        turn->httpStatus_ = event.httpStatus;
        return true;

    case ai::EventType::Failed:
        turn->status_ = ConversationTurn::Status::Failed;
        turn->error_ = event.errorMessage;
        turn->errorKind_ = event.errorKind;
        turn->httpStatus_ = event.httpStatus;
        return true;
    }

    return false;
}

qsizetype ConversationSession::revisionCount() const noexcept {
    return revisions_.size();
}

qsizetype ConversationSession::turnCount() const noexcept {
    return turns_.size();
}

const SnapshotRevision* ConversationSession::revisionAt(
    qsizetype index) const noexcept {
    if (index < 0 || index >= revisions_.size()) {
        return nullptr;
    }
    return revisions_.at(index).get();
}

const SnapshotRevision* ConversationSession::latestRevision() const noexcept {
    return revisions_.isEmpty() ? nullptr : revisions_.constLast().get();
}

const ConversationTurn* ConversationSession::turnAt(qsizetype index) const
    noexcept {
    if (index < 0 || index >= turns_.size()) {
        return nullptr;
    }
    return turns_.at(index).get();
}

const ConversationTurn* ConversationSession::turnForRequest(
    const QUuid& requestId) const noexcept {
    for (const auto& turn : turns_) {
        if (turn->requestId() == requestId) {
            return turn.get();
        }
    }
    return nullptr;
}

QList<const SnapshotRevision*> ConversationSession::revisions() const {
    QList<const SnapshotRevision*> result;
    result.reserve(revisions_.size());
    for (const auto& revision : revisions_) {
        result.append(revision.get());
    }
    return result;
}

QList<const ConversationTurn*> ConversationSession::turns() const {
    QList<const ConversationTurn*> result;
    result.reserve(turns_.size());
    for (const auto& turn : turns_) {
        result.append(turn.get());
    }
    return result;
}

SnapshotCacheCompactionResult ConversationSession::compactOldSnapshotImages(
    const QSet<QByteArray>& retainedHashes) {
    SnapshotCacheCompactionResult result;
    if (revisions_.size() <= 1) {
        return result;
    }

    const qsizetype oldRevisionCount = revisions_.size() - 1;
    result.scannedOldRevisionCount = oldRevisionCount;
    for (qsizetype index = 0; index < oldRevisionCount; ++index) {
        const SnapshotRevision& revision = *revisions_.at(index);
        if (retainedHashes.contains(revision.renderedHash())) {
            continue;
        }

        const RenderedSnapshot& snapshot = revision.renderedSnapshot();
        const quint64 decodedBytes = snapshot.releaseDecodedImage();
        if (decodedBytes == 0) {
            continue;
        }

        ++result.releasedDecodedImageCount;
        if (decodedBytes > std::numeric_limits<quint64>::max() -
                               result.releasedDecodedImageBytes) {
            result.releasedDecodedImageBytes =
                std::numeric_limits<quint64>::max();
        } else {
            result.releasedDecodedImageBytes += decodedBytes;
        }
    }
    return result;
}

ConversationTurn* ConversationSession::mutableTurnForRequest(
    const QUuid& requestId) noexcept {
    for (const auto& turn : turns_) {
        if (turn->requestId() == requestId) {
            return turn.get();
        }
    }
    return nullptr;
}

bool ConversationSession::containsRequestId(const QUuid& requestId) const
    noexcept {
    return turnForRequest(requestId) != nullptr;
}

QUuid ConversationSession::uniqueRequestId() const {
    QUuid result;
    do {
        result = QUuid::createUuid();
    } while (result.isNull() || containsRequestId(result));
    return result;
}

const ConversationTurn* ConversationSession::appendQueuedTurn(
    std::shared_ptr<const SnapshotRevision> revision,
    QString question,
    QUuid providerProfileId,
    QString modelId,
    QUuid requestId) {
    if (revision == nullptr || !revision->renderedSnapshot().isValid() ||
        question.trimmed().isEmpty() || providerProfileId.isNull() ||
        modelId.trimmed().isEmpty() || requestId.isNull() ||
        containsRequestId(requestId)) {
        return nullptr;
    }

    auto turn = std::shared_ptr<ConversationTurn>(new ConversationTurn(
        QUuid::createUuid(), std::move(requestId), std::move(question),
        std::move(providerProfileId), std::move(modelId),
        std::move(revision)));
    turn->status_ = ConversationTurn::Status::Queued;
    const ConversationTurn* result = turn.get();
    turns_.append(std::move(turn));
    return result;
}

QDateTime ConversationSession::normalizedCreatedAt(QDateTime createdAt) {
    if (!createdAt.isValid()) {
        return QDateTime::currentDateTimeUtc();
    }
    return createdAt.toUTC();
}

}  // namespace snapask
