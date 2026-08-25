#include "services/SessionMemoryBudget.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace snapask {

bool MemoryBudgetResult::accepted() const noexcept
{
    return status == MemoryBudgetStatus::Accepted
        || status == MemoryBudgetStatus::AcceptedAfterCacheEviction;
}

SessionMemoryBudget::SessionMemoryBudget(quint64 hardLimitBytes) noexcept
    : hardLimitBytes_(hardLimitBytes)
{
}

MemoryBudgetResult SessionMemoryBudget::setHardLimitBytes(quint64 hardLimitBytes)
{
    if (hardLimitBytes == hardLimitBytes_) {
        return makeResult(MemoryBudgetStatus::Accepted, 0, usedBytes_);
    }

    quint64 proposedUsage = usedBytes_;
    QStringList evictedKeys;
    if (proposedUsage > hardLimitBytes) {
        const QStringList candidates = leastRecentlyUsedCacheKeys();
        for (const QString& key : candidates) {
            const auto entry = rebuildableCaches_.constFind(key);
            if (entry == rebuildableCaches_.cend()) {
                continue;
            }
            proposedUsage -= entry->byteSize;
            evictedKeys.append(key);
            if (proposedUsage <= hardLimitBytes) {
                break;
            }
        }
    }

    if (proposedUsage > hardLimitBytes) {
        MemoryBudgetResult result = makeResult(
            MemoryBudgetStatus::RejectedUnreclaimableFootprint,
            0,
            proposedUsage);
        result.hardLimitBytes = hardLimitBytes;
        return result;
    }

    for (const QString& key : std::as_const(evictedKeys)) {
        rebuildableCaches_.remove(key);
    }
    hardLimitBytes_ = hardLimitBytes;
    usedBytes_ = proposedUsage;
    const MemoryBudgetStatus status = evictedKeys.isEmpty()
        ? MemoryBudgetStatus::Accepted
        : MemoryBudgetStatus::AcceptedAfterCacheEviction;
    return makeResult(
        status,
        0,
        usedBytes_,
        std::move(evictedKeys));
}

MemoryBudgetResult SessionMemoryBudget::setOriginalImageBytes(quint64 byteSize)
{
    return upsertEntry(EntryKind::OriginalImage, {}, {}, byteSize);
}

MemoryBudgetResult SessionMemoryBudget::upsertSentSnapshotPng(
    QByteArray snapshotHash,
    quint64 byteSize)
{
    if (snapshotHash.isEmpty()) {
        return makeResult(MemoryBudgetStatus::RejectedEmptyIdentifier, byteSize);
    }
    return upsertEntry(
        EntryKind::SentSnapshotPng,
        snapshotHash,
        {},
        byteSize);
}

MemoryBudgetResult SessionMemoryBudget::upsertRebuildableCache(
    QString cacheKey,
    quint64 byteSize)
{
    if (cacheKey.isEmpty()) {
        return makeResult(MemoryBudgetStatus::RejectedEmptyIdentifier, byteSize);
    }
    return upsertEntry(
        EntryKind::RebuildableCache,
        {},
        cacheKey,
        byteSize);
}

void SessionMemoryBudget::clearOriginalImage() noexcept
{
    usedBytes_ -= originalImageBytes_;
    originalImageBytes_ = 0;
}

bool SessionMemoryBudget::removeSentSnapshotPng(const QByteArray& snapshotHash) noexcept
{
    const auto entry = sentSnapshots_.find(snapshotHash);
    if (entry == sentSnapshots_.end()) {
        return false;
    }
    usedBytes_ -= entry.value();
    sentSnapshots_.erase(entry);
    return true;
}

bool SessionMemoryBudget::removeRebuildableCache(const QString& cacheKey) noexcept
{
    const auto entry = rebuildableCaches_.find(cacheKey);
    if (entry == rebuildableCaches_.end()) {
        return false;
    }
    usedBytes_ -= entry->byteSize;
    rebuildableCaches_.erase(entry);
    return true;
}

bool SessionMemoryBudget::touchRebuildableCache(const QString& cacheKey) noexcept
{
    const auto entry = rebuildableCaches_.find(cacheKey);
    if (entry == rebuildableCaches_.end()) {
        return false;
    }
    entry->lastUseSequence = nextUseSequence();
    return true;
}

