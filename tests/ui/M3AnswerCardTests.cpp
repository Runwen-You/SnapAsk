#include "ui/answer/AnswerCardWindow.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <utility>

using snapask::ai::AiStreamEvent;
using snapask::ai::ErrorKind;
using snapask::ai::EventType;
using snapask::ui::answer::AnswerCardState;
using snapask::ui::answer::AnswerServiceChoice;
using snapask::ui::answer::AnswerCardWindow;

namespace {

template <typename Widget>
Widget* requiredChild(AnswerCardWindow& window, const char* objectName)
{
    Widget* child = window.findChild<Widget*>(QString::fromLatin1(objectName));
    if (child == nullptr) {
        qFatal("Required AnswerCardWindow child was not found: %s", objectName);
    }
    return child;
}

QTextBrowser* markdownBrowser(AnswerCardWindow& window)
{
    return requiredChild<QTextBrowser>(window, "answerMarkdownBlock");
}

QString renderedAnswerText(AnswerCardWindow& window)
{
    QStringList blocks;
    const auto browsers =
        window.findChildren<QTextBrowser*>(QStringLiteral("answerMarkdownBlock"));
    for (const QTextBrowser* browser : browsers) {
        blocks.append(browser->toPlainText());
    }
    return blocks.join(QLatin1Char('\n'));
}

AiStreamEvent streamEvent(
    const EventType type,
    const QUuid& requestId,
    QString text = {},
    const ErrorKind errorKind = ErrorKind::None)
{
    AiStreamEvent event;
    event.type = type;
    event.requestId = requestId;
    event.text = std::move(text);
    event.errorKind = errorKind;
    return event;
}

} // namespace

class M3AnswerCardTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void showingAndFocusingNeverSends();
    void ctrlEnterAndButtonEachSendExactlyOnceWithMultilineQuestion();
    void streamDeltasBatchAtFortyMillisecondsAndLateEventsAreIgnored();
    void failureAndCancellationPreserveReceivedAnswer_data();
    void failureAndCancellationPreserveReceivedAnswer();
    void stopAndRetrySignalsCarryStableRequestContext();
    void previewAndTargetDomainRemainInMemory();
    void compactCardHidesConfigurationAndCopiesSelectedAnswer();
    void fencedCodeBlocksPreserveBoundariesLabelsAndCopyIndependently();
    void nonClosingFenceTextRemainsInsideTheCodeBlock();
    void markdownNeverLoadsExternalResources();
};

void M3AnswerCardTests::initTestCase()
{
    static_assert(sizeof(AnswerServiceChoice) > 0);
    qRegisterMetaType<AnswerCardState>();
    qRegisterMetaType<AiStreamEvent>();
}

void M3AnswerCardTests::showingAndFocusingNeverSends()
{
    AnswerCardWindow window;
    window.setQuestion(QStringLiteral("显示窗口不能发送这段问题"));
    QSignalSpy sendSpy(&window, &AnswerCardWindow::sendRequested);

    window.show();
    QTest::qWait(30);
    window.focusQuestionInput();
    QTest::qWait(80);

    QVERIFY(window.isVisible());
    QCOMPARE(sendSpy.size(), 0);
    QCOMPARE(window.state(), AnswerCardState::Idle);
    QCOMPARE(window.question(), QStringLiteral("显示窗口不能发送这段问题"));
}

