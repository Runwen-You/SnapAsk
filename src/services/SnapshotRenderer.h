#pragma once

#include "domain/annotation/Annotation.h"

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QRect>
#include <QSize>
#include <QUuid>
#include <QVector>

namespace snapask {

class ScreenshotSession;
class ConversationSession;

// A value-only, immutable-at-use rendering input. It is captured on the UI
// thread and is safe to move to QThreadPool; QPixmap and live document objects
// never cross the thread boundary.
struct SnapshotRenderInput final {
    QUuid sessionId;
    QImage sourceImage;
    QRect cropRect;
    QVector<Annotation> annotations;
    quint64 revision{0};

    [[nodiscard]] bool isValid() const noexcept;
};

class RenderedSnapshot final {
public:
    RenderedSnapshot() = default;
    RenderedSnapshot(const RenderedSnapshot& other);
    RenderedSnapshot(RenderedSnapshot&& other) noexcept;
    RenderedSnapshot& operator=(const RenderedSnapshot& other);
    RenderedSnapshot& operator=(RenderedSnapshot&& other) noexcept;
    ~RenderedSnapshot() = default;

    [[nodiscard]] bool isValid() const noexcept;

    // The decoded image is a rebuildable cache. Returning a value keeps the
    // pixels alive for the caller even if another operation releases this
    // snapshot's cache immediately afterwards. A missing cache is decoded only
    // from pngBytes; the screenshot and annotations are never rendered again.
    [[nodiscard]] QImage image() const;
    [[nodiscard]] bool hasDecodedImage() const noexcept;
    [[nodiscard]] quint64 decodedImageByteSize() const noexcept;
    // Returns the logical decoded byte size whose cache reference was dropped.
    // Live QImage values previously returned by image() may continue to share
    // those pixels until their own lifetimes end.
    quint64 releaseDecodedImage() const noexcept;

    [[nodiscard]] const QByteArray& pngBytes() const noexcept;
    [[nodiscard]] const QByteArray& sha256() const noexcept;
    [[nodiscard]] QSize pixelSize() const noexcept;
    [[nodiscard]] quint64 revision() const noexcept;

private:
    friend class SnapshotRenderer;
    friend class ConversationSession;

    RenderedSnapshot(QImage image,
                     QByteArray pngBytes,
                     QByteArray sha256,
                     quint64 revision);
    RenderedSnapshot(QByteArray pngBytes,
                     QByteArray sha256,
                     QSize pixelSize,
                     quint64 revision);

    // Cache-only mutation is synchronized. Metadata is immutable during normal
    // use; as with other Qt value types, assigning to the same instance from a
    // second thread still requires external synchronization.
    mutable QMutex decodedImageMutex_;
    mutable QImage image_;
    QByteArray pngBytes_;
    QByteArray sha256_;
    QSize pixelSize_;
    quint64 revision_{0};
};

struct SnapshotRenderResult final {
    QUuid sessionId;
    quint64 revision{0};
    RenderedSnapshot snapshot;

    [[nodiscard]] bool isValid() const noexcept
    {
        return !sessionId.isNull() && snapshot.isValid();
    }
};

class SnapshotRenderer final {
public:
    // Freeze live session state before dispatching work to a thread pool.
    [[nodiscard]] static SnapshotRenderInput freezeCurrent(
        const ScreenshotSession& session);

    // Performs crop, annotation rasterization, lossless PNG encoding and
    // SHA-256 using only the frozen value object.
    [[nodiscard]] static SnapshotRenderResult renderFrozen(
        SnapshotRenderInput input);

    // Render once at an export/send boundary, then share the returned value with
    // save, clipboard, pin and AI consumers. No consumer should redraw the session.
    [[nodiscard]] static RenderedSnapshot renderCurrent(
        const ScreenshotSession& session);
};

}  // namespace snapask
