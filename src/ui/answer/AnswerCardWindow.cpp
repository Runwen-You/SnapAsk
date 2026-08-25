#include "ui/answer/AnswerCardWindow.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QtConcurrentRun>

#include "ui/common/GlyphIcon.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace snapask::ui::answer {

namespace {

constexpr int streamBatchIntervalMs = 40;
constexpr qsizetype synchronousPreviewPixelLimit = 1'000'000;

struct PreviewScaleResult final {
    quint64 generation{0};
    QImage thumbnail;
};

struct MarkdownSegment {
    bool isCode{false};
    QString content;
    QString language;
};

struct MarkdownFence {
    QChar marker;
    qsizetype length{0};
    QString info;
};

[[nodiscard]] bool parseOpeningFence(
    const QString& line,
    MarkdownFence* fence)
{
    qsizetype cursor = 0;
    while (cursor < line.size() && line.at(cursor) == QLatin1Char(' ')) {
        ++cursor;
    }
    // CommonMark permits at most three leading spaces. Four-space-indented
    // text is an indented code block and must not accidentally open a fenced
    // CodeBlockWidget.
    if (cursor > 3 || cursor >= line.size()) {
        return false;
    }

    const QChar marker = line.at(cursor);
    if (marker != QLatin1Char('`') && marker != QLatin1Char('~')) {
        return false;
    }

    const qsizetype markerStart = cursor;
    while (cursor < line.size() && line.at(cursor) == marker) {
        ++cursor;
    }
    const qsizetype markerLength = cursor - markerStart;
    if (markerLength < 3) {
        return false;
    }

    const QString info = line.mid(cursor).trimmed();
    if (marker == QLatin1Char('`') && info.contains(QLatin1Char('`'))) {
        return false;
    }

    fence->marker = marker;
    fence->length = markerLength;
    fence->info = info;
    return true;
}

[[nodiscard]] bool isClosingFence(
    const QString& line,
    const MarkdownFence& openingFence)
{
    qsizetype cursor = 0;
    while (cursor < line.size() && line.at(cursor) == QLatin1Char(' ')) {
        ++cursor;
    }
    if (cursor > 3 || cursor >= line.size()
        || line.at(cursor) != openingFence.marker) {
        return false;
    }

    const qsizetype markerStart = cursor;
    while (cursor < line.size() && line.at(cursor) == openingFence.marker) {
        ++cursor;
    }
    return cursor - markerStart >= openingFence.length
        && line.mid(cursor).trimmed().isEmpty();
}

[[nodiscard]] std::vector<MarkdownSegment> splitMarkdown(const QString& markdown)
{
    std::vector<MarkdownSegment> segments;
    const QStringList lines = markdown.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QStringList bufferedLines;
    bool inCodeBlock = false;
    MarkdownFence openingFence;

    const auto flush =
        [&segments, &bufferedLines, &inCodeBlock, &openingFence](const bool force) {
        if (bufferedLines.isEmpty() && !force) {
            return;
        }
        segments.push_back(
            MarkdownSegment{
                inCodeBlock,
                bufferedLines.join(QLatin1Char('\n')),
                openingFence.info});
        bufferedLines.clear();
    };

    for (const QString& line : lines) {
        if (inCodeBlock) {
            if (isClosingFence(line, openingFence)) {
                flush(true);
                inCodeBlock = false;
                openingFence = {};
            } else {
                bufferedLines.push_back(line);
            }
            continue;
        }

        MarkdownFence candidate;
        if (parseOpeningFence(line, &candidate)) {
            flush(false);
            inCodeBlock = true;
            openingFence = std::move(candidate);
        } else {
            bufferedLines.push_back(line);
        }
    }
    flush(inCodeBlock);
    return segments;
}

[[nodiscard]] QString normalizedLanguage(const QString& language)
{
    const QString result = language.trimmed().toLower();
    if (result == QStringLiteral("c++") || result == QStringLiteral("cc") ||
        result == QStringLiteral("cxx")) {
        return QStringLiteral("cpp");
    }
    if (result == QStringLiteral("js")) {
        return QStringLiteral("javascript");
    }
    if (result == QStringLiteral("py")) {
        return QStringLiteral("python");
    }
    return result;
}

class SafeTextBrowser final : public QTextBrowser {
public:
    explicit SafeTextBrowser(QWidget* parent = nullptr)
        : QTextBrowser(parent)
    {
    }

protected:
    QVariant loadResource(int, const QUrl&) override
    {
        // Answer Markdown is untrusted network output. Never resolve embedded
        // resources while rendering it.
        return {};
    }
};

class CodeHighlighter final : public QSyntaxHighlighter {
public:
    CodeHighlighter(QTextDocument* document, QString language)
        : QSyntaxHighlighter(document)
        , language_(normalizedLanguage(language))
    {
        keywordFormat_.setForeground(QColor(QStringLiteral("#7c3aed")));
        keywordFormat_.setFontWeight(QFont::DemiBold);
        stringFormat_.setForeground(QColor(QStringLiteral("#047857")));
        numberFormat_.setForeground(QColor(QStringLiteral("#b45309")));
        commentFormat_.setForeground(QColor(QStringLiteral("#64748b")));
        commentFormat_.setFontItalic(true);
        keyFormat_.setForeground(QColor(QStringLiteral("#0369a1")));
    }

protected:
    void highlightBlock(const QString& text) override
    {
        const auto highlightPattern = [this, &text](
                                          const QRegularExpression& expression,
                                          const QTextCharFormat& format) {
            QRegularExpressionMatchIterator matches = expression.globalMatch(text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                setFormat(match.capturedStart(), match.capturedLength(), format);
            }
        };

        highlightPattern(
            QRegularExpression(QStringLiteral(R"(("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'))")),
            stringFormat_);
        highlightPattern(
            QRegularExpression(QStringLiteral(R"(\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?)\b)")),
            numberFormat_);

        if (language_ == QStringLiteral("json")) {
            highlightPattern(
                QRegularExpression(QStringLiteral(R"("(?:\\.|[^"\\])*"(?=\s*:))")),
                keyFormat_);
            highlightPattern(
                QRegularExpression(QStringLiteral(R"(\b(?:true|false|null)\b)")),
                keywordFormat_);
            return;
        }

        QStringList keywords;
        QString commentPattern;
        if (language_ == QStringLiteral("python")) {
            keywords = {
                QStringLiteral("and"), QStringLiteral("as"), QStringLiteral("assert"),
                QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("break"),
                QStringLiteral("class"), QStringLiteral("continue"), QStringLiteral("def"),
                QStringLiteral("del"), QStringLiteral("elif"), QStringLiteral("else"),
                QStringLiteral("except"), QStringLiteral("False"), QStringLiteral("finally"),
                QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("global"),
                QStringLiteral("if"), QStringLiteral("import"), QStringLiteral("in"),
                QStringLiteral("is"), QStringLiteral("lambda"), QStringLiteral("None"),
                QStringLiteral("not"), QStringLiteral("or"), QStringLiteral("pass"),
                QStringLiteral("raise"), QStringLiteral("return"), QStringLiteral("True"),
                QStringLiteral("try"), QStringLiteral("while"), QStringLiteral("with"),
                QStringLiteral("yield")};
            commentPattern = QStringLiteral(R"(#[^\n]*)");
        } else if (
            language_ == QStringLiteral("javascript") ||
            language_ == QStringLiteral("typescript")) {
            keywords = {
                QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("break"),
                QStringLiteral("case"), QStringLiteral("catch"), QStringLiteral("class"),
                QStringLiteral("const"), QStringLiteral("continue"), QStringLiteral("default"),
                QStringLiteral("delete"), QStringLiteral("do"), QStringLiteral("else"),
                QStringLiteral("export"), QStringLiteral("extends"), QStringLiteral("false"),
                QStringLiteral("finally"), QStringLiteral("for"), QStringLiteral("from"),
                QStringLiteral("function"), QStringLiteral("if"), QStringLiteral("import"),
                QStringLiteral("in"), QStringLiteral("instanceof"), QStringLiteral("let"),
                QStringLiteral("new"), QStringLiteral("null"), QStringLiteral("return"),
                QStringLiteral("static"), QStringLiteral("super"), QStringLiteral("switch"),
                QStringLiteral("this"), QStringLiteral("throw"), QStringLiteral("true"),
                QStringLiteral("try"), QStringLiteral("typeof"), QStringLiteral("undefined"),
                QStringLiteral("var"), QStringLiteral("while")};
            commentPattern = QStringLiteral(R"(//[^\n]*)");
        } else if (
            language_ == QStringLiteral("cpp") || language_ == QStringLiteral("c") ||
            language_ == QStringLiteral("csharp") || language_.isEmpty()) {
            keywords = {
                QStringLiteral("auto"), QStringLiteral("bool"), QStringLiteral("break"),
                QStringLiteral("case"), QStringLiteral("catch"), QStringLiteral("char"),
                QStringLiteral("class"), QStringLiteral("const"), QStringLiteral("constexpr"),
                QStringLiteral("continue"), QStringLiteral("default"), QStringLiteral("delete"),
                QStringLiteral("do"), QStringLiteral("double"), QStringLiteral("else"),
                QStringLiteral("enum"), QStringLiteral("explicit"), QStringLiteral("false"),
                QStringLiteral("float"), QStringLiteral("for"), QStringLiteral("if"),
                QStringLiteral("int"), QStringLiteral("long"), QStringLiteral("namespace"),
                QStringLiteral("new"), QStringLiteral("nullptr"), QStringLiteral("private"),
                QStringLiteral("protected"), QStringLiteral("public"), QStringLiteral("return"),
                QStringLiteral("short"), QStringLiteral("signed"), QStringLiteral("static"),
                QStringLiteral("struct"), QStringLiteral("switch"), QStringLiteral("template"),
                QStringLiteral("this"), QStringLiteral("throw"), QStringLiteral("true"),
                QStringLiteral("try"), QStringLiteral("typedef"), QStringLiteral("typename"),
                QStringLiteral("union"), QStringLiteral("unsigned"), QStringLiteral("using"),
                QStringLiteral("virtual"), QStringLiteral("void"), QStringLiteral("volatile"),
                QStringLiteral("while")};
            commentPattern = QStringLiteral(R"(//[^\n]*)");
        }

        if (!keywords.isEmpty()) {
            const QString keywordPattern = QStringLiteral("\\b(?:%1)\\b")
                                               .arg(keywords.join(QLatin1Char('|')));
            highlightPattern(QRegularExpression(keywordPattern), keywordFormat_);
        }
        if (!commentPattern.isEmpty()) {
            // Apply comments last so their appearance wins over tokens inside
            // the comment.
            highlightPattern(QRegularExpression(commentPattern), commentFormat_);
        }
    }

private:
    QString language_;
    QTextCharFormat keywordFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat keyFormat_;
};

class CodeBlockWidget final : public QFrame {
public:
    CodeBlockWidget(QString code, QString language, QWidget* parent = nullptr)
        : QFrame(parent)
        , code_(std::move(code))
    {
        setObjectName(QStringLiteral("answerCodeBlock"));
        setFrameShape(QFrame::StyledPanel);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 6, 8, 8);
        layout->setSpacing(4);

        auto* headerLayout = new QHBoxLayout();
        const QString displayLanguage = language.trimmed().isEmpty()
                                            ? tr("代码")
                                            : language.trimmed();
        auto* languageLabel = new QLabel(displayLanguage, this);
        languageLabel->setObjectName(QStringLiteral("answerCodeLanguageLabel"));
        languageLabel->setTextFormat(Qt::PlainText);
        languageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        headerLayout->addWidget(languageLabel);
        headerLayout->addStretch(1);

        auto* copyButton = new QPushButton(tr("复制代码"), this);
        copyButton->setObjectName(QStringLiteral("copyCodeButton"));
        copyButton->setToolTip(tr("仅复制此代码块"));
        headerLayout->addWidget(copyButton);
        layout->addLayout(headerLayout);

        editor_ = new QPlainTextEdit(this);
        editor_->setObjectName(QStringLiteral("answerCodeEditor"));
        editor_->setReadOnly(true);
        editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
        editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        editor_->setPlainText(code_);
        editor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        editor_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        editor_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        const int lineHeight = editor_->fontMetrics().lineSpacing();
        const int visibleLines = std::clamp(editor_->document()->blockCount(), 2, 16);
        editor_->setFixedHeight((visibleLines * lineHeight) + 24);
        layout->addWidget(editor_);

        new CodeHighlighter(editor_->document(), std::move(language));

        connect(copyButton, &QPushButton::clicked, this, [this]() {
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr) {
                clipboard->setText(code_, QClipboard::Clipboard);
            }
        });
    }

private:
    QString code_;
    QPlainTextEdit* editor_{nullptr};
};

}  // namespace

class MarkdownAnswerView final : public QScrollArea {
public:
    explicit MarkdownAnswerView(QWidget* parent = nullptr)
        : QScrollArea(parent)
    {
        setObjectName(QStringLiteral("answerScrollArea"));
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        content_ = new QWidget(this);
        content_->setObjectName(QStringLiteral("answerContent"));
        layout_ = new QVBoxLayout(content_);
        layout_->setContentsMargins(2, 2, 8, 2);
        layout_->setSpacing(10);
        layout_->setAlignment(Qt::AlignTop);
        setWidget(content_);
    }

    void setLinkHandler(std::function<void(const QUrl&)> handler)
    {
        linkHandler_ = std::move(handler);
    }

    void setConversation(
        const QList<AnswerTurnPresentation>& history,
        const QString& currentMarkdown,
        const bool hasCurrentTurn,
        const quint64 currentAnswerNumber,
        const quint64 currentSnapshotVersion,
        const QString& currentQuestion,
        const QString& currentServiceName,
        const QString& currentModelId)
    {
        const QScrollBar* scrollBar = verticalScrollBar();
        const bool wasNearBottom =
            (scrollBar->maximum() - scrollBar->value()) <= 32;

        clearContent();
        browsers_.clear();

        const auto addBindingLabel = [this](
                                         const quint64 answerNumber,
                                         const quint64 snapshotVersion,
                                         const QString& serviceName,
                                         const QString& modelId,
                                         const QString& status) {
            QString binding = tr("A%1 · 基于 v%2")
                                  .arg(answerNumber)
                                  .arg(snapshotVersion);
            QStringList context;
            if (!serviceName.trimmed().isEmpty()) context.append(serviceName.trimmed());
            if (!modelId.trimmed().isEmpty()) context.append(modelId.trimmed());
            if (!context.isEmpty()) {
                binding.append(QStringLiteral(" · "));
                binding.append(context.join(QStringLiteral(" / ")));
            }
            if (!status.isEmpty()) {
                binding.append(QStringLiteral(" · "));
                binding.append(status);
            }
            auto* label = new QLabel(binding, content_);
            label->setObjectName(QStringLiteral("answerTurnBindingLabel"));
            label->setTextFormat(Qt::PlainText);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setWordWrap(true);
            layout_->addWidget(label);
        };

        const auto addQuestion = [this](const QString& question) {
            if (question.isEmpty()) return;
            auto* label = new QLabel(tr("问题：%1").arg(question), content_);
            label->setObjectName(QStringLiteral("answerTurnQuestionLabel"));
            label->setTextFormat(Qt::PlainText);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setWordWrap(true);
            layout_->addWidget(label);
        };

        const auto addMarkdown = [this](const QString& markdown) {
            if (markdown.isEmpty()) return false;
            const std::vector<MarkdownSegment> segments = splitMarkdown(markdown);
            bool added = false;
            for (const MarkdownSegment& segment : segments) {
                if (segment.isCode) {
                    layout_->addWidget(new CodeBlockWidget(
                        segment.content,
                        segment.language,
                        content_));
                    added = true;
                    continue;
                }
                if (segment.content.isEmpty()) continue;

                auto* browser = new SafeTextBrowser(content_);
                browser->setObjectName(QStringLiteral("answerMarkdownBlock"));
                browser->setFrameShape(QFrame::NoFrame);
                browser->setOpenLinks(false);
                browser->setOpenExternalLinks(false);
                browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                browser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                browser->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                browser->document()->setDocumentMargin(2.0);
                browser->document()->setMarkdown(
                    segment.content,
                    QTextDocument::MarkdownFeatures{
                        QTextDocument::MarkdownDialectGitHub,
                        QTextDocument::MarkdownNoHTML});
                connect(browser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
                    if (linkHandler_) linkHandler_(url);
                });
                layout_->addWidget(browser);
                browsers_.push_back(browser);
                added = true;
            }
            return added;
        };

        const auto addSeparator = [this]() {
            auto* separator = new QFrame(content_);
            separator->setObjectName(QStringLiteral("answerTurnSeparator"));
            separator->setFrameShape(QFrame::HLine);
            separator->setFrameShadow(QFrame::Sunken);
            layout_->addWidget(separator);
        };

        bool hasVisibleTurn = false;
        for (const AnswerTurnPresentation& turn : history) {
            if (hasVisibleTurn) addSeparator();
            QString status;
            if (turn.state == AnswerCardState::Failed) status = tr("失败");
            else if (turn.state == AnswerCardState::Cancelled) status = tr("已取消");
            addBindingLabel(
                turn.answerNumber,
                turn.snapshotVersion,
                turn.serviceName,
                turn.modelId,
                status);
            addQuestion(turn.question);
            if (!addMarkdown(turn.answerMarkdown)) {
                auto* empty = new QLabel(
                    turn.detail.isEmpty() ? tr("此回合没有可显示的回答。") : turn.detail,
                    content_);
                empty->setObjectName(QStringLiteral("answerTurnEmptyLabel"));
                empty->setTextFormat(Qt::PlainText);
                empty->setWordWrap(true);
                layout_->addWidget(empty);
            }
            hasVisibleTurn = true;
        }

        if (hasCurrentTurn) {
            if (hasVisibleTurn) addSeparator();
            addBindingLabel(
                currentAnswerNumber,
                currentSnapshotVersion,
                currentServiceName,
                currentModelId,
                {});
            addQuestion(currentQuestion);
            if (!addMarkdown(currentMarkdown)) {
                auto* placeholder = new QLabel(tr("回答会在这里流式显示。"), content_);
                placeholder->setObjectName(QStringLiteral("answerPlaceholder"));
                placeholder->setAlignment(Qt::AlignCenter);
                placeholder->setWordWrap(true);
                layout_->addWidget(placeholder);
            }
            hasVisibleTurn = true;
        } else if (!currentMarkdown.isEmpty()) {
            if (hasVisibleTurn) addSeparator();
            (void)addMarkdown(currentMarkdown);
            hasVisibleTurn = true;
        }

        if (!hasVisibleTurn) {
            auto* placeholder = new QLabel(tr("回答会在这里流式显示。"), content_);
            placeholder->setObjectName(QStringLiteral("answerPlaceholder"));
            placeholder->setAlignment(Qt::AlignCenter);
            placeholder->setWordWrap(true);
            placeholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            layout_->addWidget(placeholder, 1);
        }
        layout_->addStretch(1);

        updateBrowserSizes();
        if (wasNearBottom) {
            QTimer::singleShot(0, this, [this]() {
                verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            });
        }
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QScrollArea::resizeEvent(event);
        updateBrowserSizes();
    }

private:
    void clearContent()
    {
        while (QLayoutItem* item = layout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    }

    void updateBrowserSizes()
    {
        const int availableWidth = std::max(240, viewport()->width() - 18);
        for (SafeTextBrowser* browser : browsers_) {
            browser->document()->setTextWidth(static_cast<qreal>(availableWidth));
            const qreal documentHeight = browser->document()->size().height();
            browser->setFixedHeight(
                std::max(24, static_cast<int>(std::ceil(documentHeight)) + 2));
        }
    }

    QWidget* content_{nullptr};
    QVBoxLayout* layout_{nullptr};
    std::vector<SafeTextBrowser*> browsers_;
    std::function<void(const QUrl&)> linkHandler_;
};

AnswerCardWindow::AnswerCardWindow(QWidget* parent)
    : QWidget(
          parent,
          Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint)
{
    setObjectName(QStringLiteral("answerCardWindow"));
    setWindowTitle(tr("SnapAsk 回答"));
    setMinimumSize(360, 300);
    resize(460, 560);

    batchTimer_ = new QTimer(this);
    batchTimer_->setSingleShot(true);
    batchTimer_->setInterval(streamBatchIntervalMs);
    connect(batchTimer_, &QTimer::timeout, this, &AnswerCardWindow::flushPendingText);

    buildCompactUi();
    updateHeader();
    updateStatusLabel();
    updateControls();
}

AnswerCardWindow::~AnswerCardWindow()
{
    // Editable combo boxes may emit index/text changes while QWidget destroys
    // its children. Disconnect them before C++ members such as
    // serviceChoices_ are torn down.
    if (serviceCombo_ != nullptr) serviceCombo_->disconnect(this);
    if (modelCombo_ != nullptr) modelCombo_->disconnect(this);
}

void AnswerCardWindow::setRequestContext(
    QString serviceName,
    QString modelId,
    const quint64 snapshotVersion)
{
    serviceName_ = std::move(serviceName);
    modelId_ = std::move(modelId);
    snapshotVersion_ = snapshotVersion;
    updateHeader();
}

QString AnswerCardWindow::serviceName() const
{
    if (const auto* choice = selectedServiceChoice(); choice != nullptr) {
        return choice->displayName;
    }
    return serviceName_;
}

QString AnswerCardWindow::modelId() const
{
    if (modelCombo_ != nullptr && !modelCombo_->currentText().trimmed().isEmpty()) {
        return modelCombo_->currentText().trimmed();
    }
    return modelId_;
}

quint64 AnswerCardWindow::snapshotVersion() const noexcept
{
    return snapshotVersion_;
}

void AnswerCardWindow::setQuestion(const QString& question)
{
    questionEdit_->setPlainText(question);
}

QString AnswerCardWindow::question() const
{
    return questionEdit_->toPlainText();
}

void AnswerCardWindow::setAnswerMarkdown(const QString& markdown)
{
    batchTimer_->stop();
    pendingText_.clear();
    answerMarkdown_ = markdown;
    refreshAnswerView();
    updateControls();
}

QString AnswerCardWindow::answerMarkdown() const
{
    return answerMarkdown_ + pendingText_;
}

AnswerCardState AnswerCardWindow::state() const noexcept
{
    return state_;
}

QUuid AnswerCardWindow::activeRequestId() const noexcept
{
    return activeRequestId_;
}

QString AnswerCardWindow::errorMessage() const
{
    return errorMessage_;
}

void AnswerCardWindow::setPendingSnapshotPreview(
    const QImage& image,
    QString targetDomain)
{
    ++previewGeneration_;
    pendingSnapshotPreview_ = image;
    targetDomain_ = std::move(targetDomain);

    if (pendingSnapshotPreview_.isNull()) {
        snapshotPreviewLabel_->clear();
        targetDomainLabel_->clear();
        snapshotPreviewPanel_->hide();
        return;
    }

    constexpr int previewWidth = 128;
    constexpr int previewHeight = 80;
    const auto applyThumbnail = [this](const QImage& thumbnail) {
        snapshotPreviewLabel_->setPixmap(QPixmap::fromImage(thumbnail));
    };
    if (pendingSnapshotPreview_.sizeInBytes()
        <= synchronousPreviewPixelLimit * 4) {
        applyThumbnail(pendingSnapshotPreview_.scaled(
            previewWidth,
            previewHeight,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    } else {
        snapshotPreviewLabel_->clear();
        snapshotPreviewLabel_->setText(tr("准备预览…"));
        const quint64 generation = previewGeneration_;
        const QImage frozenImage = pendingSnapshotPreview_;
        auto* watcher = new QFutureWatcher<PreviewScaleResult>(this);
        connect(
            watcher,
            &QFutureWatcher<PreviewScaleResult>::finished,
            this,
            [this, watcher]() {
                const PreviewScaleResult result = watcher->result();
                watcher->deleteLater();
                if (result.generation != previewGeneration_
                    || pendingSnapshotPreview_.isNull()) {
                    return;
                }
                snapshotPreviewLabel_->setPixmap(
                    QPixmap::fromImage(result.thumbnail));
            });
        watcher->setFuture(QtConcurrent::run(
            [frozenImage, generation]() {
                PreviewScaleResult result;
                result.generation = generation;
                result.thumbnail = frozenImage.scaled(
                    previewWidth,
                    previewHeight,
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
                return result;
            }));
    }
    snapshotPreviewLabel_->setToolTip(
        tr("待发送截图：%1 × %2")
            .arg(pendingSnapshotPreview_.width())
            .arg(pendingSnapshotPreview_.height()));
    targetDomainLabel_->setText(
        tr("目标域名：%1")
            .arg(targetDomain_.isEmpty() ? tr("未设置") : targetDomain_));
    // The canonical preview remains in memory for the explicit-send boundary,
    // but the compact card does not repeat the editor image or target domain.
    snapshotPreviewPanel_->hide();
}

const QImage& AnswerCardWindow::pendingSnapshotPreview() const noexcept
{
    return pendingSnapshotPreview_;
}

quint64 AnswerCardWindow::pendingSnapshotPreviewByteSize() const noexcept
{
    return pendingSnapshotPreview_.isNull()
        ? 0
        : static_cast<quint64>(pendingSnapshotPreview_.sizeInBytes());
}

quint64 AnswerCardWindow::releasePendingSnapshotPreviewImage() noexcept
{
    const quint64 released = pendingSnapshotPreviewByteSize();
    ++previewGeneration_;
    pendingSnapshotPreview_ = {};
    return released;
}

QString AnswerCardWindow::targetDomain() const
{
    return targetDomain_;
}

void AnswerCardWindow::setServiceChoices(
    QList<AnswerServiceChoice> choices,
    const QUuid& selectedProfileId,
    const QString& selectedModelId)
{
    serviceChoices_ = std::move(choices);
    const QSignalBlocker serviceBlocker(serviceCombo_);
    const QSignalBlocker modelBlocker(modelCombo_);
    serviceCombo_->clear();
    for (const auto& choice : std::as_const(serviceChoices_)) {
        serviceCombo_->addItem(
            choice.displayName.isEmpty() ? tr("未命名服务") : choice.displayName,
            choice.profileId);
    }

    int selectedIndex = -1;
    if (!selectedProfileId.isNull()) {
        selectedIndex = serviceCombo_->findData(selectedProfileId);
    }
    if (selectedIndex < 0 && !serviceChoices_.isEmpty()) selectedIndex = 0;
    serviceCombo_->setCurrentIndex(selectedIndex);
    rebuildModelChoices(selectedModelId);

    if (const auto* choice = selectedServiceChoice(); choice != nullptr) {
        serviceName_ = choice->displayName;
        targetDomain_ = choice->targetDomain;
    }
    modelId_ = modelCombo_->currentText().trimmed();
    serviceCombo_->setEnabled(!serviceChoices_.isEmpty());
    updateHeader();
    if (!pendingSnapshotPreview_.isNull()) {
        setPendingSnapshotPreview(pendingSnapshotPreview_, targetDomain_);
    }
}

QUuid AnswerCardWindow::selectedProfileId() const
{
    return serviceCombo_ != nullptr
        ? serviceCombo_->currentData().toUuid() : QUuid{};
}

QString AnswerCardWindow::selectedModelId() const
{
    return modelCombo_ != nullptr ? modelCombo_->currentText().trimmed() : modelId_;
}

void AnswerCardWindow::setConversationHistory(
    QList<AnswerTurnPresentation> turns)
{
    conversationHistory_ = std::move(turns);
    refreshAnswerView();
}

const QList<AnswerTurnPresentation>&
AnswerCardWindow::conversationHistory() const noexcept
{
    return conversationHistory_;
}

void AnswerCardWindow::setHasUnsentChanges(const bool hasChanges)
{
    hasUnsentChanges_ = hasChanges;
    if (unsentChangesLabel_ == nullptr) return;
    unsentChangesLabel_->setVisible(hasUnsentChanges_);
    unsentChangesLabel_->setText(
        tr("图片已修改，有未发送修改；下一次发送将创建新版本。"));
}

bool AnswerCardWindow::hasUnsentChanges() const noexcept
{
    return hasUnsentChanges_;
}

void AnswerCardWindow::setLinkedToEditor(const bool linked)
{
    if (linkedToEditor_ == linked) return;
    linkedToEditor_ = linked;
    if (linkToggleButton_ != nullptr) {
        const QSignalBlocker blocker(linkToggleButton_);
        linkToggleButton_->setChecked(linkedToEditor_);
    }
    updateLinkControl();
    emit linkToEditorChanged(linkedToEditor_);
}

bool AnswerCardWindow::isLinkedToEditor() const noexcept
{
    return linkedToEditor_;
}

void AnswerCardWindow::focusQuestionInput()
{
    composerRequested_ = true;
    updateControls();
    show();
    raise();
    activateWindow();
    questionEdit_->setFocus(Qt::OtherFocusReason);
}

void AnswerCardWindow::beginRequest(const QUuid& requestId)
{
    flushPendingText();
    activeRequestId_ = requestId;
    activeAnswerNumber_ = nextAnswerNumber();
    activeAnswerSnapshotVersion_ = snapshotVersion_;
    activeAnswerQuestion_ = questionEdit_->toPlainText();
    activeAnswerServiceName_ = serviceName();
    activeAnswerModelId_ = modelId();
    hasActiveAnswer_ = !requestId.isNull();
    inputTokens_ = -1;
    outputTokens_ = -1;
    acceptingEvents_ = !requestId.isNull();
    composerRequested_ = false;
    refreshAnswerView();
    setState(
        acceptingEvents_ ? AnswerCardState::Sending : AnswerCardState::Failed,
        acceptingEvents_ ? QString{} : tr("无法开始请求：请求标识无效。"));
}

void AnswerCardWindow::consumeStreamEvent(const snapask::ai::AiStreamEvent& event)
{
    if (activeRequestId_.isNull()) {
        if (event.type != snapask::ai::EventType::Started || event.requestId.isNull()) {
            return;
        }
        beginRequest(event.requestId);
    }
    if (event.requestId != activeRequestId_ || !acceptingEvents_) {
        return;
    }

    switch (event.type) {
    case snapask::ai::EventType::Started:
        setState(AnswerCardState::Sending);
        break;
    case snapask::ai::EventType::TextDelta:
        appendTextDelta(event.text);
        break;
    case snapask::ai::EventType::UsageUpdated:
        if (event.inputTokens >= 0) {
            inputTokens_ = event.inputTokens;
        }
        if (event.outputTokens >= 0) {
            outputTokens_ = event.outputTokens;
        }
        updateStatusLabel();
        break;
    case snapask::ai::EventType::Completed:
        completeRequest();
        break;
    case snapask::ai::EventType::Cancelled:
        cancelRequest();
        break;
    case snapask::ai::EventType::Failed:
        failRequest(
            event.errorMessage.isEmpty() ? tr("回答生成失败。") : event.errorMessage);
        break;
    }
}

void AnswerCardWindow::appendTextDelta(const QString& delta)
{
    if (!acceptingEvents_ || delta.isEmpty()) {
        return;
    }

    pendingText_.append(delta);
    if (state_ != AnswerCardState::Streaming) {
        setState(AnswerCardState::Streaming);
    } else {
        updateControls();
    }
    if (!batchTimer_->isActive()) {
        batchTimer_->start();
    }
}

void AnswerCardWindow::completeRequest()
{
    if (!acceptingEvents_) {
        return;
    }
    flushPendingText();
    acceptingEvents_ = false;
    setState(AnswerCardState::Completed);
}

void AnswerCardWindow::failRequest(const QString& message)
{
    if (!acceptingEvents_) {
        return;
    }
    flushPendingText();
    acceptingEvents_ = false;
    setState(
        AnswerCardState::Failed,
        message.isEmpty() ? tr("回答生成失败。") : message);
}

void AnswerCardWindow::cancelRequest()
{
    if (!acceptingEvents_) {
        return;
    }
    flushPendingText();
    acceptingEvents_ = false;
    setState(AnswerCardState::Cancelled, tr("生成已取消。"));
}

bool AnswerCardWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == questionEdit_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool isEnter =
            keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        if (isEnter && keyEvent->modifiers().testFlag(Qt::ControlModifier)) {
            requestSend();
            keyEvent->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AnswerCardWindow::closeEvent(QCloseEvent* event)
{
    if (acceptingEvents_ && !activeRequestId_.isNull()) {
        emit stopRequested(activeRequestId_);
    }
    event->accept();
}

void AnswerCardWindow::requestSend()
{
    if (acceptingEvents_) {
        return;
    }
    const QString currentQuestion = questionEdit_->toPlainText();
    if (currentQuestion.trimmed().isEmpty()) {
        return;
    }
    emit sendRequested(currentQuestion);
}

void AnswerCardWindow::requestStop()
{
    if (!acceptingEvents_ || activeRequestId_.isNull()) {
        return;
    }
    emit stopRequested(activeRequestId_);
}

void AnswerCardWindow::requestRetry()
{
    if (acceptingEvents_ || activeRequestId_.isNull()) {
        return;
    }
    const QString currentQuestion = questionEdit_->toPlainText();
    if (currentQuestion.trimmed().isEmpty()) {
        return;
    }
    emit retryRequested(activeRequestId_, currentQuestion);
}

void AnswerCardWindow::requestRetryCurrent()
{
    if (acceptingEvents_ || activeRequestId_.isNull()) return;
    const QString currentQuestion = questionEdit_->toPlainText();
    if (currentQuestion.trimmed().isEmpty()) return;
    emit retryCurrentRequested(activeRequestId_, currentQuestion);
}

void AnswerCardWindow::copyAnswer()
{
    flushPendingText();
    QString textToCopy;
    if (answerView_ != nullptr) {
        const auto browsers = answerView_->findChildren<QTextBrowser*>(
            QStringLiteral("answerMarkdownBlock"));
        for (const QTextBrowser* browser : browsers) {
            if (browser->textCursor().hasSelection()) {
                textToCopy = browser->textCursor().selectedText();
                textToCopy.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
                break;
            }
        }
    }
    if (textToCopy.isEmpty()) {
        textToCopy = answerMarkdown_;
    }
    if (textToCopy.isEmpty()) {
        return;
    }
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr) {
        clipboard->setText(textToCopy, QClipboard::Clipboard);
    }
    emit copyAnswerRequested(textToCopy);
}

void AnswerCardWindow::flushPendingText()
{
    batchTimer_->stop();
    if (pendingText_.isEmpty()) {
        return;
    }
    answerMarkdown_.append(pendingText_);
    pendingText_.clear();
    refreshAnswerView();
    updateControls();
}

void AnswerCardWindow::buildCompactUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 10, 12, 12);
    rootLayout->setSpacing(8);
    const QColor iconColor = palette().color(QPalette::WindowText);

    auto* titleLayout = new QHBoxLayout();
    auto* title = new QLabel(tr("SnapAsk"), this);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    title->setFont(titleFont);
    titleLayout->addWidget(title);
    titleLayout->addStretch(1);
    versionLabel_ = new QLabel(this);
    versionLabel_->setObjectName(QStringLiteral("answerVersionLabel"));
    titleLayout->addWidget(versionLabel_);
    auto* closeButton = new QPushButton(this);
    closeButton->setObjectName(QStringLiteral("answerCloseButton"));
    closeButton->setIcon(glyphIcon(Glyph::Close, iconColor));
    closeButton->setFlat(true);
    closeButton->setFixedSize(32, 32);
    closeButton->setToolTip(tr("关闭回答"));
    closeButton->setAccessibleName(tr("关闭回答"));
    titleLayout->addWidget(closeButton);
    rootLayout->addLayout(titleLayout);

    auto* hiddenSelectionPanel = new QFrame(this);
    auto* hiddenSelectionLayout = new QHBoxLayout(hiddenSelectionPanel);
    serviceLabel_ = new QLabel(tr("服务"), hiddenSelectionPanel);
    serviceLabel_->setObjectName(QStringLiteral("answerServiceLabel"));
    serviceCombo_ = new QComboBox(hiddenSelectionPanel);
    serviceCombo_->setObjectName(QStringLiteral("answerServiceCombo"));
    serviceCombo_->setAccessibleName(tr("下一轮 AI 服务"));
    modelLabel_ = new QLabel(tr("模型"), hiddenSelectionPanel);
    modelLabel_->setObjectName(QStringLiteral("answerModelLabel"));
    modelCombo_ = new QComboBox(hiddenSelectionPanel);
    modelCombo_->setObjectName(QStringLiteral("answerModelCombo"));
    modelCombo_->setAccessibleName(tr("下一轮 AI 模型"));
    modelCombo_->setEditable(true);
    modelCombo_->setInsertPolicy(QComboBox::NoInsert);
    hiddenSelectionLayout->addWidget(serviceLabel_);
    hiddenSelectionLayout->addWidget(serviceCombo_);
    hiddenSelectionLayout->addWidget(modelLabel_);
    hiddenSelectionLayout->addWidget(modelCombo_);
    rootLayout->addWidget(hiddenSelectionPanel);
    hiddenSelectionPanel->hide();

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("answerStatusLabel"));
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setWordWrap(true);
    rootLayout->addWidget(statusLabel_);

    unsentChangesLabel_ = new QLabel(this);
    unsentChangesLabel_->setObjectName(QStringLiteral("answerUnsentChangesLabel"));
    unsentChangesLabel_->setTextFormat(Qt::PlainText);
    unsentChangesLabel_->setWordWrap(true);
    unsentChangesLabel_->hide();
    rootLayout->addWidget(unsentChangesLabel_);

    linkToggleButton_ = new QPushButton(this);
    linkToggleButton_->setObjectName(QStringLiteral("answerLinkToggleButton"));
    linkToggleButton_->setCheckable(true);
    linkToggleButton_->setChecked(linkedToEditor_);
    rootLayout->addWidget(linkToggleButton_);
    linkToggleButton_->hide();
    updateLinkControl();

    answerSurface_ = new QFrame(this);
    answerSurface_->setObjectName(QStringLiteral("GlassCard"));
    auto* answerLayout = new QGridLayout(answerSurface_);
    answerLayout->setContentsMargins(4, 4, 4, 4);
    answerView_ = new MarkdownAnswerView(answerSurface_);
    answerView_->setLinkHandler([this](const QUrl& url) {
        if (url.isValid()) {
            emit externalLinkRequested(url);
        }
    });
    answerView_->setMouseTracking(true);
    answerView_->viewport()->setMouseTracking(true);
    answerLayout->addWidget(answerView_, 0, 0);

    copyButton_ = new QPushButton(answerSurface_);
    copyButton_->setObjectName(QStringLiteral("answerCopyButton"));
    copyButton_->setIcon(glyphIcon(Glyph::Copy, iconColor));
    copyButton_->setFixedSize(34, 34);
    copyButton_->setToolTip(tr("复制回答；选中文字时只复制选中内容"));
    copyButton_->setAccessibleName(tr("复制回答"));
    copyButton_->hide();
    answerLayout->addWidget(
        copyButton_, 0, 0, Qt::AlignTop | Qt::AlignRight);
    rootLayout->addWidget(answerSurface_, 1);

    snapshotPreviewPanel_ = new QFrame(this);
    snapshotPreviewPanel_->setObjectName(QStringLiteral("pendingSnapshotPanel"));
    auto* previewLayout = new QHBoxLayout(snapshotPreviewPanel_);
    snapshotPreviewLabel_ = new QLabel(snapshotPreviewPanel_);
    snapshotPreviewLabel_->setObjectName(QStringLiteral("pendingSnapshotPreview"));
    snapshotPreviewLabel_->setFixedSize(128, 80);
    snapshotPreviewLabel_->setAlignment(Qt::AlignCenter);
    targetDomainLabel_ = new QLabel(snapshotPreviewPanel_);
    targetDomainLabel_->setObjectName(QStringLiteral("pendingSnapshotTargetDomain"));
    targetDomainLabel_->setWordWrap(true);
    previewLayout->addWidget(snapshotPreviewLabel_);
    previewLayout->addWidget(targetDomainLabel_, 1);
    rootLayout->addWidget(snapshotPreviewPanel_);
    snapshotPreviewPanel_->hide();

    composerPanel_ = new QFrame(this);
    composerPanel_->setObjectName(QStringLiteral("GlassCard"));
    auto* composerLayout = new QHBoxLayout(composerPanel_);
    composerLayout->setContentsMargins(7, 6, 7, 6);
    composerLayout->setSpacing(6);
    questionEdit_ = new QPlainTextEdit(composerPanel_);
    questionEdit_->setObjectName(QStringLiteral("answerQuestionEdit"));
    questionEdit_->setPlaceholderText(tr("问问这张截图…  Ctrl+Enter 发送"));
    questionEdit_->setTabChangesFocus(true);
    questionEdit_->setMinimumHeight(52);
    questionEdit_->setMaximumHeight(92);
    questionEdit_->installEventFilter(this);
    composerLayout->addWidget(questionEdit_, 1);

    auto* actionColumn = new QVBoxLayout();
    sendButton_ = new QPushButton(composerPanel_);
    sendButton_->setObjectName(QStringLiteral("answerSendButton"));
    sendButton_->setIcon(glyphIcon(Glyph::Send, iconColor));
    sendButton_->setFixedSize(36, 36);
    sendButton_->setToolTip(tr("发送"));
    sendButton_->setAccessibleName(tr("发送"));
    sendButton_->setDefault(true);
    stopButton_ = new QPushButton(composerPanel_);
    stopButton_->setObjectName(QStringLiteral("answerStopButton"));
    stopButton_->setIcon(glyphIcon(Glyph::Stop, iconColor));
    stopButton_->setFixedSize(36, 36);
    stopButton_->setToolTip(tr("停止生成"));
    stopButton_->setAccessibleName(tr("停止生成"));
    actionColumn->addWidget(sendButton_);
    actionColumn->addWidget(stopButton_);
    actionColumn->addStretch(1);
    composerLayout->addLayout(actionColumn);
    rootLayout->addWidget(composerPanel_);

    auto* hiddenCommandPanel = new QFrame(this);
    auto* hiddenCommandLayout = new QHBoxLayout(hiddenCommandPanel);
    retryButton_ = new QPushButton(tr("重试原快照"), hiddenCommandPanel);
    retryButton_->setObjectName(QStringLiteral("answerRetryButton"));
    retryCurrentButton_ = new QPushButton(
        tr("用当前截图重新发送"), hiddenCommandPanel);
    retryCurrentButton_->setObjectName(
        QStringLiteral("answerRetryCurrentButton"));
    hiddenCommandLayout->addWidget(retryButton_);
    hiddenCommandLayout->addWidget(retryCurrentButton_);
    rootLayout->addWidget(hiddenCommandPanel);
    hiddenCommandPanel->hide();

    connect(closeButton, &QPushButton::clicked, this, &AnswerCardWindow::close);
    connect(linkToggleButton_, &QPushButton::toggled, this, [this](bool linked) {
        if (linkedToEditor_ == linked) return;
        linkedToEditor_ = linked;
        updateLinkControl();
        emit linkToEditorChanged(linkedToEditor_);
    });
    connect(sendButton_, &QPushButton::clicked, this, &AnswerCardWindow::requestSend);
    connect(stopButton_, &QPushButton::clicked, this, &AnswerCardWindow::requestStop);
    connect(retryButton_, &QPushButton::clicked, this, &AnswerCardWindow::requestRetry);
    connect(retryCurrentButton_, &QPushButton::clicked,
            this, &AnswerCardWindow::requestRetryCurrent);
    connect(copyButton_, &QPushButton::clicked, this, &AnswerCardWindow::copyAnswer);
    connect(questionEdit_, &QPlainTextEdit::textChanged,
            this, &AnswerCardWindow::updateControls);
    connect(serviceCombo_, &QComboBox::currentIndexChanged,
            this, &AnswerCardWindow::serviceSelectionChanged);
    connect(modelCombo_, &QComboBox::currentTextChanged,
            this, &AnswerCardWindow::modelSelectionChanged);
}

void AnswerCardWindow::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 11, 12, 12);
    rootLayout->setSpacing(8);