void M3AnswerCardTests::ctrlEnterAndButtonEachSendExactlyOnceWithMultilineQuestion()
{
    AnswerCardWindow window;
    const QString multilineQuestion =
        QStringLiteral("第一行：描述截图\n第二行：保留换行\n第三行：不要合并");
    window.setQuestion(multilineQuestion);
    window.show();
    QTest::qWait(20);

    QSignalSpy sendSpy(&window, &AnswerCardWindow::sendRequested);
    QPlainTextEdit* editor =
        requiredChild<QPlainTextEdit>(window, "answerQuestionEdit");
    editor->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(20);

    QCOMPARE(sendSpy.size(), 1);
    QCOMPARE(sendSpy.at(0).at(0).toString(), multilineQuestion);
    QCOMPARE(window.question(), multilineQuestion);

    QPushButton* sendButton =
        requiredChild<QPushButton>(window, "answerSendButton");
    QVERIFY(sendButton->isEnabled());
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QTest::qWait(20);

    QCOMPARE(sendSpy.size(), 2);
    QCOMPARE(sendSpy.at(1).at(0).toString(), multilineQuestion);
    QCOMPARE(window.question(), multilineQuestion);
}

void M3AnswerCardTests::streamDeltasBatchAtFortyMillisecondsAndLateEventsAreIgnored()
{
    AnswerCardWindow window;
    window.setAnswerMarkdown(QStringLiteral("起始"));
    const QUuid requestId = QUuid::createUuid();
    window.beginRequest(requestId);
    QCOMPARE(window.state(), AnswerCardState::Sending);

    const auto timers = window.findChildren<QTimer*>();
    QCOMPARE(timers.size(), 1);
    QCOMPARE(timers.first()->interval(), 40);
    QVERIFY(timers.first()->isSingleShot());

    QTextBrowser* originalBrowser = markdownBrowser(window);
    QSignalSpy originalDestroyedSpy(originalBrowser, &QObject::destroyed);
    window.consumeStreamEvent(
        streamEvent(EventType::TextDelta, requestId, QStringLiteral("甲")));
    window.consumeStreamEvent(
        streamEvent(EventType::TextDelta, requestId, QStringLiteral("乙")));

    QCOMPARE(window.answerMarkdown(), QStringLiteral("起始甲乙"));
    QCOMPARE(renderedAnswerText(window), QStringLiteral("起始"));
    QCOMPARE(originalDestroyedSpy.size(), 0);
    QTest::qWait(20);
    QCOMPARE(originalDestroyedSpy.size(), 0);
    QCOMPARE(renderedAnswerText(window), QStringLiteral("起始"));

    QTRY_COMPARE_WITH_TIMEOUT(originalDestroyedSpy.size(), 1, 250);
    QCOMPARE(renderedAnswerText(window), QStringLiteral("起始甲乙"));
    QCOMPARE(window.state(), AnswerCardState::Streaming);

    // A terminal event flushes a pending sub-40 ms delta immediately.
    window.consumeStreamEvent(
        streamEvent(EventType::TextDelta, requestId, QStringLiteral("丙")));
    window.consumeStreamEvent(streamEvent(EventType::Completed, requestId));
    QCOMPARE(window.state(), AnswerCardState::Completed);
    QCOMPARE(window.answerMarkdown(), QStringLiteral("起始甲乙丙"));
    QCOMPARE(renderedAnswerText(window), QStringLiteral("起始甲乙丙"));

    window.consumeStreamEvent(
        streamEvent(EventType::TextDelta, requestId, QStringLiteral("迟到内容")));
    window.consumeStreamEvent(
        streamEvent(EventType::Failed, requestId, QStringLiteral("迟到错误"),
                    ErrorKind::Server));
    QTest::qWait(70);
    QCOMPARE(window.state(), AnswerCardState::Completed);
    QCOMPARE(window.answerMarkdown(), QStringLiteral("起始甲乙丙"));
    QCOMPARE(renderedAnswerText(window), QStringLiteral("起始甲乙丙"));
}

void M3AnswerCardTests::failureAndCancellationPreserveReceivedAnswer_data()
{
    QTest::addColumn<int>("terminalType");
    QTest::addColumn<int>("expectedState");
    QTest::newRow("failed")
        << static_cast<int>(EventType::Failed)
        << static_cast<int>(AnswerCardState::Failed);
    QTest::newRow("cancelled")
        << static_cast<int>(EventType::Cancelled)
        << static_cast<int>(AnswerCardState::Cancelled);
}

