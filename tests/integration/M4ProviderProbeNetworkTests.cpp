#include "ai/ProviderProbeClient.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>

#include <functional>
#include <utility>

using snapask::ai::Capabilities;
using snapask::ai::ErrorKind;
using snapask::ai::ImageInput;
using snapask::ai::ModelList;
using snapask::ai::Protocol;
using snapask::ai::ProviderProbeClient;
using snapask::ai::ProviderProbeOperation;
using snapask::ai::ProviderProbeResult;
using snapask::ai::ProviderProfile;

namespace {

constexpr qsizetype maximumTestResponseBytes = 256 * 1024;
constexpr auto userQuestionSentinel = "USER_QUESTION_MUST_NOT_BE_SENT";
constexpr auto userScreenshotSentinel = "USER_SCREENSHOT_MUST_NOT_BE_SENT";
constexpr auto rawProviderBodySentinel = "RAW_PROVIDER_BODY_MUST_NOT_ESCAPE";

struct CapturedRequest {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct ResponsePlan {
    int statusCode{200};
    QByteArray reasonPhrase{QByteArrayLiteral("OK")};
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    qint64 contentLengthOverride{-1};
    bool omitContentLength{false};
    bool sendHeaders{true};
    bool closeAfterWrite{true};
};

class LocalHttpServer final : public QObject {
public:
    using Handler = std::function<ResponsePlan(const CapturedRequest&)>;

    explicit LocalHttpServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            acceptConnections();
        });
    }

    [[nodiscard]] bool listen()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    [[nodiscard]] QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1")
                        .arg(server_.serverPort()));
    }

    [[nodiscard]] QUrl urlFor(const QString& path) const
    {
        QUrl result = baseUrl();
        result.setPath(path);
        return result;
    }

    void setHandler(Handler handler)
    {
        handler_ = std::move(handler);
    }

    [[nodiscard]] qsizetype requestCount() const noexcept
    {
        return requests_.size();
    }

    [[nodiscard]] int disconnectCount() const noexcept
    {
        return disconnectCount_;
    }

    [[nodiscard]] const CapturedRequest& requestAt(const qsizetype index) const
    {
        return requests_.at(index);
    }

    [[nodiscard]] bool writeLateBody(const QByteArray& bytes)
    {
        if (lastSocket_.isNull()
            || lastSocket_->state() == QAbstractSocket::UnconnectedState) {
            return false;
        }
        const qint64 written = lastSocket_->write(bytes);
        lastSocket_->flush();
        return written == bytes.size();
    }

