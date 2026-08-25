#include "ai/AiNetworkClient.h"
#include "ai/AiProfileRepository.h"
#include "app/AiSessionController.h"
#include "domain/annotation/Annotation.h"
#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"
#include "domain/conversation/ConversationSession.h"
#include "infrastructure/EndpointConsentStore.h"
#include "platform/windows/CredentialStore.h"
#include "services/SnapshotRenderer.h"
#include "ui/answer/AnswerCardWindow.h"
#include "ui/editor/EditorWindow.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Psapi.h>

#include <QAbstractSocket>
#include <QAbstractButton>
#include <QApplication>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QEvent>
#include <QComboBox>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QUndoStack>

#include <utility>
#include <algorithm>

using snapask::Annotation;
using snapask::ConversationTurn;
using snapask::RenderedSnapshot;
using snapask::ai::AiNetworkClient;
using snapask::ai::AiProfileRepository;
using snapask::ai::Capabilities;
using snapask::ai::ImageInput;
using snapask::ai::Protocol;
using snapask::ai::ProviderProfile;
using snapask::ai::Streaming;
using snapask::app::AiSessionController;
using snapask::infrastructure::EndpointConsentStore;
using snapask::platform::windows::CredentialStore;
using snapask::ui::answer::AnswerCardState;
using snapask::ui::answer::AnswerCardWindow;
using snapask::ui::editor::EditorWindow;

static_assert(sizeof(AiSessionController) > 0);

namespace {

struct CapturedRequest {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

class LocalSseServer final : public QObject {
public:
    enum class ResponseMode {
        Complete,
        HoldOpen,
    };

    explicit LocalSseServer(ResponseMode mode, QObject* parent = nullptr)
        : QObject(parent)
        , mode_(mode)
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

    [[nodiscard]] qsizetype requestCount() const noexcept
    {
        return requests_.size();
    }

    [[nodiscard]] int disconnectCount() const noexcept
    {
        return disconnectCount_;
    }

    [[nodiscard]] const CapturedRequest& lastRequest() const
    {
        return requests_.last();
    }

    void discardCapturedRequests()
    {
        requests_.clear();
    }

private:
    static QByteArray deltaEvent(const QString& text)
    {
        const QJsonObject object{
            {QStringLiteral("type"), QStringLiteral("response.output_text.delta")},
            {QStringLiteral("delta"), text},
        };
        return QByteArrayLiteral(
                   "event: response.output_text.delta\r\ndata: ")
            + QJsonDocument(object).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\r\n\r\n");
    }

    static QByteArray completedEvent()
    {
        const QJsonObject usage{
            {QStringLiteral("input_tokens"), 19},
            {QStringLiteral("output_tokens"), 11},
        };
        const QJsonObject response{{QStringLiteral("usage"), usage}};
        const QJsonObject object{
            {QStringLiteral("type"), QStringLiteral("response.completed")},
            {QStringLiteral("response"), response},
        };
        return QByteArrayLiteral("event: response.completed\r\ndata: ")
            + QJsonDocument(object).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\r\n\r\n");
    }

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
                handledSockets_.remove(socket);
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
        sendResponse(socket);
    }