    auto* titleLayout = new QHBoxLayout();
    titleLayout->setSpacing(7);
    auto* title = new QLabel(tr("SnapAsk"), this);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    title->setFont(titleFont);
    titleLayout->addWidget(title);
    titleLayout->addStretch(1);
    versionLabel_ = new QLabel(this);
    versionLabel_->setObjectName(QStringLiteral("answerVersionLabel"));
    versionLabel_->setTextFormat(Qt::PlainText);
    titleLayout->addWidget(versionLabel_);
    auto* closeButton = new QPushButton(this);
    closeButton->setObjectName(QStringLiteral("answerCloseButton"));
    closeButton->setText(QStringLiteral("×"));
    closeButton->setFlat(true);
    closeButton->setFixedSize(32, 32);
    closeButton->setToolTip(tr("关闭回答"));
    closeButton->setAccessibleName(tr("关闭回答"));
    titleLayout->addWidget(closeButton);
    rootLayout->addLayout(titleLayout);

    // Service/model choice remains an internal compatibility surface for the
    // controller and tests. It is deliberately absent from the visible card;
    // the default service and model are selected only in Settings.
    serviceLabel_ = new QLabel(this);
    serviceLabel_->setObjectName(QStringLiteral("answerServiceLabel"));
    serviceLabel_->hide();
    serviceCombo_ = new QComboBox(this);
    serviceCombo_->setObjectName(QStringLiteral("answerServiceCombo"));
    serviceCombo_->setAccessibleName(tr("下一轮 AI 服务"));
    serviceCombo_->hide();
    modelLabel_ = new QLabel(this);
    modelLabel_->setObjectName(QStringLiteral("answerModelLabel"));
    modelLabel_->hide();
    modelCombo_ = new QComboBox(this);
    modelCombo_->setObjectName(QStringLiteral("answerModelCombo"));
    modelCombo_->setAccessibleName(tr("下一轮 AI 模型"));
    modelCombo_->setEditable(true);
    modelCombo_->setInsertPolicy(QComboBox::NoInsert);
    modelCombo_->hide();

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("answerStatusLabel"));
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(statusLabel_);

