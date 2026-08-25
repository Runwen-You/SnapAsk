#pragma once

#include "domain/annotation/AnnotationDocument.h"

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QUndoStack>
#include <QUuid>

namespace snapask {

class ScreenshotSession final {
public:
    ScreenshotSession();
    explicit ScreenshotSession(QImage sourceImage,
                               QUuid sessionId = QUuid::createUuid());

    ScreenshotSession(const ScreenshotSession&) = delete;
    ScreenshotSession& operator=(const ScreenshotSession&) = delete;

    [[nodiscard]] const QUuid& sessionId() const noexcept;
    [[nodiscard]] const QImage& sourceImage() const noexcept;
    [[nodiscard]] bool hasSourceImage() const noexcept;
    bool setSourceImage(QImage sourceImage);

    [[nodiscard]] QRect cropRect() const noexcept;
    // Crop rectangles use source-image physical pixels, never Qt logical pixels.
    bool setCropRect(const QRect& sourcePixelRect);
    void restoreOriginalCrop();

    [[nodiscard]] AnnotationDocument& annotations() noexcept;
    [[nodiscard]] const AnnotationDocument& annotations() const noexcept;
    [[nodiscard]] QUndoStack& undoStack() noexcept;
    [[nodiscard]] const QUndoStack& undoStack() const noexcept;

    [[nodiscard]] quint64 currentRevision() const noexcept;

    [[nodiscard]] const QByteArray& lastSavedHash() const noexcept;
    [[nodiscard]] const QByteArray& lastSentHash() const noexcept;
    void markSavedHash(QByteArray sha256);
    void markSentHash(QByteArray sha256);
    void clearSavedHash();
    void clearSentHash();
    [[nodiscard]] bool hasUnsavedChanges(
        const QByteArray& currentSnapshotHash) const;
    [[nodiscard]] bool hasUnsentChanges(
        const QByteArray& currentSnapshotHash) const;

private:
    static QImage normalizeSourceImage(QImage image);
    void recordSourceMutation();

    QUuid sessionId_;
    QImage sourceImage_;
    QRect cropRect_;
    AnnotationDocument annotations_;
    QUndoStack undoStack_;
    quint64 sourceRevision_{0};
    QByteArray lastSavedHash_;
    QByteArray lastSentHash_;
};

}  // namespace snapask