    void sendResponse(QTcpSocket* socket)
    {
        socket->write(QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"));
        socket->write(deltaEvent(
            mode_ == ResponseMode::Complete
                ? QStringLiteral("结论：端到端回答已完成。")
                : QStringLiteral("取消前已收到的回答")));
        if (mode_ == ResponseMode::Complete) {
            socket->write(completedEvent());
        }
        socket->flush();

        if (mode_ == ResponseMode::Complete) {
            const QPointer<QTcpSocket> guardedSocket(socket);
            QTimer::singleShot(20, this, [guardedSocket]() {
                if (guardedSocket != nullptr) {
                    guardedSocket->disconnectFromHost();
                }
            });
        }
    }

    ResponseMode mode_;
    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QSet<QTcpSocket*> handledSockets_;
    QList<CapturedRequest> requests_;
    int disconnectCount_{0};
};

[[nodiscard]] QImage sourceImage()
{
    QImage image(QSize(47, 31), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(
                x,
                y,
                QColor(
                    (x * 13 + y * 7) % 256,
                    (x * 5 + y * 17) % 256,
                    (x * 19 + y * 3) % 256,
                    255));
        }
    }
    return image;
}

[[nodiscard]] ProviderProfile profileFor(
    const LocalSseServer& server,
    const QUuid& profileId)
{
    ProviderProfile profile;
    profile.id = profileId;
    profile.displayName = QStringLiteral("Local controller integration");
    profile.protocol = Protocol::OpenAIResponses;
    profile.baseUrl = server.baseUrl();
    profile.credentialRef = QStringLiteral("SnapAsk/provider/")
        + profileId.toString(QUuid::WithoutBraces);
    profile.modelId = QStringLiteral("local-vision-model");
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 4'000;
    profile.capabilities = Capabilities(ImageInput | Streaming);
    return profile;
}

[[nodiscard]] QPlainTextEdit* questionEditor(AnswerCardWindow* window)
{
    return window != nullptr
        ? window->findChild<QPlainTextEdit*>(QStringLiteral("answerQuestionEdit"))
        : nullptr;
}

[[nodiscard]] QPushButton* answerButton(
    AnswerCardWindow* window,
    const QString& objectName)
{
    return window != nullptr
        ? window->findChild<QPushButton*>(objectName)
        : nullptr;
}

[[nodiscard]] QByteArray uploadedPng(const CapturedRequest& request)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    const QJsonArray input = document.object().value(QStringLiteral("input")).toArray();
    if (input.isEmpty()) {
        return {};
    }
    const QJsonArray content = input.last()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    constexpr auto imagePrefix = "data:image/png;base64,";
    for (const QJsonValue& value : content) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString()
            != QStringLiteral("input_image")) {
            continue;
        }
        const QString imageUrl = object.value(QStringLiteral("image_url")).toString();
        if (!imageUrl.startsWith(QString::fromLatin1(imagePrefix))) {
            return {};
        }
        return QByteArray::fromBase64(
            imageUrl.sliced(static_cast<qsizetype>(qstrlen(imagePrefix))).toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
    }
    return {};
}

[[nodiscard]] QString uploadedQuestion(const CapturedRequest& request)
{
    const QJsonDocument document = QJsonDocument::fromJson(request.body);
    const QJsonArray input = document.object().value(QStringLiteral("input")).toArray();
    if (input.isEmpty()) {
        return {};
    }
    const QJsonArray content = input.last()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue& value : content) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString()
            == QStringLiteral("input_text")) {
            return object.value(QStringLiteral("text")).toString();
        }
    }
    return {};
}

[[nodiscard]] quint64 processWorkingSetBytes()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    const BOOL succeeded = GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        static_cast<DWORD>(sizeof(counters)));
    return succeeded != FALSE
        ? static_cast<quint64>(counters.WorkingSetSize)
        : 0;
}

[[nodiscard]] quint64 medianWorkingSet(QList<quint64> samples)
{
    if (samples.isEmpty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    return samples.at(samples.size() / 2);
}

}  // namespace

class M3AiSessionControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void explicitCtrlEnterFreezesAndUploadsOneCanonicalSnapshot();
    void stopDisconnectsAndPreservesPartialAnswer();
    void quickServiceAndModelSelectionControlsNextTurn();
    void unapprovedCustomOriginCancelsBeforeNetwork();
    void oneHundredProductUiNetworkFlowsStayStable();
};