void M3AnswerCardTests::failureAndCancellationPreserveReceivedAnswer()
{
    QFETCH(int, terminalType);
    QFETCH(int, expectedState);
    AnswerCardWindow window;
    window.setAnswerMarkdown(QStringLiteral("已有回答："));
    const QUuid requestId = QUuid::createUuid();
    window.beginRequest(requestId);
    window.consumeStreamEvent(
        streamEvent(EventType::TextDelta, requestId, QStringLiteral("有效片段")));

    AiStreamEvent terminal = streamEvent(
        static_cast<EventType>(terminalType), requestId);
    if (terminal.type == EventType::Failed) {
        terminal.errorKind = ErrorKind::Network;
        terminal.errorMessage = QStringLiteral("连接中断");
    }
    window.consumeStreamEvent(terminal);

    QCOMPARE(static_cast<int>(window.state()), expectedState);
    QCOMPARE(window.answerMarkdown(), QStringLiteral("已有回答：有效片段"));
    QCOMPARE(renderedAnswerText(window), QStringLiteral("已有回答：有效片段"));
    QVERIFY(!window.errorMessage().isEmpty());

    window.consumeStreamEvent(
        streamEvent(EventType::TextDelta, requestId, QStringLiteral("终态后片段")));
    QTest::qWait(60);
    QCOMPARE(window.answerMarkdown(), QStringLiteral("已有回答：有效片段"));
}