    unsentChangesLabel_ = new QLabel(this);
    unsentChangesLabel_->setObjectName(QStringLiteral("answerUnsentChangesLabel"));
    unsentChangesLabel_->setTextFormat(Qt::PlainText);
    unsentChangesLabel_->setWordWrap(true);
    unsentChangesLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unsentChangesLabel_->hide();
    rootLayout->addWidget(unsentChangesLabel_);

    linkToggleButton_ = new QPushButton(this);
    linkToggleButton_->setObjectName(QStringLiteral("answerLinkToggleButton"));
    linkToggleButton_->setCheckable(true);
    linkToggleButton_->setChecked(linkedToEditor_);
    linkToggleButton_->hide();
    updateLinkControl();
    connect(linkToggleButton_, &QPushButton::toggled, this, [this](const bool linked) {
        if (linkedToEditor_ == linked) return;
        linkedToEditor_ = linked;
        updateLinkControl();
        emit linkToEditorChanged(linkedToEditor_);
    });

    answerSurface_ = new QFrame(this);
    answerSurface_->setObjectName(QStringLiteral("GlassCard"));
    auto* answerLayout = new QVBoxLayout(answerSurface_);
    answerLayout->setContentsMargins(4, 4, 4, 4);
    answerView_ = new MarkdownAnswerView(answerSurface_);
    answerView_->setLinkHandler([this](const QUrl& url) {
        if (url.isValid()) {
            emit externalLinkRequested(url);
        }
    });
    answerView_->setMouseTracking(true);
    answerView_->viewport()->setMouseTracking(true);
    answerLayout->addWidget(answerView_, 1);
    rootLayout->addWidget(answerSurface_, 1);

