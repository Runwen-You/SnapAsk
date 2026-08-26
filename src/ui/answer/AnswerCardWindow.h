#pragma once

#include "ai/AiTypes.h"
#include "ui/glass/GlassSurface.h"

#include <QImage>
#include <QMetaType>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QWidget>

class QCloseEvent;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QObject;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace snapask::ui::answer {

class MarkdownAnswerView;

enum class AnswerCardState {
    Idle,
    Sending,
    Streaming,
    Completed,
    Failed,
    Cancelled,
};

struct AnswerServiceChoice {
    QUuid profileId;
    QString displayName;
    QString targetDomain;
    QStringList modelIds;
    QString defaultModelId;
};

// Presentation-only copy of a completed or superseded conversation turn.
// It deliberately contains no provider credentials or request payload bytes.
struct AnswerTurnPresentation {
    QUuid requestId;
    quint64 answerNumber{0};
    QString question;
    QString answerMarkdown;
    quint64 snapshotVersion{0};
    QString serviceName;
    QString modelId;
    AnswerCardState state{AnswerCardState::Completed};
    QString detail;
};

class AnswerCardWindow final : public snapask::ui::glass::GlassSurface {
    Q_OBJECT

public:
    explicit AnswerCardWindow(QWidget* parent = nullptr);
    ~AnswerCardWindow() override;

    AnswerCardWindow(const AnswerCardWindow&) = delete;
    AnswerCardWindow& operator=(const AnswerCardWindow&) = delete;

    void setRequestContext(
        QString serviceName,
        QString modelId,
        quint64 snapshotVersion);
    [[nodiscard]] QString serviceName() const;
    [[nodiscard]] QString modelId() const;
    [[nodiscard]] quint64 snapshotVersion() const noexcept;

    void setQuestion(const QString& question);
    [[nodiscard]] QString question() const;

    void setAnswerMarkdown(const QString& markdown);
    [[nodiscard]] QString answerMarkdown() const;

    [[nodiscard]] AnswerCardState state() const noexcept;
    [[nodiscard]] QUuid activeRequestId() const noexcept;
    [[nodiscard]] QString errorMessage() const;

    // Shows the exact in-memory snapshot that will be sent after the user
    // explicitly submits the question. Passing a null image clears the
    // preview. This method never performs file or network I/O.
    void setPendingSnapshotPreview(const QImage& image, QString targetDomain);
    [[nodiscard]] const QImage& pendingSnapshotPreview() const noexcept;
    [[nodiscard]] quint64 pendingSnapshotPreviewByteSize() const noexcept;
    quint64 releasePendingSnapshotPreviewImage() noexcept;
    [[nodiscard]] QString targetDomain() const;

    void setServiceChoices(
        QList<AnswerServiceChoice> choices,
        const QUuid& selectedProfileId = {},
        const QString& selectedModelId = {});
    [[nodiscard]] QUuid selectedProfileId() const;
    [[nodiscard]] QString selectedModelId() const;

    // Replaces the visible list of earlier turns. The active turn remains in
    // the streaming pane and is not expected in this list.
    void setConversationHistory(QList<AnswerTurnPresentation> turns);
    [[nodiscard]] const QList<AnswerTurnPresentation>& conversationHistory()
        const noexcept;

    // Driven by comparing the latest canonical RenderedSnapshot hash with the
    // last explicitly sent hash. This setter itself performs no rendering or
    // network operation.
    void setHasUnsentChanges(bool hasChanges);
    [[nodiscard]] bool hasUnsentChanges() const noexcept;

    // The application-level linked-window coordinator owns the actual window
    // movement/visibility policy. The card only presents this explicit user
    // choice and never moves another window itself.
    void setLinkedToEditor(bool linked);
    [[nodiscard]] bool isLinkedToEditor() const noexcept;

public slots:
    // Presents the answer card and puts the caret in the multi-line question
    // editor. Intended for the capture-completed handoff.
    void focusQuestionInput();