private:
    void acceptConnections()
    {
        while (server_.hasPendingConnections()) {
            QTcpSocket* socket = server_.nextPendingConnection();
            socket->setParent(this);
            buffers_.insert(socket, {});
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                readRequest(socket);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                ++disconnectCount_;
                buffers_.remove(socket);
                handled_.remove(socket);
                socket->deleteLater();
            });
        }
    }

    void readRequest(QTcpSocket* socket)
    {
        if (handled_.contains(socket)) {
            (void)socket->readAll();
            return;
        }

        QByteArray& bytes = buffers_[socket];
        bytes.append(socket->readAll());
        const qsizetype headerEnd = bytes.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        const QList<QByteArray> lines = bytes.left(headerEnd).split('\n');
        if (lines.isEmpty()) {
            return;
        }

        CapturedRequest request;
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() >= 2) {
            request.method = requestLine.at(0);
            request.target = requestLine.at(1);
        }

        qint64 contentLength = 0;
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const QByteArray line = lines.at(index).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.sliced(colon + 1).trimmed();
            request.headers.insert(name, value);
            if (name == QByteArrayLiteral("content-length")) {
                bool ok = false;
                const qint64 parsed = value.toLongLong(&ok);
                if (ok && parsed >= 0) {
                    contentLength = parsed;
                }
            }
        }

        const qsizetype bodyStart = headerEnd + 4;
        const qint64 availableBodyBytes = bytes.size() - bodyStart;
        if (availableBodyBytes < contentLength) {
            return;
        }

        request.body = bytes.sliced(
            bodyStart,
            static_cast<qsizetype>(contentLength));
        requests_.append(std::move(request));
        handled_.insert(socket);
        lastSocket_ = socket;

        const ResponsePlan plan = handler_
            ? handler_(requests_.last())
            : ResponsePlan{};
        sendResponse(socket, plan);
    }

    static void sendResponse(QTcpSocket* socket, const ResponsePlan& plan)
    {
        if (!plan.sendHeaders) {
            return;
        }

        QByteArray response = QByteArrayLiteral("HTTP/1.1 ")
            + QByteArray::number(plan.statusCode) + ' ' + plan.reasonPhrase
            + QByteArrayLiteral("\r\n");
        bool hasContentLength = false;
        bool hasConnection = false;
        for (const auto& header : plan.headers) {
            const QByteArray lowerName = header.first.toLower();
            hasContentLength = hasContentLength
                || lowerName == QByteArrayLiteral("content-length");
            hasConnection = hasConnection
                || lowerName == QByteArrayLiteral("connection");
            response += header.first + QByteArrayLiteral(": ") + header.second
                + QByteArrayLiteral("\r\n");
        }
        if (!plan.omitContentLength && !hasContentLength) {
            const qint64 declaredLength = plan.contentLengthOverride >= 0
                ? plan.contentLengthOverride
                : plan.body.size();
            response += QByteArrayLiteral("Content-Length: ")
                + QByteArray::number(declaredLength)
                + QByteArrayLiteral("\r\n");
        }
        if (!hasConnection) {
            response += plan.closeAfterWrite
                ? QByteArrayLiteral("Connection: close\r\n")
                : QByteArrayLiteral("Connection: keep-alive\r\n");
        }
        response += QByteArrayLiteral("\r\n");
        response += plan.body;

        (void)socket->write(response);
        socket->flush();
        if (plan.closeAfterWrite) {
            socket->disconnectFromHost();
        }
    }

    QTcpServer server_;
    Handler handler_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QSet<QTcpSocket*> handled_;
    QList<CapturedRequest> requests_;
    QPointer<QTcpSocket> lastSocket_;
    int disconnectCount_{0};
};

[[nodiscard]] ProviderProfile profileFor(
    const LocalHttpServer& server,
    const Protocol protocol)
{
    ProviderProfile profile;
    profile.id = QUuid::createUuid();
    profile.displayName = QString::fromLatin1(userQuestionSentinel);
    profile.protocol = protocol;
    profile.baseUrl = server.baseUrl();
    profile.credentialRef = QString::fromLatin1(userScreenshotSentinel);
    profile.modelId = QStringLiteral("fixed-probe-model");
    profile.availableModels = {
        QStringLiteral("PRIVATE_MODEL_HISTORY_MUST_NOT_BE_SENT"),
    };
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 3'000;
    profile.capabilities = Capabilities(ImageInput | ModelList);
    return profile;
}

[[nodiscard]] QByteArray modelListResponse()
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("data"), QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("model-a")}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("model-a")}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("model-b")}},
         }},
    }).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray probeResponse(
    const Protocol protocol,
    const ProviderProbeOperation operation)
{
    const QString marker = operation == ProviderProbeOperation::TextConnection
        ? QStringLiteral("SNAPASK_TEXT_OK")
        : QStringLiteral("SNAPASK_IMAGE_RED");
    if (protocol == Protocol::OpenAIResponses) {
        return QJsonDocument(QJsonObject{
            {QStringLiteral("output"), QJsonArray{QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("message")},
                 {QStringLiteral("content"), QJsonArray{QJsonObject{
                      {QStringLiteral("type"), QStringLiteral("output_text")},
                      {QStringLiteral("text"), marker},
                  }}},
             }}},
        }).toJson(QJsonDocument::Compact);
    }
    return QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{QJsonObject{
             {QStringLiteral("message"), QJsonObject{
                  {QStringLiteral("role"), QStringLiteral("assistant")},
                  {QStringLiteral("content"), marker},
              }},
         }}},
    }).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QUuid startProbe(
    ProviderProbeClient* client,
    const ProviderProfile& profile,
    const ProviderProbeOperation operation,
    QString apiKey)
{
    switch (operation) {
    case ProviderProbeOperation::ModelList:
        return client->fetchModels(profile, std::move(apiKey));
    case ProviderProbeOperation::TextConnection:
        return client->testTextConnection(profile, std::move(apiKey));
    case ProviderProbeOperation::ImageUnderstanding:
        return client->testImageUnderstanding(profile, std::move(apiKey));
    }
    return {};
}