    copyButton_ = new QPushButton(answerSurface_);
    copyButton_->setObjectName(QStringLiteral("answerCopyButton"));
    copyButton_->setText(QStringLiteral("⧉"));
    copyButton_->setFixedSize(34, 34);
    copyButton_->setToolTip(tr("复制回答；选中文字时只复制选中内容"));
    copyButton_->setAccessibleName(tr("复制回答"));
    copyButton_->hide();

    snapshotPreviewPanel_ = new QFrame(this);
    snapshotPreviewPanel_->setObjectName(QStringLiteral("pendingSnapshotPanel"));
    static_cast<QFrame*>(snapshotPreviewPanel_)->setFrameShape(QFrame::StyledPanel);
    auto* previewLayout = new QHBoxLayout(snapshotPreviewPanel_);
    previewLayout->setContentsMargins(8, 6, 8, 6);
    previewLayout->setSpacing(10);

    snapshotPreviewLabel_ = new QLabel(snapshotPreviewPanel_);
    snapshotPreviewLabel_->setObjectName(QStringLiteral("pendingSnapshotPreview"));
    snapshotPreviewLabel_->setFixedSize(128, 80);
    snapshotPreviewLabel_->setAlignment(Qt::AlignCenter);
    snapshotPreviewLabel_->setTextFormat(Qt::PlainText);
    previewLayout->addWidget(snapshotPreviewLabel_);

