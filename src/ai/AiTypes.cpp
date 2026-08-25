#include "ai/AiTypes.h"

namespace snapask::ai {

QString protocolName(const Protocol protocol)
{
    switch (protocol) {
    case Protocol::OpenAIResponses:
        return QStringLiteral("openai-responses");
    case Protocol::ChatCompletions:
        return QStringLiteral("openai-compatible-chat-completions");
    }
    return QStringLiteral("unknown");
}

QString errorKindName(const ErrorKind kind)
{
    switch (kind) {
    case ErrorKind::None: return QStringLiteral("none");
    case ErrorKind::InvalidConfiguration: return QStringLiteral("invalid_configuration");
    case ErrorKind::Authentication: return QStringLiteral("authentication");
    case ErrorKind::RateLimited: return QStringLiteral("rate_limited");
    case ErrorKind::Timeout: return QStringLiteral("timeout");
    case ErrorKind::Network: return QStringLiteral("network");
    case ErrorKind::UnsupportedCapability: return QStringLiteral("unsupported_capability");
    case ErrorKind::InvalidResponse: return QStringLiteral("invalid_response");
    case ErrorKind::RedirectRejected: return QStringLiteral("redirect_rejected");
    case ErrorKind::Server: return QStringLiteral("server");
    case ErrorKind::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("unknown");
}

QString providerProbeOperationName(const ProviderProbeOperation operation)
{
    switch (operation) {
    case ProviderProbeOperation::ModelList:
        return QStringLiteral("model_list");
    case ProviderProbeOperation::TextConnection:
        return QStringLiteral("text_connection");
    case ProviderProbeOperation::ImageUnderstanding:
        return QStringLiteral("image_understanding");
    }
    return QStringLiteral("unknown");
}

} // namespace snapask::ai