[[nodiscard]] ProviderProbeResult firstResult(const QSignalSpy& spy)
{
    if (spy.isEmpty() || spy.first().isEmpty()) {
        return {};
    }
    return qvariant_cast<ProviderProbeResult>(spy.first().first());
}

struct PayloadView {
    QString prompt;
    QString imageDataUrl;
};

[[nodiscard]] PayloadView inspectPayload(
    const Protocol protocol,
    const QJsonObject& root)
{
    PayloadView result;
    if (protocol == Protocol::OpenAIResponses) {
        const QJsonArray input = root.value(QStringLiteral("input")).toArray();
        if (input.isEmpty()) {
            return result;
        }
        const QJsonArray content = input.first()
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue& value : content) {
            const QJsonObject item = value.toObject();
            const QString type = item.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("input_text")) {
                result.prompt = item.value(QStringLiteral("text")).toString();
            } else if (type == QStringLiteral("input_image")) {
                result.imageDataUrl =
                    item.value(QStringLiteral("image_url")).toString();
            }
        }
        return result;
    }

    const QJsonArray messages = root.value(QStringLiteral("messages")).toArray();
    if (messages.isEmpty()) {
        return result;
    }
    const QJsonValue contentValue = messages.first()
                                        .toObject()
                                        .value(QStringLiteral("content"));
    if (contentValue.isString()) {
        result.prompt = contentValue.toString();
        return result;
    }
    for (const QJsonValue& value : contentValue.toArray()) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("text")) {
            result.prompt = item.value(QStringLiteral("text")).toString();
        } else if (type == QStringLiteral("image_url")) {
            result.imageDataUrl = item.value(QStringLiteral("image_url"))
                                      .toObject()
                                      .value(QStringLiteral("url"))
                                      .toString();
        }
    }
    return result;
}

[[nodiscard]] quint32 bigEndian32(const QByteArray& bytes, const qsizetype offset)
{
    if (offset < 0 || bytes.size() - offset < 4) {
        return 0;
    }
    const auto byteAt = [&bytes](const qsizetype index) {
        return static_cast<quint32>(
            static_cast<unsigned char>(bytes.at(index)));
    };
    return (byteAt(offset) << 24U) | (byteAt(offset + 1) << 16U)
        | (byteAt(offset + 2) << 8U) | byteAt(offset + 3);
}

}  // namespace

class M4ProviderProbeNetworkTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void modelsGetAndDeduplicate_data();
    void modelsGetAndDeduplicate();
    void credentialEchoedAsModelIdIsRejected();
    void fixedTextAndImagePostsSucceed_data();
    void fixedTextAndImagePostsSucceed();
    void sameOriginRedirectIsFollowed();
    void crossOriginRedirectIsRejectedWithoutCredentialLeak();
    void malformedAndOversizedResponsesAreRejected_data();
    void malformedAndOversizedResponsesAreRejected();
    void httpFailuresAreClassified_data();
    void httpFailuresAreClassified();
    void cancelDisconnectsAndDropsLateResponse();
    void timeoutAbortsTheProbe();
};

void M4ProviderProbeNetworkTests::initTestCase()
{
    qRegisterMetaType<ProviderProbeResult>();
}

void M4ProviderProbeNetworkTests::modelsGetAndDeduplicate_data()
{
    QTest::addColumn<int>("protocol");
    QTest::newRow("responses") << static_cast<int>(Protocol::OpenAIResponses);
    QTest::newRow("chat-completions")
        << static_cast<int>(Protocol::ChatCompletions);
}