    targetDomainLabel_ = new QLabel(snapshotPreviewPanel_);
    targetDomainLabel_->setObjectName(QStringLiteral("pendingSnapshotTargetDomain"));
    targetDomainLabel_->setTextFormat(Qt::PlainText);
    targetDomainLabel_->setWordWrap(true);
    targetDomainLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewLayout->addWidget(targetDomainLabel_, 1);
    snapshotPreviewPanel_->hide();

    composerPanel_ = new QFrame(this);
    composerPanel_->setObjectName(QStringLiteral("GlassCard"));
    auto* composerLayout = new QHBoxLayout(composerPanel_);
    composerLayout->setContentsMargins(7, 6, 7, 6);
    composerLayout->setSpacing(6);
    questionEdit_ = new QPlainTextEdit(composerPanel_);
    questionEdit_->setObjectName(QStringLiteral("answerQuestionEdit"));
    questionEdit_->setPlaceholderText(tr("问问这张截图…  Ctrl+Enter 发送"));
    questionEdit_->setTabChangesFocus(true);
    questionEdit_->setMinimumHeight(52);
    questionEdit_->setMaximumHeight(92);
    questionEdit_->installEventFilter(this);
    composerLayout->addWidget(questionEdit_, 1);

    auto* actionColumn = new QVBoxLayout();
    actionColumn->setSpacing(4);
    sendButton_ = new QPushButton(composerPanel_);
    sendButton_->setObjectName(QStringLiteral("answerSendButton"));
    sendButton_->setText(QStringLiteral("↑"));
    sendButton_->setFixedSize(36, 36);
    sendButton_->setToolTip(tr("发送"));
    sendButton_->setAccessibleName(tr("发送"));
    sendButton_->setDefault(true);
    stopButton_ = new QPushButton(composerPanel_);
    stopButton_->setObjectName(QStringLiteral("answerStopButton"));
    stopButton_->setText(QStringLiteral("■"));
    stopButton_->setFixedSize(36, 36);
    stopButton_->setToolTip(tr("停止生成"));
    stopButton_->setAccessibleName(tr("停止生成"));
    actionColumn->addWidget(sendButton_);
    actionColumn->addWidget(stopButton_);
    actionColumn->addStretch(1);
    composerLayout->addLayout(actionColumn);
    rootLayout->addWidget(composerPanel_);

