#include "ai/AiNetworkClient.h"
#include "domain/annotation/Annotation.h"
#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"
#include "services/SnapshotRenderer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <optional>
#include <utility>

using namespace snapask;
using namespace snapask::ai;

namespace {

struct CapturedHttpRequest {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct ResponsePlan {
    int statusCode{200};
    QByteArray reasonPhrase{QByteArrayLiteral("OK")};
    QList<QByteArray> chunks;
    int chunkIntervalMs{4};
    bool sendHeaders{true};
    bool closeAfterChunks{true};
    bool abortAfterChunks{false};
};

class FakeHttpServer final : public QObject {
    Q_OBJECT

public:
    explicit FakeHttpServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection,
                this, &FakeHttpServer::acceptConnections);
    }

    bool listen()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1")
                        .arg(server_.serverPort()));
    }

    void setResponsePlan(ResponsePlan plan)
    {
        plan_ = std::move(plan);
    }

    int requestCount() const noexcept
    {
        return requests_.size();
    }

    int disconnectCount() const noexcept
    {
        return disconnectCount_;
    }

    const CapturedHttpRequest& lastRequest() const
    {
        return requests_.last();
    }

    bool writeLate(const QByteArray& bytes)
    {
        if (lastSocket_.isNull()) {
            return false;
        }
        const qint64 accepted = lastSocket_->write(bytes);
        lastSocket_->flush();
        return accepted == bytes.size();
    }

signals:
    void requestReceived();
    void peerDisconnected();

private:
    void acceptConnections()
    {
        while (server_.hasPendingConnections()) {
            auto* socket = server_.nextPendingConnection();
            socket->setParent(this);
            inputBuffers_.insert(socket, {});
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                readRequest(socket);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                ++disconnectCount_;
                inputBuffers_.remove(socket);
                handledSockets_.remove(socket);
                emit peerDisconnected();
                socket->deleteLater();
            });
        }
    }

    void readRequest(QTcpSocket* socket)
    {
        if (handledSockets_.contains(socket)) {
            (void)socket->readAll();
            return;
        }

        auto& bytes = inputBuffers_[socket];
        bytes.append(socket->readAll());
        const qsizetype headerEnd = bytes.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        const QByteArray headerBlock = bytes.left(headerEnd);
        const QList<QByteArray> lines = headerBlock.split('\n');
        if (lines.isEmpty()) {
            return;
        }

        CapturedHttpRequest request;
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() >= 2) {
            request.method = requestLine.at(0);
            request.target = requestLine.at(1);
        }

        qsizetype contentLength = 0;
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const QByteArray line = lines.at(index).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            request.headers.insert(name, value);
            if (name == QByteArrayLiteral("content-length")) {
                bool ok = false;
                const qlonglong parsed = value.toLongLong(&ok);
                if (ok && parsed >= 0) {
                    contentLength = static_cast<qsizetype>(parsed);
                }
            }
        }

        const qsizetype bodyStart = headerEnd + 4;
        if (bytes.size() - bodyStart < contentLength) {
            return;
        }
        request.body = bytes.mid(bodyStart, contentLength);
        requests_.append(std::move(request));
        handledSockets_.insert(socket);
        lastSocket_ = socket;
        emit requestReceived();
        startResponse(socket);
    }

    void startResponse(QTcpSocket* socket)
    {
        if (!plan_.sendHeaders) {
            return;
        }

        QByteArray headers = QByteArrayLiteral("HTTP/1.1 ")
            + QByteArray::number(plan_.statusCode) + ' ' + plan_.reasonPhrase
            + QByteArrayLiteral("\r\nConnection: close\r\n");
        if (plan_.statusCode == 200) {
            headers += QByteArrayLiteral(
                "Content-Type: text/event-stream; charset=utf-8\r\n"
                "Cache-Control: no-cache\r\n\r\n");
        } else {
            headers += QByteArrayLiteral("Content-Length: 0\r\n\r\n");
        }
        socket->write(headers);
        socket->flush();

        const QPointer<QTcpSocket> guardedSocket(socket);
        int delay = 0;
        for (const QByteArray& chunk : plan_.chunks) {
            delay += plan_.chunkIntervalMs;
            QTimer::singleShot(delay, this, [guardedSocket, chunk] {
                if (guardedSocket.isNull()
                    || guardedSocket->state() == QAbstractSocket::UnconnectedState) {
                    return;
                }
                guardedSocket->write(chunk);
                guardedSocket->flush();
            });
        }

        if (plan_.closeAfterChunks) {
            QTimer::singleShot(delay + plan_.chunkIntervalMs, this,
                               [guardedSocket, abort = plan_.abortAfterChunks] {
                if (guardedSocket.isNull()) {
                    return;
                }
                if (abort) {
                    guardedSocket->abort();
                } else {
                    guardedSocket->disconnectFromHost();
                }
            });
        }
    }

    QTcpServer server_;
    ResponsePlan plan_;
    QHash<QTcpSocket*, QByteArray> inputBuffers_;
    QSet<QTcpSocket*> handledSockets_;
    QList<CapturedHttpRequest> requests_;
    QPointer<QTcpSocket> lastSocket_;
    int disconnectCount_{0};
};

