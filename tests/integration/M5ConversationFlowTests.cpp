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

#include <QColor>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QUndoStack>

#include <utility>

using snapask::Annotation;
using snapask::ConversationTurn;
using snapask::RenderedSnapshot;
using snapask::SnapshotRenderer;
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

namespace {

struct CapturedRequest {
    QByteArray body;
};

class SequencedSseServer final : public QObject {
public:
    explicit SequencedSseServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
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

    [[nodiscard]] const CapturedRequest& requestAt(const qsizetype index) const
    {
        return requests_.at(index);
    }

private:
    static QByteArray deltaEvent(const QString& text)
    {
        const QJsonObject object{
            {QStringLiteral("type"), QStringLiteral("response.output_text.delta")},
            {QStringLiteral("delta"), text},
        };
        return QByteArrayLiteral("event: response.output_text.delta\r\ndata: ")
            + QJsonDocument(object).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\r\n\r\n");
    }

    static QByteArray completedEvent()
    {
        const QJsonObject object{
            {QStringLiteral("type"), QStringLiteral("response.completed")},
            {QStringLiteral("response"), QJsonObject{}},
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
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                readRequest(socket);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
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
        if (headerEnd < 0) return;

        const QList<QByteArray> lines = bytes.left(headerEnd).split('\n');
        qsizetype contentLength = 0;
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const QByteArray line = lines.at(index).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0) continue;
            if (line.left(colon).trimmed().compare(
                    QByteArrayLiteral("content-length"),
                    Qt::CaseInsensitive) != 0) {
                continue;
            }
            bool ok = false;
            const qlonglong parsed = line.mid(colon + 1).trimmed().toLongLong(&ok);
            if (ok && parsed >= 0) contentLength = static_cast<qsizetype>(parsed);
        }

        const qsizetype bodyStart = headerEnd + 4;
        if (bytes.size() - bodyStart < contentLength) return;
        requests_.append({bytes.mid(bodyStart, contentLength)});
        handledSockets_.insert(socket);
        sendResponse(socket, requests_.size());
    }

    void sendResponse(QTcpSocket* socket, const qsizetype answerNumber)
    {
        socket->write(QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n"));
        socket->write(deltaEvent(
            QStringLiteral("回答 A%1").arg(answerNumber)));
        socket->write(completedEvent());
        socket->flush();

        const QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(15, this, [guarded] {
            if (guarded != nullptr) guarded->disconnectFromHost();
        });
    }

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QSet<QTcpSocket*> handledSockets_;
    QList<CapturedRequest> requests_;
};

[[nodiscard]] QImage sourceImage()
{
    QImage image(QSize(80, 54), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(
                x,
                y,
                QColor(
                    (x * 9 + y * 5) % 256,
                    (x * 3 + y * 11) % 256,
                    (x * 17 + y * 7) % 256,
                    255));
        }
    }
    return image;
}

[[nodiscard]] ProviderProfile profileFor(
    const SequencedSseServer& server,
    const QUuid& id)
{
    ProviderProfile profile;
    profile.id = id;
    profile.displayName = QStringLiteral("M5 local service");
    profile.protocol = Protocol::OpenAIResponses;
    profile.baseUrl = server.baseUrl();
    profile.credentialRef = QStringLiteral("SnapAsk/provider/")
        + id.toString(QUuid::WithoutBraces);
    profile.modelId = QStringLiteral("m5-vision-model");
    profile.connectTimeoutMs = 1'000;
    profile.requestTimeoutMs = 4'000;
    profile.capabilities = Capabilities(ImageInput | Streaming);
    return profile;
}

[[nodiscard]] QByteArray uploadedPng(const CapturedRequest& request)
{
    const QJsonDocument document = QJsonDocument::fromJson(request.body);
    if (!document.isObject()) return {};
    const QJsonArray input = document.object().value(QStringLiteral("input")).toArray();
    if (input.isEmpty()) return {};
    const QJsonArray content = input.last()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    constexpr auto prefix = "data:image/png;base64,";
    for (const QJsonValue& value : content) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString()
            != QStringLiteral("input_image")) {
            continue;
        }
        const QString url = object.value(QStringLiteral("image_url")).toString();
        if (!url.startsWith(QString::fromLatin1(prefix))) return {};
        return QByteArray::fromBase64(
            url.sliced(static_cast<qsizetype>(qstrlen(prefix))).toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
    }
    return {};
}

[[nodiscard]] QPushButton* button(
    AnswerCardWindow* window,
    const QString& objectName)
{
    return window != nullptr
        ? window->findChild<QPushButton*>(objectName) : nullptr;
}

[[nodiscard]] QStringList bindingTexts(AnswerCardWindow* window)
{
    QStringList result;
    if (window == nullptr) return result;
    const QList<QLabel*> labels = window->findChildren<QLabel*>(
        QStringLiteral("answerTurnBindingLabel"));
    for (const QLabel* label : labels) result.append(label->text());
    return result;
}

