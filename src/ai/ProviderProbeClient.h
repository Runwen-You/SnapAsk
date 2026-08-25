#pragma once

#include "ai/AiTypes.h"

#include <QHash>
#include <QObject>

#include <memory>

class QNetworkReply;

namespace snapask::ai {

// Asynchronous, non-user-data provider diagnostics. The only image probe uses
// a provider-owned fixed PNG; this public surface deliberately has no image,
// screenshot, snapshot, question, or arbitrary-payload parameter.
class ProviderProbeClient final : public QObject {
    Q_OBJECT

public:
    explicit ProviderProbeClient(QObject* parent = nullptr);
    ~ProviderProbeClient() override;

    ProviderProbeClient(const ProviderProbeClient&) = delete;
    ProviderProbeClient& operator=(const ProviderProbeClient&) = delete;

    [[nodiscard]] QUuid fetchModels(
        const ProviderProfile& profile,
        QString&& apiKey);
    [[nodiscard]] QUuid testTextConnection(
        const ProviderProfile& profile,
        QString&& apiKey);
    [[nodiscard]] QUuid testImageUnderstanding(
        const ProviderProfile& profile,
        QString&& apiKey);

    void cancel(const QUuid& operationId);
    [[nodiscard]] bool isActive(const QUuid& operationId) const;

signals:
    void resultReady(const snapask::ai::ProviderProbeResult& result);

private:
    struct ActiveProbe;

    [[nodiscard]] QUuid start(
        ProviderProbeOperation operation,
        const ProviderProfile& profile,
        QString&& apiKey);
    void dispatch(
        const std::shared_ptr<ActiveProbe>& active,
        const QUrl& endpoint);
    void handleMetadata(
        const QUuid& operationId,
        QNetworkReply* reply);
    void consume(
        const QUuid& operationId,
        QNetworkReply* reply);
    void finish(
        const QUuid& operationId,
        QNetworkReply* reply);
    void succeed(
        const std::shared_ptr<ActiveProbe>& active,
        QStringList modelIds = {});
    void fail(
        const std::shared_ptr<ActiveProbe>& active,
        ErrorKind kind,
        int httpStatus = 0);
    void emitImmediateFailure(
        const QUuid& operationId,
        const QUuid& providerProfileId,
        ProviderProbeOperation operation,
        ErrorKind kind);

    QHash<QUuid, std::shared_ptr<ActiveProbe>> active_;
};

}  // namespace snapask::ai