void M3AiSessionControllerTests::explicitCtrlEnterFreezesAndUploadsOneCanonicalSnapshot()
{
    LocalSseServer server(LocalSseServer::ResponseMode::Complete);
    QVERIFY(server.listen());

    QTemporaryDir temporaryConfig;
    QVERIFY(temporaryConfig.isValid());
    const QString configPath = temporaryConfig.filePath(QStringLiteral("providers.json"));
    const QUuid profileId = QUuid::createUuid();
    const ProviderProfile profile = profileFor(server, profileId);

    CredentialStore credentials;
    [[maybe_unused]] const auto removeCredential = qScopeGuard([&credentials, &profile]() {
        (void)credentials.remove(profile.credentialRef);
    });
    QString error;
    const QString secret = QStringLiteral("session-controller-test-%1")
                               .arg(QUuid::createUuid().toString(
                                   QUuid::WithoutBraces));
    QVERIFY2(credentials.write(profile.credentialRef, secret, &error), qPrintable(error));

    AiProfileRepository writableProfiles(configPath);
    QVERIFY2(writableProfiles.upsert(profile, &error), qPrintable(error));
    QVERIFY2(writableProfiles.setDefault(profile.id, &error), qPrintable(error));
    QVERIFY2(writableProfiles.save(&error), qPrintable(error));
    AiProfileRepository profiles(configPath);
    QVERIFY2(profiles.load(&error), qPrintable(error));
    EndpointConsentStore endpointConsent(
        temporaryConfig.filePath(QStringLiteral("consent.ini")));
    QVERIFY(endpointConsent.approve(profile.baseUrl));

    EditorWindow editor(sourceImage());
    editor.setAttribute(Qt::WA_DeleteOnClose, false);
    AiNetworkClient networkClient;
    AiSessionController controller(
        &editor, &networkClient, &profiles, &credentials, &endpointConsent);
    AnswerCardWindow* answerWindow = controller.answerWindow();
    QVERIFY(answerWindow != nullptr);

    // Construction, showing the real editor, displaying the preview, and
    // editing both the local annotation model and question are all pre-send
    // operations and must remain network-silent.
    QTest::qWait(50);
    QCOMPARE(server.requestCount(), 0);
    editor.show();
    QTest::qWait(30);
    QCOMPARE(server.requestCount(), 0);
    controller.showQuestionCard();
    QTest::qWait(40);
    QCOMPARE(server.requestCount(), 0);
    QVERIFY(answerWindow->isVisible());
    QCOMPARE(answerWindow->targetDomain(), QStringLiteral("127.0.0.1"));

    const Annotation rectangle = Annotation::makeRectangle(
        QRectF(5.0, 4.0, 26.0, 17.0));
    editor.session().undoStack().push(new snapask::AddAnnotationCommand(
        &editor.session().annotations(), rectangle));
    controller.showQuestionCard();

    const QString question =
        QStringLiteral("请先给结论。\n第二行问题必须原样发送。");
    answerWindow->setQuestion(question);
    QTest::qWait(60);
    QCOMPARE(server.requestCount(), 0);

    const RenderedSnapshot& expectedSnapshot =
        editor.currentRenderedSnapshot();
    QVERIFY(expectedSnapshot.isValid());
    const RenderedSnapshot* const canonicalAddress = &expectedSnapshot;
    QCOMPARE(&editor.currentRenderedSnapshot(), canonicalAddress);
    QCOMPARE(answerWindow->pendingSnapshotPreview(), expectedSnapshot.image());

    QPlainTextEdit* input = questionEditor(answerWindow);
    QVERIFY(input != nullptr);
    input->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(&editor.currentRenderedSnapshot(), canonicalAddress);

    QVERIFY2(
        answerWindow->state() != AnswerCardState::Failed,
        qPrintable(answerWindow->errorMessage()));
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);

    const CapturedRequest& request = server.lastRequest();
    QCOMPARE(request.method, QByteArrayLiteral("POST"));
    QCOMPARE(request.target, QByteArrayLiteral("/v1/responses"));
    QCOMPARE(uploadedQuestion(request), question);
    const QByteArray png = uploadedPng(request);
    QVERIFY(!png.isEmpty());
    QCOMPARE(png, expectedSnapshot.pngBytes());
    QCOMPARE(
        QCryptographicHash::hash(png, QCryptographicHash::Sha256),
        expectedSnapshot.sha256());

    QTRY_COMPARE_WITH_TIMEOUT(
        answerWindow->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(
        answerWindow->answerMarkdown(),
        QStringLiteral("结论：端到端回答已完成。"));
    QCOMPARE(answerWindow->question(), question);
    QCOMPARE(server.requestCount(), 1);

    QCOMPARE(controller.conversation().revisionCount(), 1);
    QCOMPARE(controller.conversation().turnCount(), 1);
    const ConversationTurn* turn = controller.conversation().turnAt(0);
    QVERIFY(turn != nullptr);
    QCOMPARE(turn->status(), ConversationTurn::Status::Completed);
    QCOMPARE(turn->answer(), QStringLiteral("结论：端到端回答已完成。"));
    QCOMPARE(turn->question(), question);
    QCOMPARE(
        turn->snapshotRevision()->renderedSnapshot().pngBytes(),
        expectedSnapshot.pngBytes());
    QCOMPARE(
        turn->snapshotRevision()->renderedHash(),
        expectedSnapshot.sha256());
    QVERIFY(!networkClient.isActive(turn->requestId()));

    QTest::qWait(80);
    QCOMPARE(server.requestCount(), 1);
}

