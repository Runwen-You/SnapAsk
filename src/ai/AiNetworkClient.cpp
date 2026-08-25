#include "ai/AiNetworkClient.h"

#include "ai/AiProfileRepository.h"
#include "ai/EndpointPolicy.h"
#include "ai/ILlmProvider.h"
#include "ai/SseDecoder.h"
#include "services/SnapshotRenderer.h"

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QtConcurrentRun>

#include <Windows.h>

namespace snapask::ai {

namespace {

struct PayloadBuildResult final {
    QUuid sessionId;
    QUuid snapshotId;
    QUuid requestId;
    QByteArray snapshotSha256;
    QByteArray payload;
    QString error;
};

void wipeByteArray(QByteArray& value) noexcept
{
    if (!value.isEmpty()) {
        SecureZeroMemory(value.data(), static_cast<SIZE_T>(value.size()));
    }
    value.clear();
}

[[nodiscard]] PayloadBuildResult buildPayload(
    const ProviderProfile& profile,
    const AiRequest& request)
{
    PayloadBuildResult result;
    result.sessionId = request.sessionId;
    result.snapshotId = request.snapshotId;
    result.requestId = request.requestId;
    result.snapshotSha256 = QCryptographicHash::hash(
        request.snapshotPng, QCryptographicHash::Sha256);
    if (result.snapshotSha256 != request.snapshotSha256) {
        result.error = QStringLiteral("当前截图快照完整性校验失败");
        return result;
    }

    const std::unique_ptr<ILlmProvider> provider =
        createProvider(profile.protocol);
    if (!provider) {
        result.error = QStringLiteral("服务协议不受支持");
        return result;
    }
    result.payload =
        provider->buildStreamingPayload(profile, request, &result.error);
    return result;
}

}  // namespace

struct AiNetworkClient::ActiveRequest {
    ~ActiveRequest()
    {
        wipeByteArray(authorization);
    }