QImage sourceImage()
{
    QImage image(QSize(31, 23), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(
                x, y,
                QColor((x * 17 + y * 3) % 256,
                       (x * 5 + y * 19) % 256,
                       (x * 11 + y * 7) % 256,
                       255));
        }
    }
    return image;
}

ProviderProfile profileFor(const FakeHttpServer& server)
{
    ProviderProfile profile;
    profile.id = QUuid::createUuid();
    profile.displayName = QStringLiteral("Local fake provider");
    profile.protocol = Protocol::OpenAIResponses;
    profile.baseUrl = server.baseUrl();
    profile.modelId = QStringLiteral("fake-vision-model");
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 2'000;
    profile.capabilities = Capabilities(ImageInput | Streaming);
    return profile;
}

AiRequest requestFor(const ScreenshotSession& session)
{
    AiRequest request;
    request.requestId = QUuid::createUuid();
    request.sessionId = session.sessionId();
    request.snapshotId = QUuid::createUuid();
    request.modelId = QStringLiteral("fake-vision-model");
    request.question = QStringLiteral("请解释图中标出的区域。\n第二行也必须保留。");
    return request;
}

QList<AiStreamEvent> eventsFrom(const QSignalSpy& spy)
{
    QList<AiStreamEvent> result;
    result.reserve(spy.size());
    for (const QList<QVariant>& arguments : spy) {
        if (!arguments.isEmpty()) {
            result.append(qvariant_cast<AiStreamEvent>(arguments.first()));
        }
    }
    return result;
}

bool hasEvent(const QSignalSpy& spy, EventType type)
{
    const auto events = eventsFrom(spy);
    for (const auto& event : events) {
        if (event.type == type) {
            return true;
        }
    }
    return false;
}