    // Call immediately before AiNetworkClient::sendExplicit. Existing question
    // and answer text are intentionally retained.
    void beginRequest(const QUuid& requestId);

    // Can be connected directly to AiNetworkClient::eventReady. Events for an
    // old request or events arriving after a terminal state are ignored.
    void consumeStreamEvent(const snapask::ai::AiStreamEvent& event);
    void appendTextDelta(const QString& delta);
    void completeRequest();
    void failRequest(const QString& message);
    void cancelRequest();

signals:
    void sendRequested(const QString& question);
    void stopRequested(const QUuid& requestId);
    void retryRequested(const QUuid& requestId, const QString& question);
    void retryCurrentRequested(
        const QUuid& requestId,
        const QString& question);
    void copyAnswerRequested(const QString& answerMarkdown);
    void externalLinkRequested(const QUrl& url);
    void stateChanged(snapask::ui::answer::AnswerCardState state);
    void selectionChanged(const QUuid& profileId, const QString& modelId);
    void linkToEditorChanged(bool linked);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void requestSend();
    void requestStop();
    void requestRetry();
    void requestRetryCurrent();
    void copyAnswer();
    void flushPendingText();
    void serviceSelectionChanged(int index);
    void modelSelectionChanged(const QString& modelId);

private:
    void buildUi();
    void buildCompactUi();
    void setState(AnswerCardState state, const QString& detail = {});
    void updateHeader();
    void updateControls();
    void updateStatusLabel();
    void rebuildModelChoices(const QString& preferredModel = {});
    void refreshAnswerView();
    void updateLinkControl();
    void updateAnswerCopyButton();
    void installAnswerViewEventFilters();
    [[nodiscard]] quint64 nextAnswerNumber() const noexcept;
    [[nodiscard]] const AnswerServiceChoice* selectedServiceChoice() const;

    QString serviceName_;
    QString modelId_;
    quint64 snapshotVersion_{0};
    QUuid activeRequestId_;
    AnswerCardState state_{AnswerCardState::Idle};
    QString answerMarkdown_;
    QString pendingText_;
    QString errorMessage_;
    QImage pendingSnapshotPreview_;
    quint64 previewGeneration_{0};
    QString targetDomain_;
    QList<AnswerServiceChoice> serviceChoices_;
    QList<AnswerTurnPresentation> conversationHistory_;
    quint64 activeAnswerNumber_{0};
    quint64 activeAnswerSnapshotVersion_{0};
    QString activeAnswerQuestion_;
    QString activeAnswerServiceName_;
    QString activeAnswerModelId_;
    qint64 inputTokens_{-1};
    qint64 outputTokens_{-1};
    bool acceptingEvents_{false};
    bool hasActiveAnswer_{false};
    bool hasUnsentChanges_{false};
    bool linkedToEditor_{true};
    bool composerRequested_{true};

    QLabel* serviceLabel_{nullptr};
    QLabel* modelLabel_{nullptr};
    QComboBox* serviceCombo_{nullptr};
    QComboBox* modelCombo_{nullptr};
    QLabel* versionLabel_{nullptr};
    QLabel* statusLabel_{nullptr};
    QLabel* unsentChangesLabel_{nullptr};
    QPushButton* linkToggleButton_{nullptr};
    QWidget* snapshotPreviewPanel_{nullptr};
    QLabel* snapshotPreviewLabel_{nullptr};
    QLabel* targetDomainLabel_{nullptr};
    MarkdownAnswerView* answerView_{nullptr};
    QWidget* answerSurface_{nullptr};
    QWidget* composerPanel_{nullptr};
    QPlainTextEdit* questionEdit_{nullptr};
    QPushButton* sendButton_{nullptr};
    QPushButton* stopButton_{nullptr};
    QPushButton* retryButton_{nullptr};
    QPushButton* retryCurrentButton_{nullptr};
    QPushButton* copyButton_{nullptr};
    QTimer* batchTimer_{nullptr};
};

}  // namespace snapask::ui::answer

Q_DECLARE_METATYPE(snapask::ui::answer::AnswerCardState)