[[nodiscard]] bool hasBinding(
    AnswerCardWindow* window,
    const QString& answer,
    const QString& version)
{
    const QStringList texts = bindingTexts(window);
    for (const QString& text : texts) {
        if (text.contains(answer) && text.contains(version)) return true;
    }
    return false;
}

}  // namespace

class M5ConversationFlowTests final : public QObject {
    Q_OBJECT

private slots:
    void linkedWindowChoiceStaysAvailableWithoutVisibleChrome();
    void v1AnswerEditV2AndBothRetryPathsStayBound();
};

void M5ConversationFlowTests::linkedWindowChoiceStaysAvailableWithoutVisibleChrome()
{
    AnswerCardWindow card;
    QPushButton* link = button(
        &card, QStringLiteral("answerLinkToggleButton"));
    QVERIFY(link != nullptr);
    QVERIFY(link->isHidden());
    QVERIFY(link->isCheckable());
    QVERIFY(link->isChecked());
    QVERIFY(card.isLinkedToEditor());

    QSignalSpy changed(&card, &AnswerCardWindow::linkToEditorChanged);
    QTest::mouseClick(link, Qt::LeftButton);
    QCOMPARE(changed.size(), 1);
    QVERIFY(!card.isLinkedToEditor());
    QVERIFY(link->text().contains(QStringLiteral("解除")));

    card.setQuestion(QStringLiteral("busy state"));
    const QUuid requestId = QUuid::createUuid();
    card.beginRequest(requestId);
    QVERIFY(link->isEnabled());
    QTest::mouseClick(link, Qt::LeftButton);
    QCOMPARE(changed.size(), 2);
    QVERIFY(card.isLinkedToEditor());
    card.cancelRequest();
}