quint64 SessionMemoryBudget::clearRebuildableCaches() noexcept
{
    const quint64 releasedBytes = reclaimableBytes();
    usedBytes_ -= releasedBytes;
    rebuildableCaches_.clear();
    nextUseSequence_ = 0;
    return releasedBytes;
}

void SessionMemoryBudget::clear() noexcept
{
    originalImageBytes_ = 0;
    usedBytes_ = 0;
    nextUseSequence_ = 0;
    sentSnapshots_.clear();
    rebuildableCaches_.clear();
}

quint64 SessionMemoryBudget::hardLimitBytes() const noexcept
{
    return hardLimitBytes_;
}

quint64 SessionMemoryBudget::usedBytes() const noexcept
{
    return usedBytes_;
}

quint64 SessionMemoryBudget::unreclaimableBytes() const noexcept
{
    return usedBytes_ - reclaimableBytes();
}

quint64 SessionMemoryBudget::reclaimableBytes() const noexcept
{
    quint64 total = 0;
    for (const CacheEntry& entry : rebuildableCaches_) {
        total += entry.byteSize;
    }
    return total;
}

quint64 SessionMemoryBudget::originalImageBytes() const noexcept
{
    return originalImageBytes_;
}

qsizetype SessionMemoryBudget::sentSnapshotCount() const noexcept
{
    return sentSnapshots_.size();
}

qsizetype SessionMemoryBudget::rebuildableCacheCount() const noexcept
{
    return rebuildableCaches_.size();
}

bool SessionMemoryBudget::containsSentSnapshot(const QByteArray& snapshotHash) const noexcept
{
    return sentSnapshots_.contains(snapshotHash);
}

bool SessionMemoryBudget::containsRebuildableCache(const QString& cacheKey) const noexcept
{
    return rebuildableCaches_.contains(cacheKey);
}

std::optional<quint64> SessionMemoryBudget::checkedImageByteSize(
    qint64 bytesPerLine,
    qint64 height) noexcept
{
    if (bytesPerLine < 0 || height < 0) {
        return std::nullopt;
    }
    const quint64 unsignedStride = static_cast<quint64>(bytesPerLine);
    const quint64 unsignedHeight = static_cast<quint64>(height);
    if (unsignedStride != 0
        && unsignedHeight > std::numeric_limits<quint64>::max() / unsignedStride) {
        return std::nullopt;
    }
    return unsignedStride * unsignedHeight;
}

MemoryBudgetResult SessionMemoryBudget::upsertEntry(
    EntryKind kind,
    const QByteArray& binaryKey,
    const QString& textKey,
    quint64 byteSize)
{
    if (byteSize == 0) {
        return makeResult(MemoryBudgetStatus::RejectedZeroSize, byteSize);
    }

    quint64 previousSize = 0;
    switch (kind) {
    case EntryKind::OriginalImage:
        previousSize = originalImageBytes_;
        break;
    case EntryKind::SentSnapshotPng:
        previousSize = sentSnapshots_.value(binaryKey, 0);
        break;
    case EntryKind::RebuildableCache:
        previousSize = rebuildableCaches_.value(textKey).byteSize;
        break;
    }

    const quint64 baseUsage = usedBytes_ - previousSize;
    const std::optional<quint64> initialUsage = checkedAdd(baseUsage, byteSize);
    if (!initialUsage.has_value()) {
        return makeResult(
            MemoryBudgetStatus::RejectedArithmeticOverflow,
            byteSize,
            std::numeric_limits<quint64>::max());
    }

    quint64 proposedUsage = *initialUsage;
    QStringList evictedKeys;
    if (proposedUsage > hardLimitBytes_) {
        const QString excludedKey = kind == EntryKind::RebuildableCache ? textKey : QString{};
        const QStringList candidates = leastRecentlyUsedCacheKeys(excludedKey);
        for (const QString& key : candidates) {
            const auto entry = rebuildableCaches_.constFind(key);
            if (entry == rebuildableCaches_.cend()) {
                continue;
            }
            proposedUsage -= entry->byteSize;
            evictedKeys.append(key);
            if (proposedUsage <= hardLimitBytes_) {
                break;
            }
        }
    }

    if (proposedUsage > hardLimitBytes_) {
        const MemoryBudgetStatus rejection = kind == EntryKind::RebuildableCache
            ? MemoryBudgetStatus::RejectedHardLimit
            : MemoryBudgetStatus::RejectedUnreclaimableFootprint;
        return makeResult(rejection, byteSize, proposedUsage);
    }

    for (const QString& key : std::as_const(evictedKeys)) {
        rebuildableCaches_.remove(key);
    }
    switch (kind) {
    case EntryKind::OriginalImage:
        originalImageBytes_ = byteSize;
        break;
    case EntryKind::SentSnapshotPng:
        sentSnapshots_.insert(binaryKey, byteSize);
        break;
    case EntryKind::RebuildableCache:
        rebuildableCaches_.insert(textKey, CacheEntry{byteSize, nextUseSequence()});
        break;
    }
    usedBytes_ = proposedUsage;

    const MemoryBudgetStatus status = evictedKeys.isEmpty()
        ? MemoryBudgetStatus::Accepted
        : MemoryBudgetStatus::AcceptedAfterCacheEviction;
    return makeResult(
        status,
        byteSize,
        usedBytes_,
        std::move(evictedKeys));
}