    AiRequest request;
    ProviderProfile profile;
    std::unique_ptr<ILlmProvider> provider;
    SseDecoder decoder;
    QPointer<QNetworkAccessManager> manager;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> connectTimer;
    QPointer<QTimer> requestTimer;
    QPointer<QFutureWatcher<PayloadBuildResult>> payloadWatcher;
    QByteArray authorization;
    bool terminalSent = false;
    bool receivedResponse = false;
    bool responseMetadataValidated = false;
};

AiNetworkClient::AiNetworkClient(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<AiStreamEvent>();
}

AiNetworkClient::~AiNetworkClient()
{
    const auto ids = active_.keys();
    for (const auto& id : ids) cancel(id);
}

QUuid AiNetworkClient::sendExplicit(
    AiRequest request,
    const snapask::RenderedSnapshot& snapshot,
    const ProviderProfile& profile,
    QString&& apiKey)
{
    if (request.requestId.isNull()) request.requestId = QUuid::createUuid();
    request.providerProfileId = profile.id;
    if (request.snapshotId.isNull()) request.snapshotId = QUuid::createUuid();

    auto emitImmediateFailure = [this, &request](const ErrorKind kind, const QString& message) {
        AiStreamEvent event;
        event.type = EventType::Failed;
        event.requestId = request.requestId;
        event.errorKind = kind;
        event.errorMessage = message;
        emit eventReady(event);
    };

    if (request.sessionId.isNull() || profile.id.isNull() || !snapshot.isValid()) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, QStringLiteral("请求缺少会话或截图信息"));
        apiKey.fill(QChar{});
        return request.requestId;
    }
    request.snapshotPng = snapshot.pngBytes();
    request.snapshotSha256 = snapshot.sha256();
    if (request.question.trimmed().isEmpty() || request.question.size() > 65'536
        || request.snapshotPng.size() > 64 * 1024 * 1024
        || request.recentContext.size() > 40) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, QStringLiteral("问题、上下文或截图超出允许范围"));
        apiKey.fill(QChar{});
        return request.requestId;
    }
    if (!profile.capabilities.testFlag(ImageInput)
        || !profile.capabilities.testFlag(Streaming)) {
        emitImmediateFailure(ErrorKind::UnsupportedCapability, QStringLiteral("当前服务不支持图片流式问答"));
        apiKey.fill(QChar{});
        return request.requestId;
    }
    const auto endpointCheck = EndpointPolicy::validateBaseUrl(profile.baseUrl);
    if (!endpointCheck.accepted) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, endpointCheck.error);
        apiKey.fill(QChar{});
        return request.requestId;
    }
    QString headerError;
    if (!AiProfileRepository::customHeadersAreSafe(profile.customHeaders, &headerError)) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, headerError);
        apiKey.fill(QChar{});
        return request.requestId;
    }
    if (profile.connectTimeoutMs < 1'000 || profile.connectTimeoutMs > 120'000
        || profile.requestTimeoutMs < profile.connectTimeoutMs
        || profile.requestTimeoutMs > 600'000) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, QStringLiteral("超时设置无效"));
        apiKey.fill(QChar{});
        return request.requestId;
    }
    if (apiKey.isEmpty()) {
        emitImmediateFailure(ErrorKind::Authentication, QStringLiteral("尚未保存 API Key"));
        return request.requestId;
    }
    if (active_.contains(request.requestId)) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, QStringLiteral("请求 ID 已在使用中"));
        apiKey.fill(QChar{});
        return request.requestId;
    }

    auto active = std::make_shared<ActiveRequest>();
    active->request = request;
    active->profile = profile;
    active->provider = createProvider(profile.protocol);
    if (!active->provider) {
        emitImmediateFailure(ErrorKind::InvalidConfiguration, QStringLiteral("服务协议不受支持"));
        apiKey.fill(QChar{});
        return request.requestId;
    }

    active->authorization = QByteArrayLiteral("Bearer ") + apiKey.toUtf8();
    apiKey.fill(QChar{});

    active_.insert(request.requestId, active);
    auto* watcher = new QFutureWatcher<PayloadBuildResult>(this);
    active->payloadWatcher = watcher;
    connect(
        watcher,
        &QFutureWatcher<PayloadBuildResult>::finished,
        this,
        [this, watcher, id = request.requestId]() {
            const PayloadBuildResult result = watcher->result();
            watcher->deleteLater();

            const auto pending = active_.value(id);
            if (!pending || pending->terminalSent) {
                return;
            }
            pending->payloadWatcher = nullptr;
            if (result.sessionId != pending->request.sessionId
                || result.snapshotId != pending->request.snapshotId
                || result.requestId != pending->request.requestId
                || result.snapshotSha256
                    != pending->request.snapshotSha256) {
                fail(
                    pending,
                    ErrorKind::InvalidConfiguration,
                    QStringLiteral("后台截图结果已失效，请重新发送"));
                discard(id);
                return;
            }
            if (result.payload.isEmpty()) {
                fail(
                    pending,
                    ErrorKind::InvalidConfiguration,
                    result.error.isEmpty()
                        ? QStringLiteral("无法构建服务请求")
                        : result.error);
                discard(id);
                return;
            }
            startNetwork(pending, result.payload);
        });
    watcher->setFuture(QtConcurrent::run(
        [profile, request]() { return buildPayload(profile, request); }));

    return request.requestId;
}