void M3AiSessionControllerTests::stopDisconnectsAndPreservesPartialAnswer()
{
    LocalSseServer server(LocalSseServer::ResponseMode::HoldOpen);
    QVERIFY(server.listen());

    QTemporaryDir temporaryConfig;
    QVERIFY(temporaryConfig.isValid());
    const QString configPath = temporaryConfig.filePath(QStringLiteral("providers.json"));
    const QUuid profileId = QUuid::createUuid();
    const ProviderProfile profile = profileFor(server, profileId);

    CredentialStore credentials;
    [[maybe_unused]] const auto removeCredential = qScopeGuard([&credentials, &profile]() {
        (void)credentials.remove(profile.credentialRef);
    });
    QString error;
    const QString secret = QStringLiteral("session-controller-cancel-test-%1")
                               .arg(QUuid::createUuid().toString(
                                   QUuid::WithoutBraces));
    QVERIFY2(credentials.write(profile.credentialRef, secret, &error), qPrintable(error));

    AiProfileRepository profiles(configPath);
    QVERIFY2(profiles.upsert(profile, &error), qPrintable(error));
    QVERIFY2(profiles.setDefault(profile.id, &error), qPrintable(error));
    QVERIFY2(profiles.save(&error), qPrintable(error));
    EndpointConsentStore endpointConsent(
        temporaryConfig.filePath(QStringLiteral("consent.ini")));
    QVERIFY(endpointConsent.approve(profile.baseUrl));

    EditorWindow editor(sourceImage());
    editor.setAttribute(Qt::WA_DeleteOnClose, false);
    AiNetworkClient networkClient;
    AiSessionController controller(
        &editor, &networkClient, &profiles, &credentials, &endpointConsent);
    AnswerCardWindow* answerWindow = controller.answerWindow();
    QVERIFY(answerWindow != nullptr);
    controller.showQuestionCard();
    answerWindow->setQuestion(QStringLiteral("开始后立即测试停止"));

    QPlainTextEdit* input = questionEditor(answerWindow);
    QVERIFY(input != nullptr);
    input->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);

    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        answerWindow->answerMarkdown(),
        QStringLiteral("取消前已收到的回答"),
        2'000);
    QCOMPARE(controller.conversation().turnCount(), 1);
    const ConversationTurn* turn = controller.conversation().turnAt(0);
    QVERIFY(turn != nullptr);
    const QUuid requestId = turn->requestId();
    QVERIFY(networkClient.isActive(requestId));

    QPushButton* stopButton = answerButton(
        answerWindow, QStringLiteral("answerStopButton"));
    QVERIFY(stopButton != nullptr);
    QVERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);

    QTRY_COMPARE_WITH_TIMEOUT(
        answerWindow->state(), AnswerCardState::Cancelled, 1'000);
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnectCount() >= 1, 2'000);
    QVERIFY(!networkClient.isActive(requestId));
    QCOMPARE(answerWindow->answerMarkdown(), QStringLiteral("取消前已收到的回答"));

    turn = controller.conversation().turnForRequest(requestId);
    QVERIFY(turn != nullptr);
    QCOMPARE(turn->status(), ConversationTurn::Status::Cancelled);
    QCOMPARE(turn->answer(), QStringLiteral("取消前已收到的回答"));
    QCOMPARE(server.requestCount(), 1);
}

