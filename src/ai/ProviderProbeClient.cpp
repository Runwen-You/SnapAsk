#include "ai/ProviderProbeClient.h"

#include "ai/AiProfileRepository.h"
#include "ai/EndpointPolicy.h"
#include "ai/ILlmProvider.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QVariant>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <utility>

namespace snapask::ai {

namespace {

constexpr qint64 maximumModelResponseBytes = 2 * 1024 * 1024;
constexpr qint64 maximumTestResponseBytes = 256 * 1024;
constexpr int maximumRedirects = 3;

[[nodiscard]] qint64 responseLimit(const ProviderProbeOperation operation)
{
    return operation == ProviderProbeOperation::ModelList
        ? maximumModelResponseBytes
        : maximumTestResponseBytes;
}

[[nodiscard]] bool proxyUrlIsSafe(const QUrl& proxy)
{
    if (proxy.isEmpty()) {
        return true;
    }
    const QString scheme = proxy.scheme().toLower();
    return proxy.isValid() && !proxy.isRelative() && !proxy.host().isEmpty()
        && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
        && proxy.userInfo().isEmpty() && proxy.query().isEmpty()
        && !proxy.hasFragment()
        && (proxy.path().isEmpty() || proxy.path() == QStringLiteral("/"));
}

[[nodiscard]] ErrorKind errorKindForHttpStatus(
    const ProviderProbeOperation operation,
    const int status)
{
    switch (status) {
    case 401:
    case 403:
        return ErrorKind::Authentication;
    case 429:
        return ErrorKind::RateLimited;
    default:
        if ((operation == ProviderProbeOperation::ModelList && status == 404)
            || (operation == ProviderProbeOperation::ImageUnderstanding
                && status == 400)) {
            return ErrorKind::UnsupportedCapability;
        }
        return status >= 400 && status <= 599
            ? ErrorKind::Server
            : ErrorKind::InvalidResponse;
    }
}

[[nodiscard]] QString normalizedMessage(
    const ProviderProbeOperation operation,
    const ErrorKind kind,
    const bool success)
{
    if (success) {
        switch (operation) {
        case ProviderProbeOperation::ModelList:
            return QStringLiteral("模型列表获取成功");
        case ProviderProbeOperation::TextConnection:
            return QStringLiteral("文本连接测试成功");
        case ProviderProbeOperation::ImageUnderstanding:
            return QStringLiteral("图片理解测试成功");
        }
    }

    switch (kind) {
    case ErrorKind::InvalidConfiguration:
        return QStringLiteral("服务测试配置无效");
    case ErrorKind::Authentication:
        return QStringLiteral("服务认证失败，请检查 API Key");
    case ErrorKind::RateLimited:
        return QStringLiteral("服务请求过于频繁，请稍后重试");
    case ErrorKind::Timeout:
        return QStringLiteral("服务测试超时");
    case ErrorKind::Network:
        return QStringLiteral("无法连接服务");
    case ErrorKind::UnsupportedCapability:
        return QStringLiteral("服务不支持所选测试能力");
    case ErrorKind::InvalidResponse:
        return QStringLiteral("服务返回格式不兼容");
    case ErrorKind::RedirectRejected:
        return QStringLiteral("服务重定向不安全，已停止测试");
    case ErrorKind::Server:
        return QStringLiteral("服务暂时不可用");
    case ErrorKind::Cancelled:
        return QStringLiteral("服务测试已取消");
    case ErrorKind::None:
        break;
    }
    return QStringLiteral("服务测试失败");
}

void wipeQString(QString& value)
{
    if (!value.isEmpty()) {
        value.fill(QChar{});
    }
    value.clear();
}

void wipeByteArray(QByteArray& value)
{
    if (!value.isEmpty()) {
        SecureZeroMemory(value.data(), static_cast<SIZE_T>(value.size()));
    }
    value.clear();
}

[[nodiscard]] bool modelIdContainsCredential(
    const QString& modelId,
    const QByteArray& authorization)
{
    constexpr qsizetype bearerPrefixSize = 7;
    if (!authorization.startsWith(QByteArrayLiteral("Bearer "))
        || authorization.size() <= bearerPrefixSize) {
        return false;
    }
    const QByteArray credential = authorization.sliced(bearerPrefixSize);
    const QByteArray candidate = modelId.toUtf8();
    return candidate == credential
        || (credential.size() >= 8 && candidate.contains(credential));
}

[[nodiscard]] bool isRedirectStatus(const int status)
{
    return status == 301 || status == 302 || status == 303
        || status == 307 || status == 308;
}

}  // namespace

struct ProviderProbeClient::ActiveProbe {
    ~ActiveProbe()
    {
        wipeByteArray(authorization);
        response.clear();
        payload.clear();
    }