void AiNetworkClient::startNetwork(
    const std::shared_ptr<ActiveRequest>& active,
    QByteArray payload)
{
    if (!active || active->terminalSent
        || active_.value(active->request.requestId) != active) {
        return;
    }

    auto* manager = new QNetworkAccessManager(this);
    active->manager = manager;
    const ProviderProfile& profile = active->profile;
    if (profile.proxyUrl.isValid() && !profile.proxyUrl.isEmpty()) {
        const QString proxyScheme = profile.proxyUrl.scheme().toLower();
        if (!profile.proxyUrl.userInfo().isEmpty()
            || (proxyScheme != QStringLiteral("http")
                && proxyScheme != QStringLiteral("https"))
            || profile.proxyUrl.host().isEmpty()
            || !profile.proxyUrl.query().isEmpty()
            || profile.proxyUrl.hasFragment()
            || (profile.proxyUrl.path() != QString()
                && profile.proxyUrl.path() != QStringLiteral("/"))) {
            fail(
                active,
                ErrorKind::InvalidConfiguration,
                QStringLiteral("代理地址不能包含凭据"));
            discard(active->request.requestId);
            return;
        }
        manager->setProxy(QNetworkProxy(
            QNetworkProxy::HttpProxy,
            profile.proxyUrl.host(),
            static_cast<quint16>(profile.proxyUrl.port(8080))));
    }

    QNetworkRequest networkRequest(
        active->provider->responseEndpoint(profile));
    networkRequest.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/json"));
    networkRequest.setRawHeader("Accept", "text/event-stream");
    networkRequest.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setTransferTimeout(profile.requestTimeoutMs);
    for (auto it = profile.customHeaders.constBegin();
         it != profile.customHeaders.constEnd();
         ++it) {
        networkRequest.setRawHeader(
            it.key().toUtf8(), it.value().toString().toUtf8());
    }
    networkRequest.setRawHeader("Authorization", active->authorization);
    wipeByteArray(active->authorization);

    auto* reply = manager->post(networkRequest, std::move(payload));
    active->reply = reply;

    auto* connectTimer = new QTimer(manager);
    connectTimer->setSingleShot(true);
    connectTimer->setInterval(profile.connectTimeoutMs);
    active->connectTimer = connectTimer;
    connect(
        connectTimer,
        &QTimer::timeout,
        this,
        [this, id = active->request.requestId] {
            const auto pending = active_.value(id);
            if (!pending || pending->receivedResponse) {
                return;
            }
            fail(
                pending,
                ErrorKind::Timeout,
                QStringLiteral("连接服务超时"));
            if (pending->reply) {
                pending->reply->abort();
            }
        });
    connectTimer->start();

    auto* requestTimer = new QTimer(manager);
    requestTimer->setSingleShot(true);
    requestTimer->setInterval(profile.requestTimeoutMs);
    active->requestTimer = requestTimer;
    connect(
        requestTimer,
        &QTimer::timeout,
        this,
        [this, id = active->request.requestId] {
            const auto pending = active_.value(id);
            if (!pending || pending->terminalSent) {
                return;
            }
            fail(
                pending,
                ErrorKind::Timeout,
                QStringLiteral("生成回答超时"));
            if (pending->reply) {
                pending->reply->abort();
            }
        });
    requestTimer->start();

    connect(
        reply,
        &QNetworkReply::metaDataChanged,
        this,
        [this, id = active->request.requestId] {
            const auto pending = active_.value(id);
            if (!pending) {
                return;
            }
            pending->receivedResponse = true;
            if (pending->connectTimer) {
                pending->connectTimer->stop();
            }
            if (!validateResponseMetadata(pending, false)
                && pending->reply) {
                pending->reply->abort();
            }
        });
    connect(
        reply,
        &QIODevice::readyRead,
        this,
        [this, id = active->request.requestId] {
            const auto pending = active_.value(id);
            if (!pending || !pending->reply || pending->terminalSent) {
                return;
            }
            pending->receivedResponse = true;
            if (pending->connectTimer) {
                pending->connectTimer->stop();
            }
            if (!validateResponseMetadata(pending, false)) {
                if (pending->reply) {
                    pending->reply->abort();
                }
                return;
            }
            consume(id, pending->reply->readAll());
        });
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, id = active->request.requestId] { finish(id); });

    AiStreamEvent started;
    started.type = EventType::Started;
    started.requestId = active->request.requestId;
    emit eventReady(started);
}

void AiNetworkClient::cancel(const QUuid& requestId)
{
    const auto active = active_.take(requestId);
    if (!active) return;
    active->terminalSent = true;
    if (active->connectTimer) active->connectTimer->stop();
    if (active->requestTimer) active->requestTimer->stop();
    if (active->reply) active->reply->abort();

    AiStreamEvent event;
    event.type = EventType::Cancelled;
    event.requestId = requestId;
    event.errorKind = ErrorKind::Cancelled;
    emit eventReady(event);
    if (active->manager) active->manager->deleteLater();
}

void AiNetworkClient::cancelSession(const QUuid& sessionId)
{
    QList<QUuid> ids;
    for (auto it = active_.constBegin(); it != active_.constEnd(); ++it) {
        if (it.value()->request.sessionId == sessionId) ids.append(it.key());
    }
    for (const auto& id : ids) cancel(id);
}

bool AiNetworkClient::isActive(const QUuid& requestId) const
{
    return active_.contains(requestId);
}

void AiNetworkClient::consume(const QUuid& requestId, const QByteArray& bytes)
{
    const auto active = active_.value(requestId);
    if (!active || active->terminalSent) return;
    const auto decoded = active->decoder.push(bytes);
    if (active->decoder.hasError()) {
        fail(active, ErrorKind::InvalidResponse, active->decoder.errorString());
        if (active->reply) active->reply->abort();
        return;
    }
    for (const auto& event : decoded) {
        handleMappedEvents(active, active->provider->mapEvent(event, requestId));
        if (active->terminalSent) break;
    }
}