int eventCount(const QSignalSpy& spy, EventType type)
{
    int count = 0;
    const auto events = eventsFrom(spy);
    for (const auto& event : events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

QString concatenatedDeltas(const QSignalSpy& spy)
{
    QString result;
    const auto events = eventsFrom(spy);
    for (const auto& event : events) {
        if (event.type == EventType::TextDelta) {
            result += event.text;
        }
    }
    return result;
}

std::optional<AiStreamEvent> failureEvent(const QSignalSpy& spy)
{
    const auto events = eventsFrom(spy);
    for (auto it = events.crbegin(); it != events.crend(); ++it) {
        if (it->type == EventType::Failed) {
            return *it;
        }
    }
    return std::nullopt;
}

QByteArray deltaEvent(const QString& text)
{
    const QJsonObject object{
        {QStringLiteral("type"), QStringLiteral("response.output_text.delta")},
        {QStringLiteral("delta"), text},
    };
    return QByteArrayLiteral("event: response.output_text.delta\r\ndata: ")
        + QJsonDocument(object).toJson(QJsonDocument::Compact)
        + QByteArrayLiteral("\r\n\r\n");
}

QByteArray completedEvent()
{
    const QJsonObject usage{
        {QStringLiteral("input_tokens"), 13},
        {QStringLiteral("output_tokens"), 8},
    };
    const QJsonObject response{{QStringLiteral("usage"), usage}};
    const QJsonObject root{
        {QStringLiteral("type"), QStringLiteral("response.completed")},
        {QStringLiteral("response"), response},
    };
    return QByteArrayLiteral("event: response.completed\ndata: ")
        + QJsonDocument(root).toJson(QJsonDocument::Compact)
        + QByteArrayLiteral("\n\n");
}

} // namespace

class M3NetworkTests final : public QObject {
    Q_OBJECT

private slots:
    void constructionAndEditingDoNotUploadUntilExplicitSend();
    void fragmentedUtf8SseIsDecodedLosslessly();
    void cancelAbortsSocketAndDropsLateBytes();
    void httpStatusMapsToTypedFailure_data();
    void httpStatusMapsToTypedFailure();
    void timeoutMapsToTypedFailure();
    void generationTimeoutPreservesReceivedDelta();
    void midstreamDisconnectPreservesReceivedDelta();
    void malformedStreamMapsToInvalidResponse_data();
    void malformedStreamMapsToInvalidResponse();
};

void M3NetworkTests::constructionAndEditingDoNotUploadUntilExplicitSend()
{
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.setResponsePlan(ResponsePlan{
        200,
        QByteArrayLiteral("OK"),
        {QByteArrayLiteral("data: [DONE]\n\n")},
    });

    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    QSignalSpy requestSpy(&server, &FakeHttpServer::requestReceived);

    ScreenshotSession session(sourceImage());
    const Annotation rectangle = Annotation::makeRectangle(
        QRectF(3.0, 4.0, 17.0, 11.0));
    session.undoStack().push(
        new AddAnnotationCommand(&session.annotations(), rectangle));
    QCOMPARE(session.annotations().size(), 1);

    // Constructing the network client and performing local edits are never upload
    // boundaries. Give the event loop enough time to expose accidental eager I/O.
    QTest::qWait(80);
    QCOMPARE(server.requestCount(), 0);
    QCOMPARE(requestSpy.size(), 0);

    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    QVERIFY(snapshot.isValid());
    AiRequest request = requestFor(session);
    request.snapshotPng = QByteArrayLiteral("request-object-must-not-win");
    request.snapshotSha256 = QCryptographicHash::hash(
        request.snapshotPng, QCryptographicHash::Sha256);

    const ProviderProfile profile = profileFor(server);
    const QUuid returnedId = client.sendExplicit(
        request, snapshot, profile, QStringLiteral("local-test-key"));
    QCOMPARE(returnedId, request.requestId);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);
    QCOMPARE(requestSpy.size(), 1);

    const CapturedHttpRequest& captured = server.lastRequest();
    QCOMPARE(captured.method, QByteArrayLiteral("POST"));
    QCOMPARE(captured.target, QByteArrayLiteral("/v1/responses"));
    QCOMPARE(captured.headers.value(QByteArrayLiteral("content-type")),
             QByteArrayLiteral("application/json"));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(captured.body, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());

    const QJsonArray input = document.object().value(QStringLiteral("input")).toArray();
    QVERIFY(!input.isEmpty());
    const QJsonArray content = input.last().toObject()
                                   .value(QStringLiteral("content")).toArray();
    QString question;
    QString imageUrl;
    for (const QJsonValue& value : content) {
        const QJsonObject object = value.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("input_text")) {
            question = object.value(QStringLiteral("text")).toString();
        } else if (type == QStringLiteral("input_image")) {
            imageUrl = object.value(QStringLiteral("image_url")).toString();
        }
    }
    QCOMPARE(question, request.question);
    const QString prefix = QStringLiteral("data:image/png;base64,");
    QVERIFY(imageUrl.startsWith(prefix));
    const QByteArray uploadedPng = QByteArray::fromBase64(
        imageUrl.mid(prefix.size()).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    QCOMPARE(uploadedPng, snapshot.pngBytes());
    QCOMPARE(QCryptographicHash::hash(uploadedPng, QCryptographicHash::Sha256),
             snapshot.sha256());
    QVERIFY(uploadedPng != request.snapshotPng);

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Completed), 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(!client.isActive(request.requestId), 2'000);
    QCOMPARE(server.requestCount(), 1);
}