    // Retry operations are intentionally no longer presented in the product
    // UI. Keeping the command objects hidden preserves stable request semantics
    // for older sessions and regression tests without adding visual clutter.
    retryButton_ = new QPushButton(tr("重试原快照"), this);
    retryButton_->setObjectName(QStringLiteral("answerRetryButton"));
    retryButton_->setToolTip(tr("使用原请求冻结的截图版本和原问题再次发送"));
    retryCurrentButton_ = new QPushButton(tr("用当前截图重新发送"), this);
    retryCurrentButton_->setObjectName(QStringLiteral("answerRetryCurrentButton"));
    retryCurrentButton_->setToolTip(tr("重新渲染当前截图并创建新的截图版本"));
    retryButton_->hide();
    retryCurrentButton_->hide();

    connect(sendButton_, &QPushButton::clicked, this, &AnswerCardWindow::requestSend);
    connect(stopButton_, &QPushButton::clicked, this, &AnswerCardWindow::requestStop);
    connect(retryButton_, &QPushButton::clicked, this, &AnswerCardWindow::requestRetry);
    connect(retryCurrentButton_, &QPushButton::clicked,
            this, &AnswerCardWindow::requestRetryCurrent);
    connect(copyButton_, &QPushButton::clicked, this, &AnswerCardWindow::copyAnswer);
    connect(closeButton, &QPushButton::clicked, this, &AnswerCardWindow::close);
    connect(questionEdit_, &QPlainTextEdit::textChanged, this, &AnswerCardWindow::updateControls);
    connect(serviceCombo_, &QComboBox::currentIndexChanged,
            this, &AnswerCardWindow::serviceSelectionChanged);
    connect(modelCombo_, &QComboBox::currentTextChanged,
            this, &AnswerCardWindow::modelSelectionChanged);
}

