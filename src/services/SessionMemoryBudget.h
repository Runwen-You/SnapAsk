#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace snapask {

enum class MemoryBudgetStatus : quint8 {
    Accepted = 0,
    AcceptedAfterCacheEviction,
    RejectedEmptyIdentifier,
    RejectedZeroSize,
    RejectedArithmeticOverflow,
    RejectedHardLimit,
    RejectedUnreclaimableFootprint,
};

struct MemoryBudgetResult final {
    MemoryBudgetStatus status{MemoryBudgetStatus::Accepted};
    quint64 requestedBytes{0};
    quint64 usedBytesAfter{0};
    quint64 hardLimitBytes{0};
    quint64 minimumRequiredBytes{0};
    QStringList evictedCacheKeys;

    [[nodiscard]] bool accepted() const noexcept;
};

// A metadata-only budget ledger. It never owns, copies or serializes image data:
// callers release the cache objects named in evictedCacheKeys after a successful
// operation. Original and sent PNG records are non-reclaimable; rebuildable
// caches are reclaimed in least-recently-used order.
class SessionMemoryBudget final {
public:
    // Leaves working-set headroom for Qt/UI/network state under the 180 MB
    // single-4K active-session target in the product specification.
    static constexpr quint64 defaultHardLimitBytes = 128ULL * 1024ULL * 1024ULL;

    explicit SessionMemoryBudget(
        quint64 hardLimitBytes = defaultHardLimitBytes) noexcept;

    [[nodiscard]] MemoryBudgetResult setHardLimitBytes(quint64 hardLimitBytes);
    [[nodiscard]] MemoryBudgetResult setOriginalImageBytes(quint64 byteSize);
    [[nodiscard]] MemoryBudgetResult upsertSentSnapshotPng(
        QByteArray snapshotHash,
        quint64 byteSize);
    [[nodiscard]] MemoryBudgetResult upsertRebuildableCache(
        QString cacheKey,
        quint64 byteSize);

    void clearOriginalImage() noexcept;
    [[nodiscard]] bool removeSentSnapshotPng(const QByteArray& snapshotHash) noexcept;
    [[nodiscard]] bool removeRebuildableCache(const QString& cacheKey) noexcept;
    [[nodiscard]] bool touchRebuildableCache(const QString& cacheKey) noexcept;
    [[nodiscard]] quint64 clearRebuildableCaches() noexcept;
    void clear() noexcept;

    [[nodiscard]] quint64 hardLimitBytes() const noexcept;
    [[nodiscard]] quint64 usedBytes() const noexcept;
    [[nodiscard]] quint64 unreclaimableBytes() const noexcept;
    [[nodiscard]] quint64 reclaimableBytes() const noexcept;
    [[nodiscard]] quint64 originalImageBytes() const noexcept;
    [[nodiscard]] qsizetype sentSnapshotCount() const noexcept;
    [[nodiscard]] qsizetype rebuildableCacheCount() const noexcept;
    [[nodiscard]] bool containsSentSnapshot(const QByteArray& snapshotHash) const noexcept;
    [[nodiscard]] bool containsRebuildableCache(const QString& cacheKey) const noexcept;

    // Computes bytesPerLine * height without signed conversion or multiplication
    // overflow. Zero-sized images are represented by a valid zero result.
    [[nodiscard]] static std::optional<quint64> checkedImageByteSize(
        qint64 bytesPerLine,
        qint64 height) noexcept;

private:
    enum class EntryKind : quint8 {
        OriginalImage,
        SentSnapshotPng,
        RebuildableCache,
    };

    struct CacheEntry final {
        quint64 byteSize{0};
        quint64 lastUseSequence{0};
    };

    [[nodiscard]] MemoryBudgetResult upsertEntry(
        EntryKind kind,
        const QByteArray& binaryKey,
        const QString& textKey,
        quint64 byteSize);
    [[nodiscard]] MemoryBudgetResult makeResult(
        MemoryBudgetStatus status,
        quint64 requestedBytes,
        quint64 minimumRequiredBytes = 0,
        QStringList evictedCacheKeys = {}) const;
    [[nodiscard]] QStringList leastRecentlyUsedCacheKeys(
        const QString& excludedKey = {}) const;
    [[nodiscard]] quint64 nextUseSequence() noexcept;
    [[nodiscard]] static std::optional<quint64> checkedAdd(
        quint64 left,
        quint64 right) noexcept;

    quint64 hardLimitBytes_{defaultHardLimitBytes};
    quint64 usedBytes_{0};
    quint64 originalImageBytes_{0};
    quint64 nextUseSequence_{0};
    QHash<QByteArray, quint64> sentSnapshots_;
    QHash<QString, CacheEntry> rebuildableCaches_;
};

}  // namespace snapask