void M3AiSessionControllerTests::quickServiceAndModelSelectionControlsNextTurn()
{
    LocalSseServer firstServer(LocalSseServer::ResponseMode::Complete);
    LocalSseServer secondServer(LocalSseServer::ResponseMode::Complete);
    QVERIFY(firstServer.listen());
    QVERIFY(secondServer.listen());

    QTemporaryDir temporaryConfig;
    QVERIFY(temporaryConfig.isValid());
    ProviderProfile first = profileFor(firstServer, QUuid::createUuid());
    first.displayName = QStringLiteral("First Responses");
    first.modelId = QStringLiteral("first-default");
    first.availableModels = {first.modelId};
    ProviderProfile second = profileFor(secondServer, QUuid::createUuid());
    second.displayName = QStringLiteral("Second Responses");
    second.modelId = QStringLiteral("second-default");
    second.availableModels = {second.modelId, QStringLiteral("second-manual")};

    CredentialStore credentials;
    [[maybe_unused]] const auto cleanup = qScopeGuard([&] {
        (void)credentials.remove(first.credentialRef);
        (void)credentials.remove(second.credentialRef);
    });
    QString error;
    QVERIFY2(credentials.write(first.credentialRef, QStringLiteral("first-key"), &error),
             qPrintable(error));
    QVERIFY2(credentials.write(second.credentialRef, QStringLiteral("second-key"), &error),
             qPrintable(error));

    AiProfileRepository profiles(
        temporaryConfig.filePath(QStringLiteral("providers.json")));
    QVERIFY2(profiles.upsert(first, &error), qPrintable(error));
    QVERIFY2(profiles.upsert(second, &error), qPrintable(error));
    QVERIFY2(profiles.setDefault(first.id, &error), qPrintable(error));
    EndpointConsentStore endpointConsent(
        temporaryConfig.filePath(QStringLiteral("consent.ini")));
    QVERIFY(endpointConsent.approve(first.baseUrl));
    QVERIFY(endpointConsent.approve(second.baseUrl));

    EditorWindow editor(sourceImage());
    editor.setAttribute(Qt::WA_DeleteOnClose, false);
    AiNetworkClient networkClient;
    AiSessionController controller(
        &editor, &networkClient, &profiles, &credentials, &endpointConsent);
    AnswerCardWindow* answerWindow = controller.answerWindow();
    QVERIFY(answerWindow != nullptr);
    controller.showQuestionCard();

    auto* serviceCombo = answerWindow->findChild<QComboBox*>(
        QStringLiteral("answerServiceCombo"));
    auto* modelCombo = answerWindow->findChild<QComboBox*>(
        QStringLiteral("answerModelCombo"));
    QVERIFY(serviceCombo != nullptr);
    QVERIFY(modelCombo != nullptr);
    const int secondIndex = serviceCombo->findData(second.id);
    QVERIFY(secondIndex >= 0);
    serviceCombo->setCurrentIndex(secondIndex);
    modelCombo->setEditText(QStringLiteral("second-manual"));
    answerWindow->setQuestion(QStringLiteral("发送到第二个服务"));
    QPushButton* sendButton = answerButton(
        answerWindow, QStringLiteral("answerSendButton"));
    QVERIFY(sendButton != nullptr);
    QTest::mouseClick(sendButton, Qt::LeftButton);

    QTRY_COMPARE_WITH_TIMEOUT(secondServer.requestCount(), 1, 2'000);
    QCOMPARE(firstServer.requestCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(
        answerWindow->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(
        QJsonDocument::fromJson(secondServer.lastRequest().body)
            .object().value(QStringLiteral("model")).toString(),
        QStringLiteral("second-manual"));
    const ConversationTurn* firstTurn = controller.conversation().turnAt(0);
    QVERIFY(firstTurn != nullptr);
    QCOMPARE(firstTurn->providerProfileId(), second.id);
    QCOMPARE(firstTurn->modelId(), QStringLiteral("second-manual"));

    const int firstIndex = serviceCombo->findData(first.id);
    QVERIFY(firstIndex >= 0);
    serviceCombo->setCurrentIndex(firstIndex);
    modelCombo->setEditText(QStringLiteral("first-manual"));
    answerWindow->setQuestion(QStringLiteral("下一轮发送到第一个服务"));
    QTest::mouseClick(sendButton, Qt::LeftButton);

    QTRY_COMPARE_WITH_TIMEOUT(firstServer.requestCount(), 1, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        answerWindow->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(secondServer.requestCount(), 1);
    QCOMPARE(
        QJsonDocument::fromJson(firstServer.lastRequest().body)
            .object().value(QStringLiteral("model")).toString(),
        QStringLiteral("first-manual"));
    const ConversationTurn* secondTurn = controller.conversation().turnAt(1);
    QVERIFY(secondTurn != nullptr);
    QCOMPARE(secondTurn->providerProfileId(), first.id);
    QCOMPARE(secondTurn->modelId(), QStringLiteral("first-manual"));
}

void M3AiSessionControllerTests::unapprovedCustomOriginCancelsBeforeNetwork()
{
    LocalSseServer server(LocalSseServer::ResponseMode::Complete);
    QVERIFY(server.listen());
    QTemporaryDir temporaryConfig;
    QVERIFY(temporaryConfig.isValid());
    const ProviderProfile profile = profileFor(server, QUuid::createUuid());

    CredentialStore credentials;
    [[maybe_unused]] const auto cleanup = qScopeGuard([&] {
        (void)credentials.remove(profile.credentialRef);
    });
    QString error;
    QVERIFY2(credentials.write(
                 profile.credentialRef, QStringLiteral("consent-test-key"), &error),
             qPrintable(error));
    AiProfileRepository profiles(
        temporaryConfig.filePath(QStringLiteral("providers.json")));
    QVERIFY2(profiles.upsert(profile, &error), qPrintable(error));
    QVERIFY2(profiles.setDefault(profile.id, &error), qPrintable(error));
    EndpointConsentStore endpointConsent(
        temporaryConfig.filePath(QStringLiteral("consent.ini")));
    QVERIFY(!endpointConsent.isApproved(profile.baseUrl));

    EditorWindow editor(sourceImage());
    editor.setAttribute(Qt::WA_DeleteOnClose, false);
    AiNetworkClient networkClient;
    AiSessionController controller(
        &editor, &networkClient, &profiles, &credentials, &endpointConsent);
    AnswerCardWindow* answerWindow = controller.answerWindow();
    QVERIFY(answerWindow != nullptr);
    controller.showQuestionCard();
    answerWindow->setQuestion(QStringLiteral("拒绝自定义域名后不能发送"));

    QTimer::singleShot(0, this, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* messageBox = qobject_cast<QMessageBox*>(widget);
            if (messageBox != nullptr && messageBox->isVisible()) {
                if (QAbstractButton* cancel = messageBox->button(QMessageBox::Cancel)) {
                    cancel->click();
                }
            }
        }
    });
    QPushButton* sendButton = answerButton(
        answerWindow, QStringLiteral("answerSendButton"));
    QVERIFY(sendButton != nullptr);
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QTest::qWait(120);

    QCOMPARE(server.requestCount(), 0);
    QCOMPARE(controller.conversation().turnCount(), 0);
    QCOMPARE(answerWindow->state(), AnswerCardState::Idle);
    QVERIFY(!endpointConsent.isApproved(profile.baseUrl));
    QVERIFY(credentials.contains(profile.credentialRef));
}

void M3AiSessionControllerTests::oneHundredProductUiNetworkFlowsStayStable()
{
    constexpr int flowCount = 100;
    constexpr quint64 mebibyte = 1024ULL * 1024ULL;

    LocalSseServer server(LocalSseServer::ResponseMode::Complete);
    QVERIFY(server.listen());
    QTemporaryDir temporaryConfig;
    QVERIFY(temporaryConfig.isValid());
    const ProviderProfile profile = profileFor(server, QUuid::createUuid());

    CredentialStore credentials;
    [[maybe_unused]] const auto cleanupCredential = qScopeGuard([&] {
        (void)credentials.remove(profile.credentialRef);
    });
    QString error;
    QVERIFY2(
        credentials.write(
            profile.credentialRef,
            QStringLiteral("product-stress-%1").arg(
                QUuid::createUuid().toString(QUuid::WithoutBraces)),
            &error),
        qPrintable(error));

    AiProfileRepository profiles(
        temporaryConfig.filePath(QStringLiteral("providers.json")));
    QVERIFY2(profiles.upsert(profile, &error), qPrintable(error));
    QVERIFY2(profiles.setDefault(profile.id, &error), qPrintable(error));
    EndpointConsentStore endpointConsent(
        temporaryConfig.filePath(QStringLiteral("consent.ini")));
    QVERIFY(endpointConsent.approve(profile.baseUrl));
    AiNetworkClient networkClient;

    const quint64 startingWorkingSet = processWorkingSetBytes();
    QVERIFY(startingWorkingSet > 0);
    quint64 peakWorkingSet = startingWorkingSet;
    QList<quint64> releasedWorkingSets;
    releasedWorkingSets.reserve(flowCount);

    for (int iteration = 0; iteration < flowCount; ++iteration) {
        {
            EditorWindow editor(sourceImage());
            editor.setAttribute(Qt::WA_DeleteOnClose, false);
            AiSessionController controller(
                &editor,
                &networkClient,
                &profiles,
                &credentials,
                &endpointConsent);
            editor.session().undoStack().push(new snapask::AddAnnotationCommand(
                &editor.session().annotations(),
                Annotation::makeRectangle(
                    QRectF(3.0 + (iteration % 5), 3.0, 22.0, 14.0))));

            QCOMPARE(server.requestCount(), 0);
            controller.showQuestionCard();
            AnswerCardWindow* answerWindow = controller.answerWindow();
            QVERIFY(answerWindow != nullptr);
            const RenderedSnapshot& canonical =
                editor.currentRenderedSnapshot();
            QVERIFY(canonical.isValid());
            const QByteArray expectedPng = canonical.pngBytes();
            const QByteArray expectedHash = canonical.sha256();
            answerWindow->setQuestion(
                QStringLiteral("第 %1 次真实 UI 网络流程").arg(iteration + 1));

            QPlainTextEdit* input = questionEditor(answerWindow);
            QVERIFY(input != nullptr);
            input->setFocus(Qt::OtherFocusReason);
            QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
            QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);
            QTRY_COMPARE_WITH_TIMEOUT(
                answerWindow->state(), AnswerCardState::Completed, 2'000);

            const QByteArray actualPng = uploadedPng(server.lastRequest());
            QCOMPARE(actualPng, expectedPng);
            QCOMPARE(
                QCryptographicHash::hash(
                    actualPng, QCryptographicHash::Sha256),
                expectedHash);
            QCOMPARE(controller.conversation().revisionCount(), 1);
            QCOMPARE(controller.conversation().turnCount(), 1);
            QVERIFY(!networkClient.isActive(
                controller.conversation().turnAt(0)->requestId()));

            editor.setWindowModified(false);
            editor.close();
        }

        server.discardCapturedRequests();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        const quint64 workingSet = processWorkingSetBytes();
        QVERIFY(workingSet > 0);
        peakWorkingSet = std::max(peakWorkingSet, workingSet);
        releasedWorkingSets.append(workingSet);
    }

    const quint64 endingWorkingSet = processWorkingSetBytes();
    const quint64 earlyMedian =
        medianWorkingSet(releasedWorkingSets.mid(10, 20));
    const quint64 lateMedian =
        medianWorkingSet(releasedWorkingSets.mid(flowCount - 20, 20));
    const quint64 allowedGrowth = std::max(
        32ULL * mebibyte,
        earlyMedian / 3ULL);
    qInfo().noquote()
        << QStringLiteral(
               "M6_PRODUCT_STRESS flows=%1 start_mib=%2 peak_mib=%3 "
               "end_mib=%4 early_median_mib=%5 late_median_mib=%6")
               .arg(flowCount)
               .arg(static_cast<double>(startingWorkingSet) / mebibyte,
                    0, 'f', 1)
               .arg(static_cast<double>(peakWorkingSet) / mebibyte,
                    0, 'f', 1)
               .arg(static_cast<double>(endingWorkingSet) / mebibyte,
                    0, 'f', 1)
               .arg(static_cast<double>(earlyMedian) / mebibyte,
                    0, 'f', 1)
               .arg(static_cast<double>(lateMedian) / mebibyte,
                    0, 'f', 1);
    QVERIFY2(
        lateMedian <= earlyMedian + allowedGrowth,
        "100 product UI/network flows showed sustained working-set growth");
    QVERIFY2(
        endingWorkingSet <= startingWorkingSet + allowedGrowth,
        "working set did not recover after 100 product UI/network flows");
}

QTEST_MAIN(M3AiSessionControllerTests)
#include "M3AiSessionControllerTests.moc"
