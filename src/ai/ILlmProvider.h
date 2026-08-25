#pragma once

#include "ai/AiTypes.h"
#include "ai/SseDecoder.h"

#include <QList>
#include <QUrl>
#include <memory>

namespace snapask::ai {

class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;

    [[nodiscard]] virtual Protocol protocol() const = 0;
    [[nodiscard]] virtual QUrl responseEndpoint(const ProviderProfile& profile) const = 0;
    [[nodiscard]] virtual QUrl modelsEndpoint(const ProviderProfile& profile) const = 0;
    [[nodiscard]] virtual QByteArray buildStreamingPayload(
        const ProviderProfile& profile,
        const AiRequest& request,
        QString* error) const = 0;
    [[nodiscard]] virtual QList<AiStreamEvent> mapEvent(
        const SseEvent& event,
        const QUuid& requestId) const = 0;

    // Probe payloads are wholly provider-owned and contain only fixed,
    // non-sensitive diagnostics. In particular, callers cannot inject a
    // screenshot or user question into these APIs.
    [[nodiscard]] virtual QByteArray buildProbePayload(
        const ProviderProfile& profile,
        ProviderProbeOperation operation,
        QString* error) const = 0;
    [[nodiscard]] virtual bool parseProbeResponse(
        ProviderProbeOperation operation,
        const QByteArray& response,
        QStringList* modelIds) const = 0;

protected:
    [[nodiscard]] static QString textProbePrompt();
    [[nodiscard]] static QString imageProbePrompt();
    [[nodiscard]] static QString fixedProbeImageDataUrl();
    [[nodiscard]] static QString expectedProbeMarker(
        ProviderProbeOperation operation);
    [[nodiscard]] static bool parseOpenAiModelList(
        const QByteArray& response,
        QStringList* modelIds);
};

std::unique_ptr<ILlmProvider> createProvider(Protocol protocol);
QString defaultSystemPrompt();

} // namespace snapask::ai