void AiNetworkClient::handleMappedEvents(
    const std::shared_ptr<ActiveRequest>& active,
    const QList<AiStreamEvent>& events)
{
    for (const auto& event : events) {
        if (active->terminalSent) return;
        emit eventReady(event);
        if (event.type == EventType::Completed || event.type == EventType::Failed
            || event.type == EventType::Cancelled) {
            active->terminalSent = true;
            if (active->requestTimer) active->requestTimer->stop();
            if (active->reply && !active->reply->isFinished()) {
                active->reply->abort();
            }
        }
    }
}

void AiNetworkClient::finish(const QUuid& requestId)
{
    const auto active = active_.value(requestId);
    if (!active) return; // Cancelled/closed requests intentionally drop late callbacks.
    const bool metadataAccepted = active->terminalSent
        || validateResponseMetadata(active, true);
    if (metadataAccepted && active->reply && !active->terminalSent) {
        consume(requestId, active->reply->readAll());
        handleMappedEvents(active, [&] {
            QList<AiStreamEvent> mapped;
            for (const auto& event : active->decoder.finish()) {
                mapped.append(active->provider->mapEvent(event, requestId));
            }
            return mapped;
        }());
    }

    if (!active->terminalSent && active->reply) {
        const auto status = active->reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (active->reply->error() != QNetworkReply::NoError) {
            fail(active, ErrorKind::Network, QStringLiteral("网络连接失败，请检查网络、代理和证书"), status);
        } else {
            fail(active, ErrorKind::InvalidResponse, QStringLiteral("流式回答意外结束"), status);
        }
    }
    discard(requestId);
}

bool AiNetworkClient::validateResponseMetadata(
    const std::shared_ptr<ActiveRequest>& active,
    const bool finalCheck)
{
    if (!active || !active->reply || active->terminalSent) {
        return active && active->responseMetadataValidated;
    }
    if (active->responseMetadataValidated) return true;

    const QUrl redirect = active->reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const int status = active->reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!redirect.isEmpty()) {
        fail(active, ErrorKind::RedirectRejected,
             QStringLiteral("服务发生重定向，已为保护凭据停止请求"), status);
        return false;
    }
    if (status == 401 || status == 403) {
        fail(active, ErrorKind::Authentication,
             QStringLiteral("API Key 无效或无权访问该模型"), status);
        return false;
    }
    if (status == 429) {
        fail(active, ErrorKind::RateLimited,
             QStringLiteral("服务请求过于频繁或额度不足"), status);
        return false;
    }
    if (status >= 500) {
        fail(active, ErrorKind::Server, QStringLiteral("服务暂时不可用"), status);
        return false;
    }
    if (status > 0 && (status < 200 || status >= 300)) {
        fail(active, ErrorKind::Server, QStringLiteral("服务拒绝了请求"), status);
        return false;
    }
    if (status >= 200 && status < 300) {
        active->responseMetadataValidated = true;
        return true;
    }
    if (finalCheck && active->reply->error() == QNetworkReply::NoError) {
        fail(active, ErrorKind::InvalidResponse, QStringLiteral("服务未返回有效 HTTP 状态"));
    }
    return false;
}

void AiNetworkClient::fail(
    const std::shared_ptr<ActiveRequest>& active,
    const ErrorKind kind,
    const QString& message,
    const int httpStatus)
{
    if (!active || active->terminalSent) return;
    active->terminalSent = true;
    if (active->connectTimer) active->connectTimer->stop();
    if (active->requestTimer) active->requestTimer->stop();
    AiStreamEvent event;
    event.type = EventType::Failed;
    event.requestId = active->request.requestId;
    event.errorKind = kind;
    event.errorMessage = message;
    event.httpStatus = httpStatus;
    emit eventReady(event);
}

void AiNetworkClient::discard(const QUuid& requestId)
{
    const auto active = active_.take(requestId);
    if (!active) return;
    if (active->connectTimer) active->connectTimer->stop();
    if (active->requestTimer) active->requestTimer->stop();
    if (active->manager) active->manager->deleteLater();
}

} // namespace snapask::ai