void M3NetworkTests::fragmentedUtf8SseIsDecodedLosslessly()
{
    FakeHttpServer server;
    QVERIFY(server.listen());
    const QString expected = QStringLiteral("你好，截图🙂");
    const QByteArray stream = deltaEvent(expected);
    const QByteArray needle = QStringLiteral("你").toUtf8();
    const qsizetype splitAt = stream.indexOf(needle);
    QVERIFY(splitAt > 0);
    server.setResponsePlan(ResponsePlan{
        200,
        QByteArrayLiteral("OK"),
        {
            stream.left(splitAt + 1),
            stream.mid(splitAt + 1, 1),
            stream.mid(splitAt + 2),
            completedEvent(),
        },
        5,
    });

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    const ProviderProfile profile = profileFor(server);
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Completed), 2'000);
    QCOMPARE(concatenatedDeltas(eventSpy), expected);
    QCOMPARE(eventCount(eventSpy, EventType::Failed), 0);
    QCOMPARE(eventCount(eventSpy, EventType::UsageUpdated), 1);
}

void M3NetworkTests::cancelAbortsSocketAndDropsLateBytes()
{
    FakeHttpServer server;
    QVERIFY(server.listen());
    ResponsePlan plan;
    plan.chunks = {deltaEvent(QStringLiteral("已收到"))};
    plan.closeAfterChunks = false;
    server.setResponsePlan(plan);

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    const ProviderProfile profile = profileFor(server);
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_COMPARE_WITH_TIMEOUT(concatenatedDeltas(eventSpy), QStringLiteral("已收到"), 2'000);
    QCOMPARE(server.disconnectCount(), 0);
    client.cancel(request.requestId);
    QCOMPARE(eventCount(eventSpy, EventType::Cancelled), 1);
    QVERIFY(!client.isActive(request.requestId));

    // Race a valid late event against the TCP abort. Whether the kernel accepts
    // this write or not, the removed request must never publish it.
    (void)server.writeLate(deltaEvent(QStringLiteral("绝不能出现")));
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnectCount() >= 1, 2'000);
    QTest::qWait(80);
    QCOMPARE(concatenatedDeltas(eventSpy), QStringLiteral("已收到"));
    QCOMPARE(eventCount(eventSpy, EventType::Cancelled), 1);
    QCOMPARE(eventCount(eventSpy, EventType::Completed), 0);
    QCOMPARE(eventCount(eventSpy, EventType::Failed), 0);
}

void M3NetworkTests::httpStatusMapsToTypedFailure_data()
{
    QTest::addColumn<int>("statusCode");
    QTest::addColumn<int>("expectedKind");
    QTest::newRow("401-authentication") << 401 << static_cast<int>(ErrorKind::Authentication);
    QTest::newRow("429-rate-limited") << 429 << static_cast<int>(ErrorKind::RateLimited);
    QTest::newRow("500-server") << 500 << static_cast<int>(ErrorKind::Server);
}

void M3NetworkTests::httpStatusMapsToTypedFailure()
{
    QFETCH(int, statusCode);
    QFETCH(int, expectedKind);
    FakeHttpServer server;
    QVERIFY(server.listen());
    ResponsePlan plan;
    plan.statusCode = statusCode;
    plan.reasonPhrase = QByteArrayLiteral("Synthetic failure");
    server.setResponsePlan(plan);

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    const ProviderProfile profile = profileFor(server);
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Failed), 2'000);
    const auto failure = failureEvent(eventSpy);
    QVERIFY(failure.has_value());
    QCOMPARE(static_cast<int>(failure->errorKind), expectedKind);
    QCOMPARE(failure->httpStatus, statusCode);
    QCOMPARE(eventCount(eventSpy, EventType::Completed), 0);
}

void M3NetworkTests::timeoutMapsToTypedFailure()
{
    FakeHttpServer server;
    QVERIFY(server.listen());
    ResponsePlan plan;
    plan.sendHeaders = false;
    plan.closeAfterChunks = false;
    server.setResponsePlan(plan);

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    ProviderProfile profile = profileFor(server);
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 1'400;
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Failed), 3'000);
    const auto failure = failureEvent(eventSpy);
    QVERIFY(failure.has_value());
    QCOMPARE(failure->errorKind, ErrorKind::Timeout);
    QCOMPARE(concatenatedDeltas(eventSpy), QString());
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnectCount() >= 1, 3'000);
}