void AnswerCardWindow::setState(const AnswerCardState state, const QString& detail)
{
    const bool changed = state_ != state;
    state_ = state;
    if (state == AnswerCardState::Failed || state == AnswerCardState::Cancelled) {
        errorMessage_ = detail;
    } else {
        errorMessage_.clear();
    }
    updateStatusLabel();
    updateControls();
    if (changed) {
        emit stateChanged(state_);
    }
}

void AnswerCardWindow::updateHeader()
{
    if (serviceChoices_.isEmpty() && serviceCombo_ != nullptr) {
        const QSignalBlocker serviceBlocker(serviceCombo_);
        const QSignalBlocker modelBlocker(modelCombo_);
        serviceCombo_->clear();
        serviceCombo_->addItem(
            serviceName_.isEmpty() ? tr("未选择") : serviceName_, QUuid{});
        serviceCombo_->setCurrentIndex(0);
        serviceCombo_->setEnabled(false);
        modelCombo_->clear();
        modelCombo_->setEditText(modelId_);
    }
    versionLabel_->setText(tr("截图 v%1").arg(snapshotVersion_));
}

void AnswerCardWindow::updateControls()
{
    const bool busy = acceptingEvents_ &&
                      (state_ == AnswerCardState::Sending ||
                       state_ == AnswerCardState::Streaming);
    const bool hasQuestion = !questionEdit_->toPlainText().trimmed().isEmpty();
    const bool hasAnswer = !answerMarkdown_.isEmpty() || !pendingText_.isEmpty();
    const bool terminal = state_ == AnswerCardState::Completed ||
                          state_ == AnswerCardState::Failed ||
                          state_ == AnswerCardState::Cancelled;

    sendButton_->setEnabled(!busy && hasQuestion);
    stopButton_->setEnabled(busy && !activeRequestId_.isNull());
    retryButton_->setEnabled(
        !busy && terminal && !activeRequestId_.isNull() && hasQuestion);
    retryCurrentButton_->setEnabled(
        !busy && terminal && !activeRequestId_.isNull() && hasQuestion);
    copyButton_->setEnabled(hasAnswer);
    serviceCombo_->setEnabled(!busy && !serviceChoices_.isEmpty());
    modelCombo_->setEnabled(!busy);
    if (composerPanel_ != nullptr) {
        composerPanel_->setVisible(busy || composerRequested_);
        questionEdit_->setVisible(!busy && composerRequested_);
        sendButton_->setVisible(!busy && composerRequested_);
        stopButton_->setVisible(busy);
    }
}

