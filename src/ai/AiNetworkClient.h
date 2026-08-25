#pragma once

#include "ai/AiTypes.h"

#include <QHash>
#include <QObject>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace snapask {
class RenderedSnapshot;
}

namespace snapask::ai {

class ILlmProvider;
class SseDecoder;

class AiNetworkClient final : public QObject {
    Q_OBJECT

public:
    explicit AiNetworkClient(QObject* parent = nullptr);
    ~AiNetworkClient() override;

    // This is the only screenshot-uploading entry point. UI calls it only in direct
    // response to the Send button or Ctrl+Enter.
    QUuid sendExplicit(
        AiRequest request,
        const snapask::RenderedSnapshot& snapshot,
        const ProviderProfile& profile,
        QString&& apiKey);
    void cancel(const QUuid& requestId);
    void cancelSession(const QUuid& sessionId);
    [[nodiscard]] bool isActive(const QUuid& requestId) const;

signals:
    void eventReady(const snapask::ai::AiStreamEvent& event);

private:
    struct ActiveRequest;
    void consume(const QUuid& requestId, const QByteArray& bytes);
    bool validateResponseMetadata(
        const std::shared_ptr<ActiveRequest>& active,
        bool finalCheck);
    void handleMappedEvents(
        const std::shared_ptr<ActiveRequest>& active,
        const QList<AiStreamEvent>& events);
    void startNetwork(
        const std::shared_ptr<ActiveRequest>& active,
        QByteArray payload);
    void finish(const QUuid& requestId);
    void fail(
        const std::shared_ptr<ActiveRequest>& active,
        ErrorKind kind,
        const QString& message,
        int httpStatus = 0);
    void discard(const QUuid& requestId);

    QHash<QUuid, std::shared_ptr<ActiveRequest>> active_;
};

} // namespace snapask::ai