MemoryBudgetResult SessionMemoryBudget::makeResult(
    MemoryBudgetStatus status,
    quint64 requestedBytes,
    quint64 minimumRequiredBytes,
    QStringList evictedCacheKeys) const
{
    return MemoryBudgetResult{
        status,
        requestedBytes,
        usedBytes_,
        hardLimitBytes_,
        minimumRequiredBytes,
        std::move(evictedCacheKeys),
    };
}

QStringList SessionMemoryBudget::leastRecentlyUsedCacheKeys(
    const QString& excludedKey) const
{
    struct Candidate final {
        QString key;
        quint64 lastUseSequence{0};
    };

    QVector<Candidate> candidates;
    candidates.reserve(rebuildableCaches_.size());
    for (auto entry = rebuildableCaches_.cbegin(); entry != rebuildableCaches_.cend(); ++entry) {
        if (!excludedKey.isEmpty() && entry.key() == excludedKey) {
            continue;
        }
        candidates.append(Candidate{entry.key(), entry->lastUseSequence});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.lastUseSequence != right.lastUseSequence) {
            return left.lastUseSequence < right.lastUseSequence;
        }
        return left.key < right.key;
    });

    QStringList keys;
    keys.reserve(candidates.size());
    for (const Candidate& candidate : std::as_const(candidates)) {
        keys.append(candidate.key);
    }
    return keys;
}

quint64 SessionMemoryBudget::nextUseSequence() noexcept
{
    if (nextUseSequence_ == std::numeric_limits<quint64>::max()) {
        struct RankedCache final {
            QString key;
            quint64 lastUseSequence{0};
        };
        QVector<RankedCache> ranked;
        ranked.reserve(rebuildableCaches_.size());
        for (auto entry = rebuildableCaches_.cbegin(); entry != rebuildableCaches_.cend(); ++entry) {
            ranked.append(RankedCache{entry.key(), entry->lastUseSequence});
        }
        std::sort(ranked.begin(), ranked.end(), [](const RankedCache& left, const RankedCache& right) {
            if (left.lastUseSequence != right.lastUseSequence) {
                return left.lastUseSequence < right.lastUseSequence;
            }
            return left.key < right.key;
        });
        quint64 compactSequence = 0;
        for (const RankedCache& cache : std::as_const(ranked)) {
            auto entry = rebuildableCaches_.find(cache.key);
            if (entry != rebuildableCaches_.end()) {
                entry->lastUseSequence = compactSequence;
                ++compactSequence;
            }
        }
        nextUseSequence_ = compactSequence;
    }

    const quint64 sequence = nextUseSequence_;
    ++nextUseSequence_;
    return sequence;
}

std::optional<quint64> SessionMemoryBudget::checkedAdd(
    quint64 left,
    quint64 right) noexcept
{
    if (right > std::numeric_limits<quint64>::max() - left) {
        return std::nullopt;
    }
    return left + right;
}

}  // namespace snapask