void AnswerCardWindow::refreshAnswerView()
{
    if (answerView_ == nullptr) return;
    answerView_->setConversation(
        conversationHistory_,
        answerMarkdown_,
        hasActiveAnswer_,
        activeAnswerNumber_,
        activeAnswerSnapshotVersion_,
        activeAnswerQuestion_,
        activeAnswerServiceName_,
        activeAnswerModelId_);
    installAnswerViewEventFilters();
    updateAnswerCopyButton();
}

void AnswerCardWindow::installAnswerViewEventFilters()
{
    if (answerView_ == nullptr) {
        return;
    }
    const auto browsers = answerView_->findChildren<QTextBrowser*>(
        QStringLiteral("answerMarkdownBlock"));
    for (QTextBrowser* browser : browsers) {
        browser->setMouseTracking(true);
        connect(browser, &QTextBrowser::selectionChanged, this, [this, browser] {
            updateAnswerCopyButton();
        });
    }
}

void AnswerCardWindow::updateAnswerCopyButton()
{
    if (copyButton_ == nullptr || answerView_ == nullptr) {
        return;
    }
    bool hasSelection = false;
    const auto browsers = answerView_->findChildren<QTextBrowser*>(
        QStringLiteral("answerMarkdownBlock"));
    for (const QTextBrowser* browser : browsers) {
        if (browser->textCursor().hasSelection()) {
            hasSelection = true;
            break;
        }
    }
    const bool hasAnswer = !answerMarkdown_.isEmpty() || !pendingText_.isEmpty();
    copyButton_->setVisible(hasAnswer && hasSelection);
    if (copyButton_->isVisible()) {
        copyButton_->raise();
    }
}

void AnswerCardWindow::updateLinkControl()
{
    if (linkToggleButton_ == nullptr) return;
    linkToggleButton_->setText(
        linkedToEditor_ ? tr("与截图联动") : tr("已解除联动"));
    linkToggleButton_->setToolTip(
        linkedToEditor_
            ? tr("回答卡将随截图窗口一起移动、置顶、隐藏和关闭；点击可单独移动回答卡")
            : tr("回答卡可以单独移动；点击恢复与截图窗口联动"));
    linkToggleButton_->setAccessibleName(tr("回答卡与截图窗口联动"));
    linkToggleButton_->setAccessibleDescription(linkToggleButton_->toolTip());
}

quint64 AnswerCardWindow::nextAnswerNumber() const noexcept
{
    quint64 maximum = 0;
    for (const AnswerTurnPresentation& turn : conversationHistory_) {
        maximum = std::max(maximum, turn.answerNumber);
    }
    return maximum == std::numeric_limits<quint64>::max()
        ? maximum : maximum + 1;
}

void AnswerCardWindow::serviceSelectionChanged(const int index)
{
    if (acceptingEvents_ || index < 0 || index >= serviceChoices_.size()) return;
    const auto& choice = serviceChoices_.at(index);
    serviceName_ = choice.displayName;
    targetDomain_ = choice.targetDomain;
    rebuildModelChoices(choice.defaultModelId);
    modelId_ = modelCombo_->currentText().trimmed();
    if (!pendingSnapshotPreview_.isNull()) {
        setPendingSnapshotPreview(pendingSnapshotPreview_, targetDomain_);
    }
    emit selectionChanged(choice.profileId, modelId_);
}

void AnswerCardWindow::modelSelectionChanged(const QString& modelId)
{
    if (acceptingEvents_) return;
    modelId_ = modelId.trimmed();
    const QUuid profileId = selectedProfileId();
    if (!profileId.isNull()) emit selectionChanged(profileId, modelId_);
}

void AnswerCardWindow::rebuildModelChoices(const QString& preferredModel)
{
    const QSignalBlocker blocker(modelCombo_);
    modelCombo_->clear();
    const auto* choice = selectedServiceChoice();
    QStringList models;
    if (choice != nullptr) {
        models = choice->modelIds;
        models.removeAll(QString{});
        models.removeDuplicates();
    }
    QString selected = preferredModel.trimmed();
    if (selected.isEmpty() && choice != nullptr) {
        selected = choice->defaultModelId.trimmed();
    }
    if (!selected.isEmpty() && !models.contains(selected)) models.prepend(selected);
    modelCombo_->addItems(models);
    if (!selected.isEmpty()) modelCombo_->setEditText(selected);
    else if (!models.isEmpty()) modelCombo_->setCurrentIndex(0);
}

const AnswerServiceChoice* AnswerCardWindow::selectedServiceChoice() const
{
    if (serviceCombo_ == nullptr) return nullptr;
    const int index = serviceCombo_->currentIndex();
    return index >= 0 && index < serviceChoices_.size()
        ? &serviceChoices_.at(index) : nullptr;
}

void AnswerCardWindow::updateStatusLabel()
{
    QString status;
    switch (state_) {
    case AnswerCardState::Idle:
        status = tr("尚未发送。截图不会在明确发送前上传。");
        break;
    case AnswerCardState::Sending:
        status = tr("正在连接并发送…");
        break;
    case AnswerCardState::Streaming:
        status = tr("正在生成回答…");
        break;
    case AnswerCardState::Completed:
        status = tr("回答已完成。");
        break;
    case AnswerCardState::Failed:
        status = tr("生成失败：%1").arg(errorMessage_);
        break;
    case AnswerCardState::Cancelled:
        status = errorMessage_.isEmpty() ? tr("生成已取消。") : errorMessage_;
        break;
    }

    QStringList usage;
    if (inputTokens_ >= 0) {
        usage.push_back(tr("输入 %1 tokens").arg(inputTokens_));
    }
    if (outputTokens_ >= 0) {
        usage.push_back(tr("输出 %1 tokens").arg(outputTokens_));
    }
    if (!usage.isEmpty()) {
        status.append(tr("（%1）").arg(usage.join(QStringLiteral("，"))));
    }
    statusLabel_->setText(status);
}

}  // namespace snapask::ui::answer
