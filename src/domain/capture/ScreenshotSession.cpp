#include "domain/capture/ScreenshotSession.h"

#include <limits>
#include <utility>

namespace snapask {

ScreenshotSession::ScreenshotSession() : sessionId_(QUuid::createUuid()) {}

ScreenshotSession::ScreenshotSession(QImage sourceImage, QUuid sessionId)
    : sessionId_(sessionId.isNull() ? QUuid::createUuid() : sessionId) {
    setSourceImage(std::move(sourceImage));
}

const QUuid& ScreenshotSession::sessionId() const noexcept {
    return sessionId_;
}

const QImage& ScreenshotSession::sourceImage() const noexcept {
    return sourceImage_;
}

bool ScreenshotSession::hasSourceImage() const noexcept {
    return !sourceImage_.isNull();
}

bool ScreenshotSession::setSourceImage(QImage sourceImage) {
    sourceImage = normalizeSourceImage(std::move(sourceImage));
    if (sourceImage.isNull()) {
        return false;
    }

    sourceImage_ = std::move(sourceImage);
    sourceImage_.detach();
    cropRect_ = sourceImage_.rect();
    undoStack_.clear();
    (void)annotations_.clearAnnotations();
    lastSavedHash_.clear();
    lastSentHash_.clear();
    recordSourceMutation();
    return true;
}

QRect ScreenshotSession::cropRect() const noexcept {
    return cropRect_;
}

bool ScreenshotSession::setCropRect(const QRect& sourcePixelRect) {
    if (sourceImage_.isNull()) {
        return false;
    }
    const QRect bounded =
        sourcePixelRect.normalized().intersected(sourceImage_.rect());
    if (bounded.isEmpty() || bounded == cropRect_) {
        return false;
    }
    cropRect_ = bounded;
    recordSourceMutation();
    return true;
}

void ScreenshotSession::restoreOriginalCrop() {
    if (!sourceImage_.isNull() && cropRect_ != sourceImage_.rect()) {
        cropRect_ = sourceImage_.rect();
        recordSourceMutation();
    }
}

AnnotationDocument& ScreenshotSession::annotations() noexcept {
    return annotations_;
}

const AnnotationDocument& ScreenshotSession::annotations() const noexcept {
    return annotations_;
}

QUndoStack& ScreenshotSession::undoStack() noexcept {
    return undoStack_;
}

const QUndoStack& ScreenshotSession::undoStack() const noexcept {
    return undoStack_;
}

quint64 ScreenshotSession::currentRevision() const noexcept {
    const quint64 annotationRevision = annotations_.revision();
    const quint64 maximum = std::numeric_limits<quint64>::max();
    if (maximum - sourceRevision_ < annotationRevision) {
        return maximum;
    }
    return sourceRevision_ + annotationRevision;
}

const QByteArray& ScreenshotSession::lastSavedHash() const noexcept {
    return lastSavedHash_;
}

const QByteArray& ScreenshotSession::lastSentHash() const noexcept {
    return lastSentHash_;
}

void ScreenshotSession::markSavedHash(QByteArray sha256) {
    lastSavedHash_ = std::move(sha256);
}

void ScreenshotSession::markSentHash(QByteArray sha256) {
    lastSentHash_ = std::move(sha256);
}

void ScreenshotSession::clearSavedHash() {
    lastSavedHash_.clear();
}

void ScreenshotSession::clearSentHash() {
    lastSentHash_.clear();
}

bool ScreenshotSession::hasUnsavedChanges(
    const QByteArray& currentSnapshotHash) const {
    return lastSavedHash_.isEmpty() || lastSavedHash_ != currentSnapshotHash;
}

bool ScreenshotSession::hasUnsentChanges(
    const QByteArray& currentSnapshotHash) const {
    return lastSentHash_.isEmpty() || lastSentHash_ != currentSnapshotHash;
}

QImage ScreenshotSession::normalizeSourceImage(QImage image) {
    if (image.isNull()) {
        return {};
    }
    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    image.setDevicePixelRatio(1.0);
    return image;
}

void ScreenshotSession::recordSourceMutation() {
    if (sourceRevision_ != std::numeric_limits<quint64>::max()) {
        ++sourceRevision_;
    }
}

}  // namespace snapask
