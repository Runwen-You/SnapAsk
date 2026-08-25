#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>

namespace snapask::ai {

inline constexpr int kProviderConfigurationSchemaVersion = 1;

enum class Protocol {
    OpenAIResponses,
    ChatCompletions,
};

enum Capability : quint32 {
    NoCapabilities = 0,
    ImageInput = 1U << 0U,
    Streaming = 1U << 1U,
    ModelList = 1U << 2U,
};
Q_DECLARE_FLAGS(Capabilities, Capability)

struct ProviderProfile {
    QUuid id;
    QString displayName;
    Protocol protocol = Protocol::OpenAIResponses;
    QUrl baseUrl;
    QString credentialRef;
    QString modelId;
    QStringList availableModels;
    int connectTimeoutMs = 15'000;
    int requestTimeoutMs = 120'000;
    Capabilities capabilities = Capabilities(ImageInput | Streaming | ModelList);
    QUrl proxyUrl;
    QJsonObject customHeaders;
    QDateTime lastTestedAt;
    QString lastTestStatus;
};

struct ConversationMessage {
    enum class Role { User, Assistant };
    Role role = Role::User;
    QString text;
};

struct AiRequest {
    QUuid requestId;
    QUuid sessionId;
    QUuid snapshotId;
    QUuid providerProfileId;
    QString modelId;
    QByteArray snapshotPng;
    QByteArray snapshotSha256;
    QString question;
    QList<ConversationMessage> recentContext;
};

enum class EventType {
    Started,
    TextDelta,
    UsageUpdated,
    Completed,
    Cancelled,
    Failed,
};

enum class ErrorKind {
    None,
    InvalidConfiguration,
    Authentication,
    RateLimited,
    Timeout,
    Network,
    UnsupportedCapability,
    InvalidResponse,
    RedirectRejected,
    Server,
    Cancelled,
};

enum class ProviderProbeOperation {
    ModelList,
    TextConnection,
    ImageUnderstanding,
};

// Sanitized result from a configuration probe. Raw provider bodies, request
// payloads, credentials and test-image bytes never cross this boundary.
struct ProviderProbeResult {
    QUuid operationId;
    QUuid providerProfileId;
    ProviderProbeOperation operation = ProviderProbeOperation::ModelList;
    bool success = false;
    QStringList modelIds;
    ErrorKind errorKind = ErrorKind::None;
    QString message;
    int httpStatus = 0;
};

struct AiStreamEvent {
    EventType type = EventType::Started;
    QUuid requestId;
    QString text;
    qint64 inputTokens = -1;
    qint64 outputTokens = -1;
    ErrorKind errorKind = ErrorKind::None;
    QString errorMessage;
    int httpStatus = 0;
};

QString protocolName(Protocol protocol);
QString errorKindName(ErrorKind kind);
QString providerProbeOperationName(ProviderProbeOperation operation);

} // namespace snapask::ai

Q_DECLARE_OPERATORS_FOR_FLAGS(snapask::ai::Capabilities)
Q_DECLARE_METATYPE(snapask::ai::AiStreamEvent)
Q_DECLARE_METATYPE(snapask::ai::ProviderProbeResult)