void M3AnswerCardTests::stopAndRetrySignalsCarryStableRequestContext()
{
    AnswerCardWindow window;
    const QString multilineQuestion = QStringLiteral("原问题第一行\n原问题第二行");
    const QUuid requestId = QUuid::createUuid();
    window.setQuestion(multilineQuestion);
    window.beginRequest(requestId);
    window.show();
    QTest::qWait(20);

    QSignalSpy stopSpy(&window, &AnswerCardWindow::stopRequested);
    QSignalSpy retrySpy(&window, &AnswerCardWindow::retryRequested);
    QPushButton* stopButton =
        requiredChild<QPushButton>(window, "answerStopButton");
    QPushButton* retryButton =
        requiredChild<QPushButton>(window, "answerRetryButton");

    QVERIFY(stopButton->isEnabled());
    QVERIFY(!retryButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTest::qWait(10);
    QCOMPARE(stopSpy.size(), 1);
    QCOMPARE(stopSpy.at(0).at(0).toUuid(), requestId);

    window.consumeStreamEvent(streamEvent(EventType::Cancelled, requestId));
    QVERIFY(!stopButton->isEnabled());
    QVERIFY(retryButton->isEnabled());
    QTest::mouseClick(retryButton, Qt::LeftButton);
    QTest::qWait(10);

    QCOMPARE(retrySpy.size(), 1);
    QCOMPARE(retrySpy.at(0).at(0).toUuid(), requestId);
    QCOMPARE(retrySpy.at(0).at(1).toString(), multilineQuestion);
    QCOMPARE(stopSpy.size(), 1);
}

void M3AnswerCardTests::previewAndTargetDomainRemainInMemory()
{
    AnswerCardWindow window;
    QImage preview(QSize(37, 19), QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(12, 78, 145, 255));
    preview.setPixelColor(3, 4, QColor(233, 44, 91, 255));
    const QString domain = QStringLiteral("vision.internal.example");

    window.setPendingSnapshotPreview(preview, domain);
    QCOMPARE(window.pendingSnapshotPreview(), preview);
    QCOMPARE(window.pendingSnapshotPreview().pixelColor(3, 4),
             QColor(233, 44, 91, 255));
    QCOMPARE(window.targetDomain(), domain);

    QLabel* previewLabel =
        requiredChild<QLabel>(window, "pendingSnapshotPreview");
    QLabel* domainLabel =
        requiredChild<QLabel>(window, "pendingSnapshotTargetDomain");
    QVERIFY(!previewLabel->pixmap().isNull());
    QVERIFY(domainLabel->text().contains(domain));

    // Mutating the caller's implicitly shared image detaches it and cannot alter
    // the exact in-memory preview retained by the card.
    preview.fill(Qt::black);
    QCOMPARE(window.pendingSnapshotPreview().pixelColor(3, 4),
             QColor(233, 44, 91, 255));
}

void M3AnswerCardTests::compactCardHidesConfigurationAndCopiesSelectedAnswer()
{
    AnswerCardWindow window;
    window.setAnswerMarkdown(QStringLiteral("只复制这一段回答"));
    window.show();
    QTest::qWait(20);

    QVERIFY(!requiredChild<QComboBox>(window, "answerServiceCombo")->isVisible());
    QVERIFY(!requiredChild<QComboBox>(window, "answerModelCombo")->isVisible());
    QVERIFY(!requiredChild<QPushButton>(window, "answerRetryButton")->isVisible());
    QVERIFY(!requiredChild<QPushButton>(
        window, "answerRetryCurrentButton")->isVisible());
    QVERIFY(!requiredChild<QFrame>(window, "pendingSnapshotPanel")->isVisible());

    QTextBrowser* browser = markdownBrowser(window);
    QTextCursor cursor = browser->textCursor();
    cursor.select(QTextCursor::Document);
    browser->setTextCursor(cursor);
    QPushButton* copy = requiredChild<QPushButton>(window, "answerCopyButton");
    QTRY_VERIFY(copy->isVisible());
    QTest::mouseClick(copy, Qt::LeftButton);
    QCOMPARE(
        QGuiApplication::clipboard()->text(QClipboard::Clipboard),
        QStringLiteral("只复制这一段回答"));
}

void M3AnswerCardTests::fencedCodeBlocksPreserveBoundariesLabelsAndCopyIndependently()
{
    const QString firstCode = QStringLiteral(
        "const char* ticks = \"```\";\n"
        "```\n"
        "return ticks;\n");
    const QString secondCode = QStringLiteral("print(\"第二块\")");
    const QString unfinishedCode = QStringLiteral("{\"unfinished\": true}");

    AnswerCardWindow window;
    window.setAnswerMarkdown(QStringLiteral(
        "导言 **加粗**\n"
        "```` c++\n"
        "const char* ticks = \"```\";\n"
        "```\n"
        "return ticks;\n"
        "\n"
        "````\n"
        "中间\n"
        "~~~ py\n"
        "print(\"第二块\")\n"
        "~~~\n"
        "尾声\n"
        "```json\n"
        "{\"unfinished\": true}"));

    const auto blocks =
        window.findChildren<QFrame*>(QStringLiteral("answerCodeBlock"));
    QCOMPARE(blocks.size(), 3);

    const QStringList expectedCode{firstCode, secondCode, unfinishedCode};
    const QStringList expectedLanguages{
        QStringLiteral("c++"),
        QStringLiteral("py"),
        QStringLiteral("json")};

    QClipboard* clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString originalClipboardText = clipboard->text(QClipboard::Clipboard);

    for (qsizetype index = 0; index < blocks.size(); ++index) {
        QFrame* block = blocks.at(index);
        QPlainTextEdit* editor =
            block->findChild<QPlainTextEdit*>(QStringLiteral("answerCodeEditor"));
        QLabel* languageLabel =
            block->findChild<QLabel*>(QStringLiteral("answerCodeLanguageLabel"));
        QPushButton* copyButton =
            block->findChild<QPushButton*>(QStringLiteral("copyCodeButton"));
        QVERIFY(editor != nullptr);
        QVERIFY(languageLabel != nullptr);
        QVERIFY(copyButton != nullptr);
        QCOMPARE(editor->toPlainText(), expectedCode.at(index));
        QCOMPARE(languageLabel->text(), expectedLanguages.at(index));

        clipboard->setText(QStringLiteral("复制前哨兵"), QClipboard::Clipboard);
        QTest::mouseClick(copyButton, Qt::LeftButton);
        QCOMPARE(clipboard->text(QClipboard::Clipboard), expectedCode.at(index));
    }
    clipboard->setText(originalClipboardText, QClipboard::Clipboard);

    const auto markdownBlocks =
        window.findChildren<QTextBrowser*>(QStringLiteral("answerMarkdownBlock"));
    QCOMPARE(markdownBlocks.size(), 3);
    QCOMPARE(markdownBlocks.at(0)->toPlainText(), QStringLiteral("导言 加粗"));
    QCOMPARE(markdownBlocks.at(1)->toPlainText(), QStringLiteral("中间"));
    QCOMPARE(markdownBlocks.at(2)->toPlainText(), QStringLiteral("尾声"));
}

void M3AnswerCardTests::nonClosingFenceTextRemainsInsideTheCodeBlock()
{
    AnswerCardWindow window;
    window.setAnswerMarkdown(QStringLiteral(
        "```python\n"
        "print('before')\n"
        "```not-a-close\n"
        "print('after')\n"
        "````\n"
        "普通文本中的 ``` 不是围栏"));

    const auto blocks =
        window.findChildren<QFrame*>(QStringLiteral("answerCodeBlock"));
    QCOMPARE(blocks.size(), 1);
    const QPlainTextEdit* editor = blocks.first()->findChild<QPlainTextEdit*>(
        QStringLiteral("answerCodeEditor"));
    QVERIFY(editor != nullptr);
    QCOMPARE(
        editor->toPlainText(),
        QStringLiteral("print('before')\n```not-a-close\nprint('after')"));

    const auto markdownBlocks =
        window.findChildren<QTextBrowser*>(QStringLiteral("answerMarkdownBlock"));
    QCOMPARE(markdownBlocks.size(), 1);
    QCOMPARE(
        markdownBlocks.first()->toPlainText(),
        QStringLiteral("普通文本中的 ``` 不是围栏"));
}

void M3AnswerCardTests::markdownNeverLoadsExternalResources()
{
    QTcpServer resourceTrap;
    QVERIFY(resourceTrap.listen(QHostAddress::LocalHost, 0));
    const QUrl resourceUrl(QStringLiteral("http://127.0.0.1:%1/tracker.png")
                               .arg(resourceTrap.serverPort()));

    AnswerCardWindow window;
    window.setAnswerMarkdown(
        QStringLiteral(
            "安全文本\n\n"
            "![远程跟踪像素](%1)\n\n"
            "<img src=\"%2\">\n\n"
            "````markdown\n"
            "![代码中的远程地址](%3)\n"
            "````")
            .arg(resourceUrl.toString(),
                 resourceUrl.resolved(QUrl(QStringLiteral("raw-html.png"))).toString(),
                 resourceUrl.resolved(QUrl(QStringLiteral("literal.png"))).toString()));
    window.show();
    QTest::qWait(100);

    QVERIFY(!resourceTrap.hasPendingConnections());
    QTextBrowser* browser = markdownBrowser(window);
    const QVariant resource = browser->document()->resource(
        QTextDocument::ImageResource, resourceUrl);
    QVERIFY(!resource.isValid() || resource.isNull());
    const auto codeBlocks =
        window.findChildren<QFrame*>(QStringLiteral("answerCodeBlock"));
    QCOMPARE(codeBlocks.size(), 1);
    const QPlainTextEdit* codeEditor = codeBlocks.first()->findChild<QPlainTextEdit*>(
        QStringLiteral("answerCodeEditor"));
    QVERIFY(codeEditor != nullptr);
    QVERIFY(codeEditor->toPlainText().contains(QStringLiteral("literal.png")));
    QCoreApplication::processEvents();
    QTest::qWait(80);
    QVERIFY(!resourceTrap.hasPendingConnections());
}

QTEST_MAIN(M3AnswerCardTests)
#include "M3AnswerCardTests.moc"
