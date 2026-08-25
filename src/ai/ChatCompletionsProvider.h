#pragma once

#include "ai/ILlmProvider.h"

namespace snapask::ai {

class ChatCompletionsProvider final : public ILlmProvider {
public:
    [[nodiscard]] Protocol protocol() const override;
    [[nodiscard]] QUrl responseEndpoint(const ProviderProfile& profile) const override;
    [[nodiscard]] QUrl modelsEndpoint(const ProviderProfile& profile) const override;
    [[nodiscard]] QByteArray buildStreamingPayload(
        const ProviderProfile& profile,
        const AiRequest& request,
        QString* error) const override;
    [[nodiscard]] QList<AiStreamEvent> mapEvent(
        const SseEvent& event,
        const QUuid& requestId) const override;
    [[nodiscard]] QByteArray buildProbePayload(
        const ProviderProfile& profile,
        ProviderProbeOperation operation,
        QString* error) const override;
    [[nodiscard]] bool parseProbeResponse(
        ProviderProbeOperation operation,
        const QByteArray& response,
        QStringList* modelIds) const override;
};

} // namespace snapask::ai
