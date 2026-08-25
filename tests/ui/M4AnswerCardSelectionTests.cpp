#include "ui/answer/AnswerCardWindow.h"

#include <QComboBox>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest>

using snapask::ai::AiStreamEvent;
using snapask::ai::EventType;
using snapask::ui::answer::AnswerCardState;
using snapask::ui::answer::AnswerCardWindow;
using snapask::ui::answer::AnswerServiceChoice;

namespace {

AnswerServiceChoice choice(
    const QUuid& id,
    const QString& name,
    const QString& domain,
    QStringList models,
    const QString& defaultModel)
{
    return {id, name, domain, std::move(models), defaultModel};
}

QComboBox* combo(AnswerCardWindow& window, const char* name)
{
    auto* result = window.findChild<QComboBox*>(QString::fromLatin1(name));
    if (result == nullptr) qFatal("Missing answer combo: %s", name);
    return result;
}

AiStreamEvent streamEvent(EventType type, const QUuid& requestId)
{
    AiStreamEvent result;
    result.type = type;
    result.requestId = requestId;
    return result;
}

}  // namespace

class M4AnswerCardSelectionTests final : public QObject {
    Q_OBJECT

private slots:
    void programmaticChoicesSelectRequestedProfileWithoutSignals();
    void userCanSwitchServiceAndUseManualModelFallback();
    void busyRequestLocksChoicesAndTerminalRestoresThem();
    void refreshedChoicesPreserveOrSafelyFallBack();
    void choicesAndPreviewNeverOpenTargetDomain();
};

void M4AnswerCardSelectionTests::programmaticChoicesSelectRequestedProfileWithoutSignals()
{
    AnswerCardWindow window;
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    QSignalSpy spy(&window, &AnswerCardWindow::selectionChanged);

    window.setServiceChoices(
        {choice(first, QStringLiteral("Responses"), QStringLiteral("api.one.test"),
                {QStringLiteral("model-a"), QStringLiteral("model-b")},
                QStringLiteral("model-a")),
         choice(second, QStringLiteral("Chat"), QStringLiteral("api.two.test"),
                {QStringLiteral("vision-x"), QStringLiteral("vision-y")},
                QStringLiteral("vision-x"))},
        second, QStringLiteral("vision-y"));

    QCOMPARE(spy.size(), 0);
    QCOMPARE(window.selectedProfileId(), second);
    QCOMPARE(window.selectedModelId(), QStringLiteral("vision-y"));
    QCOMPARE(window.serviceName(), QStringLiteral("Chat"));
    QCOMPARE(window.targetDomain(), QStringLiteral("api.two.test"));
    QCOMPARE(combo(window, "answerServiceCombo")->count(), 2);
    QCOMPARE(combo(window, "answerModelCombo")->count(), 2);
}

void M4AnswerCardSelectionTests::userCanSwitchServiceAndUseManualModelFallback()
{
    AnswerCardWindow window;
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    window.setPendingSnapshotPreview(
        QImage(QSize(8, 8), QImage::Format_ARGB32_Premultiplied),
        QStringLiteral("api.one.test"));
    window.setServiceChoices(
        {choice(first, QStringLiteral("One"), QStringLiteral("api.one.test"),
                {QStringLiteral("a")}, QStringLiteral("a")),
         choice(second, QStringLiteral("Two"), QStringLiteral("api.two.test"),
                {}, QStringLiteral("manual-default"))},
        first, QStringLiteral("a"));
    QSignalSpy spy(&window, &AnswerCardWindow::selectionChanged);

    combo(window, "answerServiceCombo")->setCurrentIndex(1);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).toUuid(), second);
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("manual-default"));
    QCOMPARE(window.targetDomain(), QStringLiteral("api.two.test"));
    auto* domainLabel = window.findChild<QLabel*>(
        QStringLiteral("pendingSnapshotTargetDomain"));
    QVERIFY(domainLabel != nullptr);
    QVERIFY(domainLabel->text().contains(QStringLiteral("api.two.test")));

    combo(window, "answerModelCombo")->setEditText(QStringLiteral("manual-vision"));
    QCOMPARE(spy.size(), 2);
    QCOMPARE(spy.last().at(0).toUuid(), second);
    QCOMPARE(spy.last().at(1).toString(), QStringLiteral("manual-vision"));
    QCOMPARE(window.selectedModelId(), QStringLiteral("manual-vision"));
}

void M4AnswerCardSelectionTests::busyRequestLocksChoicesAndTerminalRestoresThem()
{
    AnswerCardWindow window;
    const QUuid profile = QUuid::createUuid();
    window.setServiceChoices(
        {choice(profile, QStringLiteral("One"), QStringLiteral("one.test"),
                {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("a"))},
        profile, QStringLiteral("a"));
    auto* service = combo(window, "answerServiceCombo");
    auto* model = combo(window, "answerModelCombo");
    QVERIFY(service->isEnabled());
    QVERIFY(model->isEnabled());

    const QUuid requestId = QUuid::createUuid();
    window.beginRequest(requestId);
    QCOMPARE(window.state(), AnswerCardState::Sending);
    QVERIFY(!service->isEnabled());
    QVERIFY(!model->isEnabled());

    window.consumeStreamEvent(streamEvent(EventType::Completed, requestId));
    QCOMPARE(window.state(), AnswerCardState::Completed);
    QVERIFY(service->isEnabled());
    QVERIFY(model->isEnabled());
}

void M4AnswerCardSelectionTests::refreshedChoicesPreserveOrSafelyFallBack()
{
    AnswerCardWindow window;
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    const auto firstChoice = choice(
        first, QStringLiteral("One"), QStringLiteral("one.test"),
        {QStringLiteral("a")}, QStringLiteral("a"));
    const auto secondChoice = choice(
        second, QStringLiteral("Two"), QStringLiteral("two.test"),
        {QStringLiteral("b")}, QStringLiteral("b"));
    QSignalSpy spy(&window, &AnswerCardWindow::selectionChanged);

    window.setServiceChoices({firstChoice, secondChoice}, second, QStringLiteral("b"));
    window.setServiceChoices({firstChoice, secondChoice},
                             window.selectedProfileId(), window.selectedModelId());
    QCOMPARE(window.selectedProfileId(), second);
    QCOMPARE(window.selectedModelId(), QStringLiteral("b"));
    QCOMPARE(spy.size(), 0);

    window.setServiceChoices({firstChoice}, second, QStringLiteral("b"));
    QCOMPARE(window.selectedProfileId(), first);
    QCOMPARE(window.selectedModelId(), QStringLiteral("b"));
    QCOMPARE(spy.size(), 0);
}

void M4AnswerCardSelectionTests::choicesAndPreviewNeverOpenTargetDomain()
{
    QTcpServer trap;
    QVERIFY(trap.listen(QHostAddress::LocalHost, 0));
    const QString domain = QStringLiteral("127.0.0.1:%1").arg(trap.serverPort());
    AnswerCardWindow window;
    const QUuid profile = QUuid::createUuid();
    QImage image(QSize(16, 12), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    window.setPendingSnapshotPreview(image, domain);
    window.setServiceChoices(
        {choice(profile, QStringLiteral("Local"), domain,
                {QStringLiteral("model")}, QStringLiteral("model"))},
        profile, QStringLiteral("model"));
    window.show();
    QTest::qWait(120);
    QVERIFY(!trap.hasPendingConnections());
}

QTEST_MAIN(M4AnswerCardSelectionTests)
#include "M4AnswerCardSelectionTests.moc"