void M3NetworkTests::generationTimeoutPreservesReceivedDelta()
{
    FakeHttpServer server;
    QVERIFY(server.listen());
    ResponsePlan plan;
    plan.chunks = {deltaEvent(QStringLiteral("超时前已收到"))};
    plan.closeAfterChunks = false;
    server.setResponsePlan(plan);

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    ProviderProfile profile = profileFor(server);
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 1'100;
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_COMPARE_WITH_TIMEOUT(
        concatenatedDeltas(eventSpy), QStringLiteral("超时前已收到"), 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Failed), 3'000);
    const auto failure = failureEvent(eventSpy);
    QVERIFY(failure.has_value());
    QCOMPARE(failure->errorKind, ErrorKind::Timeout);
    QCOMPARE(concatenatedDeltas(eventSpy), QStringLiteral("超时前已收到"));
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnectCount() >= 1, 3'000);
}

void M3NetworkTests::midstreamDisconnectPreservesReceivedDelta()
{
    FakeHttpServer server;
    QVERIFY(server.listen());
    ResponsePlan plan;
    plan.chunks = {deltaEvent(QStringLiteral("已经到达的前缀"))};
    plan.abortAfterChunks = true;
    server.setResponsePlan(plan);

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    const ProviderProfile profile = profileFor(server);
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Failed), 2'000);
    const auto failure = failureEvent(eventSpy);
    QVERIFY(failure.has_value());
    QVERIFY(failure->errorKind == ErrorKind::Network
            || failure->errorKind == ErrorKind::InvalidResponse);
    QCOMPARE(concatenatedDeltas(eventSpy), QStringLiteral("已经到达的前缀"));

    const auto events = eventsFrom(eventSpy);
    qsizetype deltaIndex = -1;
    qsizetype failureIndex = -1;
    for (qsizetype index = 0; index < events.size(); ++index) {
        if (events.at(index).type == EventType::TextDelta) {
            deltaIndex = index;
        } else if (events.at(index).type == EventType::Failed) {
            failureIndex = index;
        }
    }
    QVERIFY(deltaIndex >= 0);
    QVERIFY(failureIndex > deltaIndex);
}

void M3NetworkTests::malformedStreamMapsToInvalidResponse_data()
{
    QTest::addColumn<QByteArray>("streamBytes");
    QTest::newRow("malformed-json")
        << QByteArrayLiteral(
            "event: response.output_text.delta\n"
            "data: {definitely-not-json}\n\n");
    QTest::newRow("malformed-sse-without-data")
        << QByteArrayLiteral(
            "event: response.output_text.delta\n"
            "retry: not-an-integer\n"
            "unknown-field: ignored\n\n");
}

void M3NetworkTests::malformedStreamMapsToInvalidResponse()
{
    QFETCH(QByteArray, streamBytes);
    FakeHttpServer server;
    QVERIFY(server.listen());
    ResponsePlan plan;
    plan.chunks = {
        deltaEvent(QStringLiteral("错误前的有效片段")),
        streamBytes,
    };
    server.setResponsePlan(plan);

    ScreenshotSession session(sourceImage());
    const RenderedSnapshot snapshot = SnapshotRenderer::renderCurrent(session);
    AiRequest request = requestFor(session);
    AiNetworkClient client;
    QSignalSpy eventSpy(&client, &AiNetworkClient::eventReady);
    const ProviderProfile profile = profileFor(server);
    client.sendExplicit(request, snapshot, profile, QStringLiteral("local-test-key"));

    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(eventSpy, EventType::Failed), 2'000);
    const auto failure = failureEvent(eventSpy);
    QVERIFY(failure.has_value());
    QCOMPARE(failure->errorKind, ErrorKind::InvalidResponse);
    QCOMPARE(concatenatedDeltas(eventSpy), QStringLiteral("错误前的有效片段"));
    QCOMPARE(eventCount(eventSpy, EventType::Completed), 0);
}

QTEST_GUILESS_MAIN(M3NetworkTests)
#include "m3_network_tests.moc"
