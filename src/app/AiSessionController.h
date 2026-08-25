#pragma once

#include "domain/conversation/ConversationSession.h"
#include "services/SessionMemoryBudget.h"

#include <QObject>
#include <QPointer>

#include <memory>

class QTimer;

namespace snapask::ai {
class AiNetworkClient;
class AiProfileRepository;
struct AiStreamEvent;
struct ProviderProfile;
}

namespace snapask::platform::windows {
class CredentialStore;
}

namespace snapask::infrastructure {
class EndpointConsentStore;
}

namespace snapask::ui::answer {
class AnswerCardWindow;
}

namespace snapask::ui::editor {
class EditorWindow;
}

namespace snapask::ui::windowing {
class LinkedWindowController;
}

namespace snapask::app {

// Bridges one editable screenshot session to the network boundary. Merely
// constructing or showing this controller never sends a request. The sole
// upload path starts in sendCurrentSnapshot(), which is connected only to the
// AnswerCard Send action (including Ctrl+Enter).
class AiSessionController final : public QObject {
    Q_OBJECT

public:
    AiSessionController(
        snapask::ui::editor::EditorWindow* editor,
        snapask::ai::AiNetworkClient* networkClient,
        snapask::ai::AiProfileRepository* profiles,
        snapask::platform::windows::CredentialStore* credentials,
        snapask::infrastructure::EndpointConsentStore* endpointConsent);
    ~AiSessionController() override;

    [[nodiscard]] snapask::ui::answer::AnswerCardWindow* answerWindow() const
        noexcept;
    [[nodiscard]] const snapask::ConversationSession& conversation() const
        noexcept;

public slots:
    void showQuestionCard();
    void reloadProfileChoices();
    void reflowLinkedWindows();

private slots:
    void sendCurrentSnapshot(const QString& question);
    void stopRequest(const QUuid& requestId);
    void retryOriginalSnapshot(
        const QUuid& previousRequestId,
        const QString& question);
    void retryCurrentSnapshot(
        const QUuid& previousRequestId,
        const QString& question);
    void handleNetworkEvent(const snapask::ai::AiStreamEvent& event);
    void handleEditorSnapshotChanged();
    void selectProfileForNextTurn(
        const QUuid& profileId,
        const QString& modelId);

private:
    [[nodiscard]] const snapask::ConversationTurn* beginCurrentTurn(
        const QString& question);
    void dispatchTurn(const snapask::ConversationTurn& turn);
    void failLocally(
        const snapask::ConversationTurn& turn,
        snapask::ai::ErrorKind kind,
        const QString& message);
    void updatePendingPreview();
    void updateUnsentState();
    void synchronizeRebuildableCacheBudget();
    void syncConversationHistory(const QUuid& activeRequestId = {});
    [[nodiscard]] bool prepareSnapshotMemory(
        const snapask::RenderedSnapshot& snapshot,
        snapask::SessionMemoryBudget* prospective,
        QString* error) const;
    void commitSnapshotMemory(snapask::SessionMemoryBudget prospective);
    void positionAnswerCard();
    [[nodiscard]] bool ensureEndpointConsent(
        const snapask::ai::ProviderProfile& profile);

    QPointer<snapask::ui::editor::EditorWindow> editor_;
    QPointer<snapask::ai::AiNetworkClient> networkClient_;
    snapask::ai::AiProfileRepository* profiles_{nullptr};
    snapask::platform::windows::CredentialStore* credentials_{nullptr};
    snapask::infrastructure::EndpointConsentStore* endpointConsent_{nullptr};
    snapask::ui::answer::AnswerCardWindow* answerWindow_{nullptr};
    snapask::ConversationSession conversation_;
    QUuid selectedProfileId_;
    QString selectedModelId_;
    bool profileReloadPending_{false};
    QTimer* pendingPreviewTimer_{nullptr};
    snapask::SessionMemoryBudget memoryBudget_;
    bool sourceMemoryRegistered_{false};
    std::unique_ptr<snapask::ui::windowing::LinkedWindowController>
        windowLink_;
};

}  // namespace snapask::app