void M5ConversationFlowTests::v1AnswerEditV2AndBothRetryPathsStayBound()
{
    SequencedSseServer server;
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
                 profile.credentialRef,
                 QStringLiteral("m5-flow-key-%1").arg(
                     QUuid::createUuid().toString(QUuid::WithoutBraces)),
                 &error),
             qPrintable(error));

    AiProfileRepository profiles(
        temporaryConfig.filePath(QStringLiteral("providers.json")));
    QVERIFY2(profiles.upsert(profile, &error), qPrintable(error));
    QVERIFY2(profiles.setDefault(profile.id, &error), qPrintable(error));
    EndpointConsentStore consent(
        temporaryConfig.filePath(QStringLiteral("consent.ini")));
    QVERIFY(consent.approve(profile.baseUrl));

    EditorWindow editor(sourceImage());
    editor.setAttribute(Qt::WA_DeleteOnClose, false);
    AiNetworkClient network;
    AiSessionController controller(
        &editor, &network, &profiles, &credentials, &consent);
    AnswerCardWindow* card = controller.answerWindow();
    QVERIFY(card != nullptr);
    controller.showQuestionCard();

    QPushButton* send = button(card, QStringLiteral("answerSendButton"));
    QPushButton* retryOriginal = button(
        card, QStringLiteral("answerRetryButton"));
    QPushButton* retryCurrent = button(
        card, QStringLiteral("answerRetryCurrentButton"));
    QLabel* unsent = card->findChild<QLabel*>(
        QStringLiteral("answerUnsentChangesLabel"));
    QVERIFY(send != nullptr);
    QVERIFY(retryOriginal != nullptr);
    QVERIFY(retryCurrent != nullptr);
    QVERIFY(unsent != nullptr);

    const QString firstQuestion = QStringLiteral("第一轮问题");
    card->setQuestion(firstQuestion);
    QCOMPARE(server.requestCount(), 0);
    QTest::mouseClick(send, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(card->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(controller.conversation().revisionCount(), 1);
    QCOMPARE(controller.conversation().turnCount(), 1);
    const ConversationTurn* firstTurn = controller.conversation().turnAt(0);
    QVERIFY(firstTurn != nullptr);
    QCOMPARE(firstTurn->snapshotRevision()->revisionNumber(), quint64{1});
    QCOMPARE(uploadedPng(server.requestAt(0)),
             firstTurn->snapshotRevision()->renderedSnapshot().pngBytes());
    QVERIFY(hasBinding(card, QStringLiteral("A1"), QStringLiteral("v1")));
    QVERIFY(!card->hasUnsentChanges());
    QVERIFY(!unsent->isVisible());

    editor.session().undoStack().push(new snapask::AddAnnotationCommand(
        &editor.session().annotations(),
        Annotation::makeRectangle(QRectF(8.0, 7.0, 31.0, 22.0))));
    QTRY_VERIFY_WITH_TIMEOUT(card->hasUnsentChanges(), 1'000);
    QVERIFY(unsent->isVisible());
    QVERIFY(unsent->text().contains(QStringLiteral("未发送修改")));
    const RenderedSnapshot secondSnapshot =
        SnapshotRenderer::renderCurrent(editor.session());
    QVERIFY(secondSnapshot.isValid());
    QVERIFY(secondSnapshot.pngBytes() != uploadedPng(server.requestAt(0)));
    QTest::qWait(80);
    QCOMPARE(server.requestCount(), 1);

    const QString secondQuestion = QStringLiteral("在新标记基础上继续追问");
    card->setQuestion(secondQuestion);
    QTest::mouseClick(send, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(card->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(controller.conversation().revisionCount(), 2);
    QCOMPARE(controller.conversation().turnCount(), 2);
    const ConversationTurn* secondTurn = controller.conversation().turnAt(1);
    QVERIFY(secondTurn != nullptr);
    QCOMPARE(secondTurn->snapshotRevision()->revisionNumber(), quint64{2});
    QCOMPARE(secondTurn->question(), secondQuestion);
    QCOMPARE(uploadedPng(server.requestAt(1)), secondSnapshot.pngBytes());
    QCOMPARE(card->conversationHistory().size(), 1);
    QCOMPARE(card->conversationHistory().first().snapshotVersion, quint64{1});
    QCOMPARE(card->conversationHistory().first().answerMarkdown,
             QStringLiteral("回答 A1"));
    QVERIFY(hasBinding(card, QStringLiteral("A1"), QStringLiteral("v1")));
    QVERIFY(hasBinding(card, QStringLiteral("A2"), QStringLiteral("v2")));
    QVERIFY(!card->hasUnsentChanges());

    editor.session().undoStack().push(new snapask::AddAnnotationCommand(
        &editor.session().annotations(),
        Annotation::makeArrow(QPointF(4.0, 45.0), QPointF(62.0, 10.0))));
    QTRY_VERIFY_WITH_TIMEOUT(card->hasUnsentChanges(), 1'000);
    const RenderedSnapshot thirdSnapshot =
        SnapshotRenderer::renderCurrent(editor.session());
    QVERIFY(thirdSnapshot.isValid());
    QVERIFY(thirdSnapshot.pngBytes() != secondSnapshot.pngBytes());
    QTest::qWait(80);
    QCOMPARE(server.requestCount(), 2);

    QVERIFY(retryOriginal->isEnabled());
    QVERIFY(retryOriginal->text().contains(QStringLiteral("原快照")));
    QTest::mouseClick(retryOriginal, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 3, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(card->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(controller.conversation().revisionCount(), 2);
    QCOMPARE(controller.conversation().turnCount(), 3);
    const ConversationTurn* originalRetry = controller.conversation().turnAt(2);
    QVERIFY(originalRetry != nullptr);
    QCOMPARE(originalRetry->snapshotRevision()->revisionNumber(), quint64{2});
    QCOMPARE(originalRetry->snapshotRevision()->snapshotId(),
             secondTurn->snapshotRevision()->snapshotId());
    QCOMPARE(uploadedPng(server.requestAt(2)), uploadedPng(server.requestAt(1)));
    QVERIFY(card->hasUnsentChanges());
    QVERIFY(hasBinding(card, QStringLiteral("A3"), QStringLiteral("v2")));

    QVERIFY(retryCurrent->isEnabled());
    QVERIFY(retryCurrent->text().contains(QStringLiteral("当前截图")));
    QTest::mouseClick(retryCurrent, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 4, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(card->state(), AnswerCardState::Completed, 2'000);
    QCOMPARE(controller.conversation().revisionCount(), 3);
    QCOMPARE(controller.conversation().turnCount(), 4);
    const ConversationTurn* currentRetry = controller.conversation().turnAt(3);
    QVERIFY(currentRetry != nullptr);
    QCOMPARE(currentRetry->snapshotRevision()->revisionNumber(), quint64{3});
    QCOMPARE(uploadedPng(server.requestAt(3)), thirdSnapshot.pngBytes());
    QVERIFY(uploadedPng(server.requestAt(3)) != uploadedPng(server.requestAt(2)));
    QVERIFY(!card->hasUnsentChanges());
    QCOMPARE(card->conversationHistory().size(), 3);
    QVERIFY(hasBinding(card, QStringLiteral("A1"), QStringLiteral("v1")));
    QVERIFY(hasBinding(card, QStringLiteral("A2"), QStringLiteral("v2")));
    QVERIFY(hasBinding(card, QStringLiteral("A3"), QStringLiteral("v2")));
    QVERIFY(hasBinding(card, QStringLiteral("A4"), QStringLiteral("v3")));
}

QTEST_MAIN(M5ConversationFlowTests)
#include "M5ConversationFlowTests.moc"