void M4ProviderProbeNetworkTests::modelsGetAndDeduplicate()
{
    QFETCH(int, protocol);
    const Protocol protocolValue = static_cast<Protocol>(protocol);

    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([](const CapturedRequest&) {
        ResponsePlan plan;
        plan.headers.append({
            QByteArrayLiteral("Content-Type"),
            QByteArrayLiteral("application/json"),
        });
        plan.body = modelListResponse();
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile = profileFor(server, protocolValue);
    const QUuid operationId = startProbe(
        &client,
        profile,
        ProviderProbeOperation::ModelList,
        QStringLiteral("model-list-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'000);
    QCOMPARE(server.requestCount(), 1);
    const CapturedRequest& request = server.requestAt(0);
    QCOMPARE(request.method, QByteArrayLiteral("GET"));
    QCOMPARE(request.target, QByteArrayLiteral("/v1/models"));
    QVERIFY(request.body.isEmpty());
    QCOMPARE(
        request.headers.value(QByteArrayLiteral("authorization")),
        QByteArrayLiteral("Bearer model-list-secret"));

    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QCOMPARE(result.providerProfileId, profile.id);
    QCOMPARE(result.operation, ProviderProbeOperation::ModelList);
    QVERIFY(result.success);
    QCOMPARE(result.errorKind, ErrorKind::None);
    QCOMPARE(
        result.modelIds,
        QStringList({QStringLiteral("model-a"), QStringLiteral("model-b")}));
    QCOMPARE(result.modelIds.count(QStringLiteral("model-a")), 1);
    QVERIFY(!client.isActive(operationId));
}

void M4ProviderProbeNetworkTests::credentialEchoedAsModelIdIsRejected()
{
    const QString credential =
        QStringLiteral("hf_probe_credential_must_not_be_persisted");

    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([credential](const CapturedRequest&) {
        ResponsePlan plan;
        plan.headers.append({
            QByteArrayLiteral("Content-Type"),
            QByteArrayLiteral("application/json"),
        });
        plan.body = QJsonDocument(QJsonObject{
            {QStringLiteral("data"), QJsonArray{
                 QJsonObject{{QStringLiteral("id"), credential}},
                 QJsonObject{{
                     QStringLiteral("id"),
                     QStringLiteral("prefix-%1-suffix").arg(credential),
                 }},
             }},
        }).toJson(QJsonDocument::Compact);
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile =
        profileFor(server, Protocol::OpenAIResponses);
    const QUuid operationId = client.fetchModels(profile, QString(credential));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'000);
    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, ErrorKind::InvalidResponse);
    QVERIFY(result.modelIds.isEmpty());
    QVERIFY(!result.message.contains(credential));
    QVERIFY(!client.isActive(operationId));
}

void M4ProviderProbeNetworkTests::fixedTextAndImagePostsSucceed_data()
{
    QTest::addColumn<int>("protocol");
    QTest::addColumn<int>("operation");
    QTest::addColumn<QByteArray>("expectedTarget");

    for (const auto protocol : {
             Protocol::OpenAIResponses,
             Protocol::ChatCompletions}) {
        const QByteArray protocolName = protocol == Protocol::OpenAIResponses
            ? QByteArrayLiteral("responses")
            : QByteArrayLiteral("chat");
        const QByteArray target = protocol == Protocol::OpenAIResponses
            ? QByteArrayLiteral("/v1/responses")
            : QByteArrayLiteral("/v1/chat/completions");
        QTest::newRow((protocolName + QByteArrayLiteral("-text")).constData())
            << static_cast<int>(protocol)
            << static_cast<int>(ProviderProbeOperation::TextConnection)
            << target;
        QTest::newRow((protocolName + QByteArrayLiteral("-image")).constData())
            << static_cast<int>(protocol)
            << static_cast<int>(ProviderProbeOperation::ImageUnderstanding)
            << target;
    }
}

void M4ProviderProbeNetworkTests::fixedTextAndImagePostsSucceed()
{
    QFETCH(int, protocol);
    QFETCH(int, operation);
    QFETCH(QByteArray, expectedTarget);
    const Protocol protocolValue = static_cast<Protocol>(protocol);
    const ProviderProbeOperation operationValue =
        static_cast<ProviderProbeOperation>(operation);

    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([protocolValue, operationValue](const CapturedRequest&) {
        ResponsePlan plan;
        plan.headers.append({
            QByteArrayLiteral("Content-Type"),
            QByteArrayLiteral("application/json"),
        });
        plan.body = probeResponse(protocolValue, operationValue);
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile = profileFor(server, protocolValue);
    const QUuid operationId = startProbe(
        &client,
        profile,
        operationValue,
        QStringLiteral("post-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'000);
    QCOMPARE(server.requestCount(), 1);
    const CapturedRequest& request = server.requestAt(0);
    QCOMPARE(request.method, QByteArrayLiteral("POST"));
    QCOMPARE(request.target, expectedTarget);
    QCOMPARE(
        request.headers.value(QByteArrayLiteral("authorization")),
        QByteArrayLiteral("Bearer post-secret"));
    QCOMPARE(
        request.headers.value(QByteArrayLiteral("content-type")),
        QByteArrayLiteral("application/json"));
    QVERIFY(!request.body.contains(userQuestionSentinel));
    QVERIFY(!request.body.contains(userScreenshotSentinel));
    QVERIFY(!request.body.contains("PRIVATE_MODEL_HISTORY"));

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(request.body, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const QJsonObject root = document.object();
    QCOMPARE(
        root.value(QStringLiteral("model")).toString(),
        QStringLiteral("fixed-probe-model"));
    QCOMPARE(root.value(QStringLiteral("stream")).toBool(true), false);
    if (protocolValue == Protocol::OpenAIResponses) {
        QVERIFY(root.contains(QStringLiteral("store")));
        QCOMPARE(root.value(QStringLiteral("store")).toBool(true), false);
    } else {
        QVERIFY(!root.contains(QStringLiteral("store")));
    }

    const PayloadView payload = inspectPayload(protocolValue, root);
    if (operationValue == ProviderProbeOperation::TextConnection) {
        QVERIFY(payload.prompt.contains(QStringLiteral("SNAPASK_TEXT_OK")));
        QVERIFY(payload.imageDataUrl.isEmpty());
        QVERIFY(!request.body.contains("data:image"));
    } else {
        QVERIFY(payload.prompt.contains(QStringLiteral("SNAPASK_IMAGE_RED")));
        const QString prefix = QStringLiteral("data:image/png;base64,");
        QVERIFY(payload.imageDataUrl.startsWith(prefix));
        QCOMPARE(request.body.count("data:image/png;base64,"), 1);
        const QByteArray png = QByteArray::fromBase64(
            payload.imageDataUrl.sliced(prefix.size()).toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        QVERIFY(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
        QCOMPARE(bigEndian32(png, 16), quint32{256});
        QCOMPARE(bigEndian32(png, 20), quint32{256});
        QVERIFY(!QCryptographicHash::hash(png, QCryptographicHash::Sha256)
                     .isEmpty());
    }

    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QCOMPARE(result.operation, operationValue);
    QVERIFY(result.success);
    QCOMPARE(result.errorKind, ErrorKind::None);
    QVERIFY(!client.isActive(operationId));
}

void M4ProviderProbeNetworkTests::sameOriginRedirectIsFollowed()
{
    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([](const CapturedRequest& request) {
        ResponsePlan plan;
        if (request.target == QByteArrayLiteral("/v1/models")) {
            plan.statusCode = 302;
            plan.reasonPhrase = QByteArrayLiteral("Found");
            plan.headers.append({
                QByteArrayLiteral("Location"),
                QByteArrayLiteral("/v1/models-final"),
            });
            return plan;
        }
        plan.headers.append({
            QByteArrayLiteral("Content-Type"),
            QByteArrayLiteral("application/json"),
        });
        plan.body = modelListResponse();
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile =
        profileFor(server, Protocol::OpenAIResponses);
    const QUuid operationId = client.fetchModels(
        profile,
        QStringLiteral("same-origin-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'000);
    QCOMPARE(server.requestCount(), 2);
    QCOMPARE(
        server.requestAt(0).target,
        QByteArrayLiteral("/v1/models"));
    QCOMPARE(
        server.requestAt(1).target,
        QByteArrayLiteral("/v1/models-final"));
    for (qsizetype index = 0; index < server.requestCount(); ++index) {
        QCOMPARE(
            server.requestAt(index)
                .headers.value(QByteArrayLiteral("authorization")),
            QByteArrayLiteral("Bearer same-origin-secret"));
    }
    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QVERIFY(result.success);
    QCOMPARE(
        result.modelIds,
        QStringList({QStringLiteral("model-a"), QStringLiteral("model-b")}));
}

void M4ProviderProbeNetworkTests::crossOriginRedirectIsRejectedWithoutCredentialLeak()
{
    LocalHttpServer trap;
    QVERIFY(trap.listen());
    trap.setHandler([](const CapturedRequest&) {
        ResponsePlan plan;
        plan.body = modelListResponse();
        return plan;
    });

    LocalHttpServer origin;
    QVERIFY(origin.listen());
    const QByteArray redirectTarget =
        trap.urlFor(QStringLiteral("/credential-trap"))
            .toString(QUrl::FullyEncoded)
            .toLatin1();
    origin.setHandler([redirectTarget](const CapturedRequest&) {
        ResponsePlan plan;
        plan.statusCode = 302;
        plan.reasonPhrase = QByteArrayLiteral("Found");
        plan.headers.append({QByteArrayLiteral("Location"), redirectTarget});
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile =
        profileFor(origin, Protocol::OpenAIResponses);
    const QUuid operationId = client.fetchModels(
        profile,
        QStringLiteral("cross-origin-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'000);
    QTest::qWait(80);
    QCOMPARE(origin.requestCount(), 1);
    QCOMPARE(trap.requestCount(), 0);
    QCOMPARE(
        origin.requestAt(0)
            .headers.value(QByteArrayLiteral("authorization")),
        QByteArrayLiteral("Bearer cross-origin-secret"));

    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, ErrorKind::RedirectRejected);
    QCOMPARE(result.httpStatus, 302);
    QVERIFY(!result.message.contains(QStringLiteral("cross-origin-secret")));
    QVERIFY(!client.isActive(operationId));
}

void M4ProviderProbeNetworkTests::malformedAndOversizedResponsesAreRejected_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("malformed-json") << 0;
    QTest::newRow("declared-over-limit") << 1;
    QTest::newRow("streamed-over-limit") << 2;
}

void M4ProviderProbeNetworkTests::malformedAndOversizedResponsesAreRejected()
{
    QFETCH(int, mode);
    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([mode](const CapturedRequest&) {
        ResponsePlan plan;
        plan.headers.append({
            QByteArrayLiteral("Content-Type"),
            QByteArrayLiteral("application/json"),
        });
        if (mode == 0) {
            plan.body = QByteArrayLiteral("{not-json");
        } else if (mode == 1) {
            plan.contentLengthOverride = maximumTestResponseBytes + 1;
            plan.closeAfterWrite = false;
        } else {
            plan.body = QByteArray(maximumTestResponseBytes + 1, 'x');
            plan.omitContentLength = true;
        }
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile =
        profileFor(server, Protocol::OpenAIResponses);
    const QUuid operationId = client.testTextConnection(
        profile,
        QStringLiteral("invalid-response-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 3'000);
    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, ErrorKind::InvalidResponse);
    if (mode == 0) {
        QCOMPARE(result.httpStatus, 200);
    }
    QVERIFY(!client.isActive(operationId));
}

void M4ProviderProbeNetworkTests::httpFailuresAreClassified_data()
{
    QTest::addColumn<int>("statusCode");
    QTest::addColumn<int>("operation");
    QTest::addColumn<int>("expectedKind");

    QTest::newRow("401-authentication")
        << 401
        << static_cast<int>(ProviderProbeOperation::TextConnection)
        << static_cast<int>(ErrorKind::Authentication);
    QTest::newRow("429-rate-limited")
        << 429
        << static_cast<int>(ProviderProbeOperation::TextConnection)
        << static_cast<int>(ErrorKind::RateLimited);
    QTest::newRow("500-server")
        << 500
        << static_cast<int>(ProviderProbeOperation::TextConnection)
        << static_cast<int>(ErrorKind::Server);
    QTest::newRow("404-model-list-unsupported")
        << 404
        << static_cast<int>(ProviderProbeOperation::ModelList)
        << static_cast<int>(ErrorKind::UnsupportedCapability);
    QTest::newRow("400-image-unsupported")
        << 400
        << static_cast<int>(ProviderProbeOperation::ImageUnderstanding)
        << static_cast<int>(ErrorKind::UnsupportedCapability);
}

void M4ProviderProbeNetworkTests::httpFailuresAreClassified()
{
    QFETCH(int, statusCode);
    QFETCH(int, operation);
    QFETCH(int, expectedKind);
    const ProviderProbeOperation operationValue =
        static_cast<ProviderProbeOperation>(operation);

    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([statusCode](const CapturedRequest&) {
        ResponsePlan plan;
        plan.statusCode = statusCode;
        plan.reasonPhrase = QByteArrayLiteral("Synthetic failure");
        plan.body = QByteArray(rawProviderBodySentinel);
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile =
        profileFor(server, Protocol::OpenAIResponses);
    const QUuid operationId = startProbe(
        &client,
        profile,
        operationValue,
        QStringLiteral("http-error-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'000);
    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QCOMPARE(result.operation, operationValue);
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, static_cast<ErrorKind>(expectedKind));
    QCOMPARE(result.httpStatus, statusCode);
    QVERIFY(!result.message.contains(QString::fromLatin1(rawProviderBodySentinel)));
    QVERIFY(!result.message.contains(QStringLiteral("http-error-secret")));
    QVERIFY(!client.isActive(operationId));
}

void M4ProviderProbeNetworkTests::cancelDisconnectsAndDropsLateResponse()
{
    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([](const CapturedRequest&) {
        ResponsePlan plan;
        plan.headers.append({
            QByteArrayLiteral("Content-Type"),
            QByteArrayLiteral("application/json"),
        });
        plan.omitContentLength = true;
        plan.closeAfterWrite = false;
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    const ProviderProfile profile =
        profileFor(server, Protocol::OpenAIResponses);
    const QUuid operationId = client.testTextConnection(
        profile,
        QStringLiteral("cancel-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);
    QCOMPARE(resultSpy.size(), 0);
    QVERIFY(client.isActive(operationId));
    client.cancel(operationId);
    QCOMPARE(resultSpy.size(), 1);
    QVERIFY(!client.isActive(operationId));

    // Race a syntactically valid success response against abort(). The probe
    // has already been removed, so accepted kernel bytes must not publish a
    // second terminal result.
    (void)server.writeLateBody(probeResponse(
        Protocol::OpenAIResponses,
        ProviderProbeOperation::TextConnection));
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnectCount() >= 1, 2'000);
    QTest::qWait(100);
    QCOMPARE(resultSpy.size(), 1);
    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, ErrorKind::Cancelled);
    QVERIFY(!result.message.contains(QStringLiteral("cancel-secret")));

    client.cancel(operationId);
    QTest::qWait(20);
    QCOMPARE(resultSpy.size(), 1);
}

void M4ProviderProbeNetworkTests::timeoutAbortsTheProbe()
{
    LocalHttpServer server;
    QVERIFY(server.listen());
    server.setHandler([](const CapturedRequest&) {
        ResponsePlan plan;
        plan.sendHeaders = false;
        plan.closeAfterWrite = false;
        return plan;
    });

    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    ProviderProfile profile =
        profileFor(server, Protocol::OpenAIResponses);
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 1'500;
    const QUuid operationId = client.testTextConnection(
        profile,
        QStringLiteral("timeout-secret"));

    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.size(), 1, 2'500);
    const ProviderProbeResult result = firstResult(resultSpy);
    QCOMPARE(result.operationId, operationId);
    QVERIFY(!result.success);
    QCOMPARE(result.errorKind, ErrorKind::Timeout);
    QCOMPARE(result.httpStatus, 0);
    QVERIFY(!result.message.contains(QStringLiteral("timeout-secret")));
    QVERIFY(!client.isActive(operationId));
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnectCount() >= 1, 2'000);
    QTest::qWait(80);
    QCOMPARE(resultSpy.size(), 1);
}

QTEST_GUILESS_MAIN(M4ProviderProbeNetworkTests)
#include "M4ProviderProbeNetworkTests.moc"