    void clearAuthorization()
    {
        wipeByteArray(authorization);
    }

    QUuid operationId;
    ProviderProbeOperation operation{ProviderProbeOperation::ModelList};
    ProviderProfile profile;
    std::unique_ptr<ILlmProvider> provider;
    QUrl originalEndpoint;
    QUrl currentEndpoint;
    QByteArray payload;
    QByteArray response;
    QByteArray authorization;
    QPointer<QNetworkAccessManager> manager;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> connectTimer;
    QPointer<QTimer> requestTimer;
    qint64 receivedBytes{0};
    int redirectCount{0};
    bool receivedMetadata{false};
};

ProviderProbeClient::ProviderProbeClient(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<ProviderProbeResult>();
}

ProviderProbeClient::~ProviderProbeClient()
{
    const QList<QUuid> operationIds = active_.keys();
    for (const QUuid& operationId : operationIds) {
        cancel(operationId);
    }
}

QUuid ProviderProbeClient::fetchModels(
    const ProviderProfile& profile,
    QString&& apiKey)
{
    return start(ProviderProbeOperation::ModelList, profile, std::move(apiKey));
}

QUuid ProviderProbeClient::testTextConnection(
    const ProviderProfile& profile,
    QString&& apiKey)
{
    return start(
        ProviderProbeOperation::TextConnection,
        profile,
        std::move(apiKey));
}

QUuid ProviderProbeClient::testImageUnderstanding(
    const ProviderProfile& profile,
    QString&& apiKey)
{
    return start(
        ProviderProbeOperation::ImageUnderstanding,
        profile,
        std::move(apiKey));
}

void ProviderProbeClient::cancel(const QUuid& operationId)
{
    const std::shared_ptr<ActiveProbe> active = active_.take(operationId);
    if (!active) {
        return;
    }
    if (active->connectTimer != nullptr) {
        active->connectTimer->stop();
    }
    if (active->requestTimer != nullptr) {
        active->requestTimer->stop();
    }
    if (active->reply != nullptr) {
        active->reply->abort();
    }
    if (active->manager != nullptr) {
        active->manager->deleteLater();
    }
    active->response.clear();
    active->clearAuthorization();

    ProviderProbeResult result;
    result.operationId = operationId;
    result.providerProfileId = active->profile.id;
    result.operation = active->operation;
    result.errorKind = ErrorKind::Cancelled;
    result.message = normalizedMessage(active->operation, result.errorKind, false);
    emit resultReady(result);
}

bool ProviderProbeClient::isActive(const QUuid& operationId) const
{
    return active_.contains(operationId);
}

QUuid ProviderProbeClient::start(
    const ProviderProbeOperation operation,
    const ProviderProfile& profile,
    QString&& apiKey)
{
    const QUuid operationId = QUuid::createUuid();
    const auto reject = [
                            this,
                            operationId,
                            providerProfileId = profile.id,
                            operation,
                            &apiKey](const ErrorKind kind) {
        wipeQString(apiKey);
        emitImmediateFailure(operationId, providerProfileId, operation, kind);
        return operationId;
    };

    if (apiKey.isEmpty()) {
        return reject(ErrorKind::Authentication);
    }
    const EndpointPolicy::Result baseUrlCheck =
        EndpointPolicy::validateBaseUrl(profile.baseUrl);
    QString headerError;
    if (!baseUrlCheck.accepted
        || !AiProfileRepository::customHeadersAreSafe(
            profile.customHeaders,
            &headerError)
        || !proxyUrlIsSafe(profile.proxyUrl)
        || profile.connectTimeoutMs < 1'000
        || profile.connectTimeoutMs > 120'000
        || profile.requestTimeoutMs < profile.connectTimeoutMs
        || profile.requestTimeoutMs > 600'000) {
        return reject(ErrorKind::InvalidConfiguration);
    }
    if (operation == ProviderProbeOperation::ImageUnderstanding
        && !profile.capabilities.testFlag(ImageInput)) {
        return reject(ErrorKind::UnsupportedCapability);
    }

    std::unique_ptr<ILlmProvider> provider = createProvider(profile.protocol);
    if (!provider) {
        return reject(ErrorKind::InvalidConfiguration);
    }
    const QUrl endpoint = operation == ProviderProbeOperation::ModelList
        ? provider->modelsEndpoint(profile)
        : provider->responseEndpoint(profile);
    const EndpointPolicy::Result endpointCheck =
        EndpointPolicy::validateBaseUrl(endpoint);
    if (!endpointCheck.accepted
        || !EndpointPolicy::isSameOrigin(profile.baseUrl, endpoint)) {
        return reject(ErrorKind::InvalidConfiguration);
    }

    QByteArray payload;
    if (operation != ProviderProbeOperation::ModelList) {
        QString payloadError;
        payload = provider->buildProbePayload(profile, operation, &payloadError);
        if (payload.isEmpty() || payload.size() > 128 * 1024) {
            return reject(ErrorKind::InvalidConfiguration);
        }
    }

    auto active = std::make_shared<ActiveProbe>();
    active->operationId = operationId;
    active->operation = operation;
    active->profile = profile;
    active->provider = std::move(provider);
    active->originalEndpoint = endpoint;
    active->currentEndpoint = endpoint;
    active->payload = std::move(payload);

    QByteArray keyBytes = apiKey.toUtf8();
    active->authorization = QByteArrayLiteral("Bearer ");
    active->authorization.append(keyBytes);
    wipeByteArray(keyBytes);
    wipeQString(apiKey);

    auto* manager = new QNetworkAccessManager(this);
    active->manager = manager;
    if (!profile.proxyUrl.isEmpty()) {
        manager->setProxy(QNetworkProxy(
            QNetworkProxy::HttpProxy,
            profile.proxyUrl.host(),
            static_cast<quint16>(profile.proxyUrl.port(8080))));
    }

    auto* connectTimer = new QTimer(manager);
    connectTimer->setSingleShot(true);
    connectTimer->setInterval(profile.connectTimeoutMs);
    active->connectTimer = connectTimer;
    connect(connectTimer, &QTimer::timeout, this, [this, operationId]() {
        const std::shared_ptr<ActiveProbe> current = active_.value(operationId);
        if (current && !current->receivedMetadata) {
            fail(current, ErrorKind::Timeout);
        }
    });

    auto* requestTimer = new QTimer(manager);
    requestTimer->setSingleShot(true);
    requestTimer->setInterval(profile.requestTimeoutMs);
    active->requestTimer = requestTimer;
    connect(requestTimer, &QTimer::timeout, this, [this, operationId]() {
        const std::shared_ptr<ActiveProbe> current = active_.value(operationId);
        if (current) {
            fail(current, ErrorKind::Timeout);
        }
    });

    active_.insert(operationId, active);
    requestTimer->start();
    dispatch(active, endpoint);
    return operationId;
}

void ProviderProbeClient::dispatch(
    const std::shared_ptr<ActiveProbe>& active,
    const QUrl& endpoint)
{
    if (!active || active_.value(active->operationId) != active
        || active->manager == nullptr) {
        return;
    }

    active->currentEndpoint = endpoint;
    active->response.clear();
    active->receivedBytes = 0;
    active->receivedMetadata = false;

    QNetworkRequest request(endpoint);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", active->authorization);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(active->profile.requestTimeoutMs);
    for (auto it = active->profile.customHeaders.constBegin();
         it != active->profile.customHeaders.constEnd();
         ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    }

    QNetworkReply* reply = nullptr;
    if (active->operation == ProviderProbeOperation::ModelList) {
        reply = active->manager->get(request);
    } else {
        request.setHeader(
            QNetworkRequest::ContentTypeHeader,
            QStringLiteral("application/json"));
        reply = active->manager->post(request, active->payload);
    }
    active->reply = reply;

    connect(reply, &QNetworkReply::metaDataChanged, this,
            [this, operationId = active->operationId, reply]() {
                handleMetadata(operationId, reply);
            });
    connect(reply, &QIODevice::readyRead, this,
            [this, operationId = active->operationId, reply]() {
                consume(operationId, reply);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, operationId = active->operationId, reply]() {
                finish(operationId, reply);
            });

    if (active->connectTimer != nullptr) {
        active->connectTimer->start();
    }
}

void ProviderProbeClient::handleMetadata(
    const QUuid& operationId,
    QNetworkReply* reply)
{
    const std::shared_ptr<ActiveProbe> active = active_.value(operationId);
    if (!active || active->reply != reply) {
        return;
    }
    active->receivedMetadata = true;
    if (active->connectTimer != nullptr) {
        active->connectTimer->stop();
    }

    bool ok = false;
    const qint64 declaredLength =
        reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
    if (ok && declaredLength > responseLimit(active->operation)) {
        fail(active, ErrorKind::InvalidResponse);
    }
}

void ProviderProbeClient::consume(
    const QUuid& operationId,
    QNetworkReply* reply)
{
    const std::shared_ptr<ActiveProbe> active = active_.value(operationId);
    if (!active || active->reply != reply) {
        (void)reply->readAll();
        return;
    }

    QByteArray bytes = reply->readAll();
    active->receivedBytes += bytes.size();
    if (active->receivedBytes > responseLimit(active->operation)) {
        bytes.clear();
        fail(active, ErrorKind::InvalidResponse);
        return;
    }

    const int status = reply->attribute(
                                 QNetworkRequest::HttpStatusCodeAttribute)
                           .toInt();
    if (status >= 200 && status < 300) {
        active->response.append(bytes);
    }
}

void ProviderProbeClient::finish(
    const QUuid& operationId,
    QNetworkReply* reply)
{
    std::shared_ptr<ActiveProbe> active = active_.value(operationId);
    if (!active || active->reply != reply) {
        reply->deleteLater();
        return;
    }
    if (reply->bytesAvailable() > 0) {
        consume(operationId, reply);
        active = active_.value(operationId);
        if (!active || active->reply != reply) {
            reply->deleteLater();
            return;
        }
    }

    const int status = reply->attribute(
                                 QNetworkRequest::HttpStatusCodeAttribute)
                           .toInt();
    if (isRedirectStatus(status)) {
        const QUrl target = reply->attribute(
                                      QNetworkRequest::RedirectionTargetAttribute)
                                .toUrl();
        const QUrl redirected = active->currentEndpoint.resolved(target);
        const EndpointPolicy::Result redirectCheck =
            EndpointPolicy::validateBaseUrl(redirected);
        if (target.isEmpty() || !redirectCheck.accepted
            || !EndpointPolicy::isSameOrigin(active->originalEndpoint, redirected)
            || active->redirectCount >= maximumRedirects) {
            fail(active, ErrorKind::RedirectRejected, status);
            reply->deleteLater();
            return;
        }

        ++active->redirectCount;
        active->reply = nullptr;
        reply->deleteLater();
        dispatch(active, redirected);
        return;
    }

    if (status < 200 || status >= 300) {
        fail(active, errorKindForHttpStatus(active->operation, status), status);
        reply->deleteLater();
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(active, ErrorKind::Network, status);
        reply->deleteLater();
        return;
    }

    QStringList modelIds;
    if (!active->provider->parseProbeResponse(
            active->operation,
            active->response,
            &modelIds)) {
        fail(active, ErrorKind::InvalidResponse, status);
        reply->deleteLater();
        return;
    }
    if (active->operation == ProviderProbeOperation::ModelList
        && modelIds.isEmpty()) {
        fail(active, ErrorKind::UnsupportedCapability, status);
        reply->deleteLater();
        return;
    }
    if (active->operation == ProviderProbeOperation::ModelList) {
        modelIds.removeIf([&active](const QString& modelId) {
            return modelIdContainsCredential(modelId, active->authorization);
        });
        if (modelIds.isEmpty()) {
            fail(active, ErrorKind::InvalidResponse, status);
            reply->deleteLater();
            return;
        }
    }
    succeed(active, std::move(modelIds));
    reply->deleteLater();
}

void ProviderProbeClient::succeed(
    const std::shared_ptr<ActiveProbe>& active,
    QStringList modelIds)
{
    if (!active || active_.take(active->operationId) != active) {
        return;
    }
    if (active->connectTimer != nullptr) {
        active->connectTimer->stop();
    }
    if (active->requestTimer != nullptr) {
        active->requestTimer->stop();
    }
    if (active->manager != nullptr) {
        active->manager->deleteLater();
    }
    active->response.clear();
    active->clearAuthorization();

    ProviderProbeResult result;
    result.operationId = active->operationId;
    result.providerProfileId = active->profile.id;
    result.operation = active->operation;
    result.success = true;
    result.modelIds = std::move(modelIds);
    result.message = normalizedMessage(active->operation, ErrorKind::None, true);
    emit resultReady(result);
}

void ProviderProbeClient::fail(
    const std::shared_ptr<ActiveProbe>& active,
    const ErrorKind kind,
    const int httpStatus)
{
    if (!active || active_.take(active->operationId) != active) {
        return;
    }
    if (active->connectTimer != nullptr) {
        active->connectTimer->stop();
    }
    if (active->requestTimer != nullptr) {
        active->requestTimer->stop();
    }
    if (active->reply != nullptr) {
        active->reply->abort();
    }
    if (active->manager != nullptr) {
        active->manager->deleteLater();
    }
    active->response.clear();
    active->clearAuthorization();

    ProviderProbeResult result;
    result.operationId = active->operationId;
    result.providerProfileId = active->profile.id;
    result.operation = active->operation;
    result.errorKind = kind;
    result.message = normalizedMessage(active->operation, kind, false);
    result.httpStatus = httpStatus;
    emit resultReady(result);
}

void ProviderProbeClient::emitImmediateFailure(
    const QUuid& operationId,
    const QUuid& providerProfileId,
    const ProviderProbeOperation operation,
    const ErrorKind kind)
{
    ProviderProbeResult result;
    result.operationId = operationId;
    result.providerProfileId = providerProfileId;
    result.operation = operation;
    result.errorKind = kind;
    result.message = normalizedMessage(operation, kind, false);
    emit resultReady(result);
}

}  // namespace snapask::ai
