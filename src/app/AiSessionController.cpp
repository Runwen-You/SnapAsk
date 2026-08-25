#include "app/AiSessionController.h"

#include "ai/AiNetworkClient.h"
#include "ai/AiProfileRepository.h"
#include "domain/capture/ScreenshotSession.h"
#include "infrastructure/EndpointConsentStore.h"
#include "platform/windows/CredentialStore.h"
#include "services/SnapshotRenderer.h"
#include "ui/answer/AnswerCardWindow.h"
#include "ui/canvas/CanvasWidget.h"
#include "ui/editor/EditorWindow.h"
#include "ui/windowing/LinkedWindowController.h"

#include <QDesktopServices>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace snapask::app {

namespace {

constexpr qsizetype kMaximumContextTurns = 8;
constexpr auto kCanvasCacheKey = "canvas-presentation";
constexpr auto kPendingPreviewCacheKey = "pending-snapshot-preview";

snapask::ai::AiStreamEvent localFailure(
    const QUuid& requestId,
    const snapask::ai::ErrorKind kind,
    const QString& message)
{
    snapask::ai::AiStreamEvent event;
    event.type = snapask::ai::EventType::Failed;
    event.requestId = requestId;
    event.errorKind = kind;
    event.errorMessage = message;
    return event;
}

}  // namespace

AiSessionController::AiSessionController(
    snapask::ui::editor::EditorWindow* editor,
    snapask::ai::AiNetworkClient* networkClient,
    snapask::ai::AiProfileRepository* profiles,
    snapask::platform::windows::CredentialStore* credentials,
    snapask::infrastructure::EndpointConsentStore* endpointConsent)
    : QObject(editor)
    , editor_(editor)
    , networkClient_(networkClient)
    , profiles_(profiles)
    , credentials_(credentials)
    , endpointConsent_(endpointConsent)
    , answerWindow_(new snapask::ui::answer::AnswerCardWindow())
    , conversation_(editor != nullptr
          ? editor->session().sessionId()
          : QUuid::createUuid())
    , windowLink_(
          std::make_unique<
              snapask::ui::windowing::LinkedWindowController>(this))
{
    Q_ASSERT(editor != nullptr);
    Q_ASSERT(networkClient != nullptr);
    Q_ASSERT(profiles != nullptr);
    Q_ASSERT(credentials != nullptr);
    Q_ASSERT(endpointConsent != nullptr);

    const QImage& sourceImage = editor->session().sourceImage();
    const auto sourceBytes = snapask::SessionMemoryBudget::checkedImageByteSize(
        sourceImage.bytesPerLine(), sourceImage.height());
    if (sourceBytes.has_value() && *sourceBytes > 0) {
        sourceMemoryRegistered_ =
            memoryBudget_.setOriginalImageBytes(*sourceBytes).accepted();
    }

    pendingPreviewTimer_ = new QTimer(this);
    pendingPreviewTimer_->setSingleShot(true);
    pendingPreviewTimer_->setInterval(40);
    connect(pendingPreviewTimer_, &QTimer::timeout,
            this, &AiSessionController::updatePendingPreview);

    connect(editor, &snapask::ui::editor::EditorWindow::askRequested,
            this, &AiSessionController::showQuestionCard);
    connect(answerWindow_, &snapask::ui::answer::AnswerCardWindow::sendRequested,
            this, &AiSessionController::sendCurrentSnapshot);
    connect(answerWindow_, &snapask::ui::answer::AnswerCardWindow::stopRequested,
            this, &AiSessionController::stopRequest);
    connect(answerWindow_, &snapask::ui::answer::AnswerCardWindow::retryRequested,
            this, &AiSessionController::retryOriginalSnapshot);
    connect(answerWindow_, &snapask::ui::answer::AnswerCardWindow::retryCurrentRequested,
            this, &AiSessionController::retryCurrentSnapshot);
    connect(answerWindow_, &snapask::ui::answer::AnswerCardWindow::externalLinkRequested,
            this, [this](const QUrl& url) {
        if (!answerWindow_) return;
        const QString scheme = url.scheme().toLower();
        if ((scheme != QStringLiteral("https")
             && scheme != QStringLiteral("http"))
            || url.host().isEmpty()) {
            QMessageBox::warning(
                answerWindow_, tr("无法打开链接"),
                tr("回答中的链接不是受支持的 HTTP 或 HTTPS 地址。"));
            return;
        }
        const auto choice = QMessageBox::question(
            answerWindow_, tr("打开外部链接？"),
            tr("即将在默认浏览器中打开 %1。\n请确认你信任此地址。")
                .arg(url.host()),
            QMessageBox::Open | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (choice == QMessageBox::Open) {
            QDesktopServices::openUrl(url);
        }
    });
    connect(answerWindow_, &snapask::ui::answer::AnswerCardWindow::selectionChanged,
            this, &AiSessionController::selectProfileForNextTurn);
    connect(answerWindow_,
            &snapask::ui::answer::AnswerCardWindow::linkToEditorChanged,
            this, [this](const bool linked) {
        if (!editor_ || answerWindow_ == nullptr || windowLink_ == nullptr) return;
        if (!linked) {
            windowLink_->setGroupAlwaysOnTop(false);
            windowLink_->unbind();
            const bool wasVisible = answerWindow_->isVisible();
            answerWindow_->setWindowFlag(Qt::WindowStaysOnTopHint, true);
            if (wasVisible) answerWindow_->show();
            return;
        }
        snapask::ui::windowing::LinkedWindowOptions options;
        options.visibilityPolicy =
            snapask::ui::windowing::LinkedVisibilityPolicy::KeepTogether;
        options.closePolicy =
            snapask::ui::windowing::LinkedClosePolicy::CloseTogether;
        options.topmostPolicy =
            snapask::ui::windowing::LinkedTopmostPolicy::KeepTogether;
        if (windowLink_->bind(editor_, answerWindow_, options)) {
            windowLink_->setGroupAlwaysOnTop(true);
            windowLink_->reflow();
        }
    });
    connect(networkClient, &snapask::ai::AiNetworkClient::eventReady,
            this, &AiSessionController::handleNetworkEvent);
    connect(editor, &snapask::ui::editor::EditorWindow::snapshotContentChanged,
            this, &AiSessionController::handleEditorSnapshotChanged);
    snapask::ui::windowing::LinkedWindowOptions linkOptions;
    linkOptions.visibilityPolicy =
        snapask::ui::windowing::LinkedVisibilityPolicy::KeepTogether;
    linkOptions.closePolicy =
        snapask::ui::windowing::LinkedClosePolicy::CloseTogether;
    linkOptions.topmostPolicy =
        snapask::ui::windowing::LinkedTopmostPolicy::KeepTogether;
    (void)windowLink_->bind(editor, answerWindow_, linkOptions);
    reloadProfileChoices();
    syncConversationHistory();
}

AiSessionController::~AiSessionController()
{
    if (networkClient_) {
        networkClient_->cancelSession(conversation_.sessionId());
    }
    if (windowLink_ != nullptr) windowLink_->unbind();
    delete answerWindow_;
    answerWindow_ = nullptr;
}

snapask::ui::answer::AnswerCardWindow*
AiSessionController::answerWindow() const noexcept
{
    return answerWindow_;
}

const snapask::ConversationSession&
AiSessionController::conversation() const noexcept
{
    return conversation_;
}

void AiSessionController::showQuestionCard()
{
    if (!editor_ || answerWindow_ == nullptr) return;
    reloadProfileChoices();
    updatePendingPreview();
    positionAnswerCard();
    answerWindow_->show();
    if (windowLink_ != nullptr && windowLink_->isBound()) {
        windowLink_->setGroupAlwaysOnTop(true);
        windowLink_->reflow();
    }
    answerWindow_->raise();
    answerWindow_->activateWindow();
    answerWindow_->focusQuestionInput();
}

void AiSessionController::reflowLinkedWindows()
{
    if (windowLink_ != nullptr && windowLink_->isBound()) {
        windowLink_->reflow();
    }
}

void AiSessionController::reloadProfileChoices()
{
    if (profiles_ == nullptr || answerWindow_ == nullptr) return;
    if (editor_ != nullptr && editor_->isGenerationActive()) {
        profileReloadPending_ = true;
        return;
    }
    profileReloadPending_ = false;
    const QList<snapask::ai::ProviderProfile> profiles = profiles_->profiles();
    bool selectedStillExists = false;
    for (const auto& profile : profiles) {
        if (profile.id == selectedProfileId_) {
            selectedStillExists = true;
            break;
        }
    }
    if (!selectedStillExists) {
        selectedProfileId_ = profiles_->defaultProfileId();
        selectedModelId_.clear();
    }

    QList<snapask::ui::answer::AnswerServiceChoice> choices;
    choices.reserve(profiles.size());
    for (const auto& profile : profiles) {
        QStringList models = profile.availableModels;
        if (!profile.modelId.trimmed().isEmpty()
            && !models.contains(profile.modelId.trimmed())) {
            models.prepend(profile.modelId.trimmed());
        }
        QString targetDomain = profile.baseUrl.host();
        if (profile.baseUrl.port() >= 0) {
            targetDomain += QStringLiteral(":%1").arg(profile.baseUrl.port());
        }
        choices.append({
            profile.id,
            profile.displayName,
            targetDomain,
            models,
            profile.modelId,
        });
        if (profile.id == selectedProfileId_ && selectedModelId_.isEmpty()) {
            selectedModelId_ = profile.modelId;
        }
    }
    answerWindow_->setServiceChoices(
        std::move(choices), selectedProfileId_, selectedModelId_);
    selectedProfileId_ = answerWindow_->selectedProfileId();
    selectedModelId_ = answerWindow_->selectedModelId();
}

void AiSessionController::selectProfileForNextTurn(
    const QUuid& profileId,
    const QString& modelId)
{
    if (profiles_ == nullptr || !profiles_->profile(profileId).has_value()) return;
    selectedProfileId_ = profileId;
    selectedModelId_ = modelId.trimmed();
    updatePendingPreview();
}

void AiSessionController::sendCurrentSnapshot(const QString& question)
{
    const snapask::ConversationTurn* turn = beginCurrentTurn(question);
    if (turn == nullptr) return;
    dispatchTurn(*turn);
}

void AiSessionController::stopRequest(const QUuid& requestId)
{
    if (networkClient_ && conversation_.turnForRequest(requestId) != nullptr) {
        networkClient_->cancel(requestId);
    }
}

void AiSessionController::retryOriginalSnapshot(
    const QUuid& previousRequestId,
    const QString& question)
{
    Q_UNUSED(question)
    const snapask::ConversationTurn* previous =
        conversation_.turnForRequest(previousRequestId);
    if (previous == nullptr || profiles_ == nullptr) return;
    const auto originalProfile = profiles_->profile(previous->providerProfileId());
    if (!originalProfile.has_value()) {
        if (answerWindow_ != nullptr) {
            QMessageBox::warning(
                answerWindow_, tr("无法重试"),
                tr("原请求使用的服务档案已不存在。"));
        }
        return;
    }
    // A retry is another explicit upload.  Re-check the current endpoint before
    // creating a turn so a changed custom endpoint cannot receive the frozen
    // screenshot without its own consent.
    if (!ensureEndpointConsent(*originalProfile)) return;

    const snapask::ConversationTurn* turn =
        conversation_.retryOriginal(previousRequestId);
    if (turn == nullptr || answerWindow_ == nullptr) return;

    selectedProfileId_ = turn->providerProfileId();
    selectedModelId_ = turn->modelId();
    reloadProfileChoices();
    const auto& revision = *turn->snapshotRevision();
    syncConversationHistory(turn->requestId());
    answerWindow_->setAnswerMarkdown({});
    answerWindow_->setQuestion(turn->question());
    const auto retryProfile = profiles_ != nullptr
        ? profiles_->profile(turn->providerProfileId())
        : std::optional<snapask::ai::ProviderProfile>{};
    answerWindow_->setRequestContext(
        retryProfile.has_value() ? retryProfile->displayName : tr("原服务"),
        turn->modelId(), revision.revisionNumber());
    answerWindow_->setPendingSnapshotPreview(
        revision.renderedSnapshot().image(), retryProfile.has_value()
            ? retryProfile->baseUrl.host() : answerWindow_->targetDomain());
    (void)revision.renderedSnapshot().releaseDecodedImage();
    answerWindow_->beginRequest(turn->requestId());
    dispatchTurn(*turn);
}

void AiSessionController::retryCurrentSnapshot(
    const QUuid& previousRequestId,
    const QString& question)
{
    Q_UNUSED(question)
    if (!editor_ || profiles_ == nullptr || answerWindow_ == nullptr) return;
    const snapask::ConversationTurn* previous =
        conversation_.turnForRequest(previousRequestId);
    if (previous == nullptr) return;

    const auto originalProfile = profiles_->profile(previous->providerProfileId());
    if (!originalProfile.has_value()) {
        QMessageBox::warning(
            answerWindow_, tr("无法重新发送"),
            tr("原请求使用的服务档案已不存在。"));
        return;
    }
    if (!ensureEndpointConsent(*originalProfile)) return;

    // This explicit action is the sole point at which the latest editor state
    // is frozen for a retry. The exact same RenderedSnapshot is then owned by
    // ConversationSession and consumed by the network boundary.
    const snapask::RenderedSnapshot& latest =
        editor_->currentRenderedSnapshot();
    if (!latest.isValid()) {
        QMessageBox::warning(
            answerWindow_, tr("无法重新发送"),
            tr("当前截图无法生成可发送快照。"));
        return;
    }

    snapask::SessionMemoryBudget prospective = memoryBudget_;
    QString memoryError;
    if (!prepareSnapshotMemory(latest, &prospective, &memoryError)) {
        QMessageBox::warning(
            answerWindow_, tr("无法重新发送"), memoryError);
        return;
    }

    const snapask::ConversationTurn* turn =
        conversation_.retryWithLatestCurrent(previousRequestId, latest);
    if (turn == nullptr) return;

    selectedProfileId_ = turn->providerProfileId();
    selectedModelId_ = turn->modelId();
    reloadProfileChoices();
    const auto& revision = *turn->snapshotRevision();
    syncConversationHistory(turn->requestId());
    answerWindow_->setAnswerMarkdown({});
    answerWindow_->setQuestion(turn->question());
    answerWindow_->setRequestContext(
        originalProfile->displayName,
        turn->modelId(),
        revision.revisionNumber());
    answerWindow_->setPendingSnapshotPreview(
        revision.renderedSnapshot().image(), originalProfile->baseUrl.host());
    answerWindow_->beginRequest(turn->requestId());
    commitSnapshotMemory(std::move(prospective));
    dispatchTurn(*turn);
}

void AiSessionController::handleNetworkEvent(
    const snapask::ai::AiStreamEvent& event)
{
    const snapask::ConversationTurn* known =
        conversation_.turnForRequest(event.requestId);
    if (known == nullptr || !conversation_.acceptEvent(event)) return;

    if (answerWindow_ != nullptr) {
        answerWindow_->consumeStreamEvent(event);
    }
    if (!editor_) return;

    const snapask::ConversationTurn* updated =
        conversation_.turnForRequest(event.requestId);
    if (event.type == snapask::ai::EventType::Started && updated != nullptr) {
        editor_->session().markSentHash(
            updated->snapshotRevision()->renderedHash());
        editor_->setGenerationActive(true);
        updateUnsentState();
    } else if (event.type == snapask::ai::EventType::Completed
               || event.type == snapask::ai::EventType::Failed
               || event.type == snapask::ai::EventType::Cancelled) {
        editor_->setGenerationActive(false);
        updatePendingPreview();
        if (profileReloadPending_) reloadProfileChoices();
    }
}

void AiSessionController::handleEditorSnapshotChanged()
{
    // CanvasWidget emits contentChanged only when a command/crop is committed,
    // not for its transient mouse-move presentation. Coalesce rapid undo,
    // redo, or keyboard adjustments as an extra guard against repeated PNG
    // encoding on the UI thread.
    if (pendingPreviewTimer_ != nullptr) pendingPreviewTimer_->start();
}

const snapask::ConversationTurn* AiSessionController::beginCurrentTurn(
    const QString& question)
{
    if (!editor_ || profiles_ == nullptr || answerWindow_ == nullptr) {
        return nullptr;
    }
    auto profile = selectedProfileId_.isNull()
        ? profiles_->defaultProfile()
        : profiles_->profile(selectedProfileId_);
    if (!profile.has_value()) {
        profile = profiles_->defaultProfile();
    }
    if (!profile.has_value()) {
        const QUuid requestId = QUuid::createUuid();
        answerWindow_->beginRequest(requestId);
        answerWindow_->consumeStreamEvent(localFailure(
            requestId, snapask::ai::ErrorKind::InvalidConfiguration,
            tr("请先在设置中配置 AI 服务、模型和 API Key。")));
        return nullptr;
    }
    if (!ensureEndpointConsent(*profile)) return nullptr;

    const snapask::RenderedSnapshot& snapshot =
        editor_->currentRenderedSnapshot();
    if (!snapshot.isValid()) {
        const QUuid requestId = QUuid::createUuid();
        answerWindow_->beginRequest(requestId);
        answerWindow_->consumeStreamEvent(localFailure(
            requestId, snapask::ai::ErrorKind::InvalidConfiguration,
            tr("当前截图无法生成可发送快照。")));
        return nullptr;
    }

    snapask::SessionMemoryBudget prospective = memoryBudget_;
    QString memoryError;
    if (!prepareSnapshotMemory(snapshot, &prospective, &memoryError)) {
        const QUuid requestId = QUuid::createUuid();
        answerWindow_->beginRequest(requestId);
        answerWindow_->consumeStreamEvent(localFailure(
            requestId, snapask::ai::ErrorKind::InvalidConfiguration,
            memoryError));
        return nullptr;
    }

    const QUuid requestId = QUuid::createUuid();
    const QString effectiveModel = selectedModelId_.trimmed().isEmpty()
        ? profile->modelId : selectedModelId_.trimmed();
    const snapask::ConversationTurn* turn = conversation_.beginExplicitSend(
        snapshot, question, profile->id, effectiveModel, requestId);
    if (turn == nullptr) return nullptr;

    const auto& revision = *turn->snapshotRevision();
    syncConversationHistory(requestId);
    answerWindow_->setAnswerMarkdown({});
    answerWindow_->setRequestContext(
        profile->displayName, effectiveModel, revision.revisionNumber());
    answerWindow_->setPendingSnapshotPreview(
        revision.renderedSnapshot().image(), profile->baseUrl.host());
    answerWindow_->beginRequest(requestId);
    commitSnapshotMemory(std::move(prospective));
    return turn;
}

void AiSessionController::dispatchTurn(const snapask::ConversationTurn& turn)
{
    if (profiles_ == nullptr || credentials_ == nullptr || !networkClient_) {
        failLocally(turn, snapask::ai::ErrorKind::InvalidConfiguration,
                    tr("AI 服务当前不可用。"));
        return;
    }
    const auto profile = profiles_->profile(turn.providerProfileId());
    if (!profile.has_value()) {
        failLocally(turn, snapask::ai::ErrorKind::InvalidConfiguration,
                    tr("原请求使用的服务档案已不存在。"));
        return;
    }
    if (endpointConsent_ == nullptr
        || !endpointConsent_->isApproved(profile->baseUrl)) {
        failLocally(turn, snapask::ai::ErrorKind::InvalidConfiguration,
                    tr("此服务端点尚未获得发送授权。"));
        return;
    }

    QString credentialError;
    auto apiKey = credentials_->read(profile->credentialRef, &credentialError);
    if (!apiKey.has_value()) {
        failLocally(turn, snapask::ai::ErrorKind::Authentication,
                    credentialError.isEmpty()
                        ? tr("未在 Windows 凭据管理器中找到 API Key。")
                        : credentialError);
        return;
    }

    snapask::ai::AiRequest request;
    request.requestId = turn.requestId();
    request.sessionId = conversation_.sessionId();
    request.snapshotId = turn.snapshotRevision()->snapshotId();
    request.providerProfileId = turn.providerProfileId();
    request.modelId = turn.modelId();
    request.question = turn.question();

    const QList<const snapask::ConversationTurn*> turns = conversation_.turns();
    const qsizetype contextStart = std::max<qsizetype>(
        0, turns.size() - kMaximumContextTurns - 1);
    for (qsizetype index = contextStart; index < turns.size(); ++index) {
        const snapask::ConversationTurn* contextTurn = turns.at(index);
        if (contextTurn == nullptr || contextTurn->requestId() == turn.requestId()
            || contextTurn->status() != snapask::ConversationTurn::Status::Completed) {
            continue;
        }
        request.recentContext.append({
            snapask::ai::ConversationMessage::Role::User,
            contextTurn->question()});
        request.recentContext.append({
            snapask::ai::ConversationMessage::Role::Assistant,
            contextTurn->answer()});
    }

    networkClient_->sendExplicit(
        std::move(request), turn.snapshotRevision()->renderedSnapshot(),
        *profile, std::move(*apiKey));
}

void AiSessionController::failLocally(
    const snapask::ConversationTurn& turn,
    const snapask::ai::ErrorKind kind,
    const QString& message)
{
    handleNetworkEvent(localFailure(turn.requestId(), kind, message));
}

void AiSessionController::updatePendingPreview()
{
    if (!editor_ || answerWindow_ == nullptr) return;
    if (pendingPreviewTimer_ != nullptr) pendingPreviewTimer_->stop();
    const snapask::RenderedSnapshot& preview =
        editor_->currentRenderedSnapshot();
    auto profile = profiles_ != nullptr && !selectedProfileId_.isNull()
        ? profiles_->profile(selectedProfileId_)
        : std::optional<snapask::ai::ProviderProfile>{};
    if (!profile.has_value() && profiles_ != nullptr) {
        profile = profiles_->defaultProfile();
    }
    if (profile.has_value()) {
        answerWindow_->setRequestContext(
            profile->displayName,
            selectedModelId_.isEmpty() ? profile->modelId : selectedModelId_,
            static_cast<quint64>(conversation_.revisionCount() + 1));
        answerWindow_->setPendingSnapshotPreview(
            preview.image(), profile->baseUrl.host());
    } else {
        answerWindow_->setRequestContext(
            tr("未配置"), {},
            static_cast<quint64>(conversation_.revisionCount() + 1));
        answerWindow_->setPendingSnapshotPreview(preview.image(), tr("未配置"));
    }
    const bool hasPriorSend = !editor_->session().lastSentHash().isEmpty();
    answerWindow_->setHasUnsentChanges(
        preview.isValid() && hasPriorSend
        && editor_->session().hasUnsentChanges(preview.sha256()));
    synchronizeRebuildableCacheBudget();
    if (hasPriorSend
        && !editor_->session().hasUnsentChanges(preview.sha256())) {
        (void)answerWindow_->releasePendingSnapshotPreviewImage();
        (void)memoryBudget_.removeRebuildableCache(
            QString::fromLatin1(kPendingPreviewCacheKey));
    }
}

void AiSessionController::updateUnsentState()
{
    if (!editor_ || answerWindow_ == nullptr) return;
    const snapask::RenderedSnapshot& current =
        editor_->currentRenderedSnapshot();
    const bool hasPriorSend = !editor_->session().lastSentHash().isEmpty();
    answerWindow_->setHasUnsentChanges(
        current.isValid() && hasPriorSend
        && editor_->session().hasUnsentChanges(current.sha256()));
}

void AiSessionController::synchronizeRebuildableCacheBudget()
{
    if (editor_ == nullptr || answerWindow_ == nullptr) {
        return;
    }

    const QString canvasKey = QString::fromLatin1(kCanvasCacheKey);
    const QString previewKey = QString::fromLatin1(kPendingPreviewCacheKey);
    const auto releaseKey = [this, &canvasKey, &previewKey](const QString& key) {
        if (key == canvasKey && editor_ != nullptr
            && editor_->canvasWidget() != nullptr) {
            editor_->canvasWidget()->releaseRebuildableCaches();
        } else if (key == previewKey && answerWindow_ != nullptr) {
            (void)answerWindow_->releasePendingSnapshotPreviewImage();
        }
    };
    const auto account = [this, &releaseKey](
                             const QString& key,
                             const quint64 byteSize) {
        if (byteSize == 0) {
            (void)memoryBudget_.removeRebuildableCache(key);
            return;
        }
        const snapask::MemoryBudgetResult result =
            memoryBudget_.upsertRebuildableCache(key, byteSize);
        for (const QString& evicted : result.evictedCacheKeys) {
            releaseKey(evicted);
        }
        if (!result.accepted()) {
            releaseKey(key);
            (void)memoryBudget_.removeRebuildableCache(key);
        }
    };

    const auto* canvas = editor_->canvasWidget();
    account(
        canvasKey,
        canvas != nullptr ? canvas->rebuildableCacheByteSize() : 0);
    account(previewKey, answerWindow_->pendingSnapshotPreviewByteSize());
}

void AiSessionController::syncConversationHistory(const QUuid& activeRequestId)
{
    if (answerWindow_ == nullptr) return;
    QList<snapask::ui::answer::AnswerTurnPresentation> history;
    const QList<const snapask::ConversationTurn*> turns = conversation_.turns();
    history.reserve(turns.size());
    for (qsizetype index = 0; index < turns.size(); ++index) {
        const snapask::ConversationTurn* turn = turns.at(index);
        if (turn == nullptr || turn->requestId() == activeRequestId) continue;

        snapask::ui::answer::AnswerCardState state =
            snapask::ui::answer::AnswerCardState::Idle;
        switch (turn->status()) {
        case snapask::ConversationTurn::Status::Draft:
        case snapask::ConversationTurn::Status::Queued:
            state = snapask::ui::answer::AnswerCardState::Sending;
            break;
        case snapask::ConversationTurn::Status::Streaming:
            state = snapask::ui::answer::AnswerCardState::Streaming;
            break;
        case snapask::ConversationTurn::Status::Completed:
            state = snapask::ui::answer::AnswerCardState::Completed;
            break;
        case snapask::ConversationTurn::Status::Failed:
            state = snapask::ui::answer::AnswerCardState::Failed;
            break;
        case snapask::ConversationTurn::Status::Cancelled:
            state = snapask::ui::answer::AnswerCardState::Cancelled;
            break;
        }

        QString serviceName = tr("已删除的服务");
        if (profiles_ != nullptr) {
            const auto profile = profiles_->profile(turn->providerProfileId());
            if (profile.has_value()) serviceName = profile->displayName;
        }
        const auto& revision = turn->snapshotRevision();
        history.append({
            turn->requestId(),
            static_cast<quint64>(index + 1),
            turn->question(),
            turn->answer(),
            revision != nullptr ? revision->revisionNumber() : 0,
            serviceName,
            turn->modelId(),
            state,
            turn->error(),
        });
    }
    answerWindow_->setConversationHistory(std::move(history));
}

bool AiSessionController::prepareSnapshotMemory(
    const snapask::RenderedSnapshot& snapshot,
    snapask::SessionMemoryBudget* prospective,
    QString* error) const
{
    if (!sourceMemoryRegistered_ || prospective == nullptr
        || !snapshot.isValid()) {
        if (error != nullptr) {
            *error = tr("当前截图超过会话内存预算，无法安全保留发送版本。"
                        "请缩小选区或关闭此会话后重试。");
        }
        return false;
    }
    const auto pngSize = static_cast<quint64>(snapshot.pngBytes().size());
    const snapask::MemoryBudgetResult result =
        prospective->upsertSentSnapshotPng(snapshot.sha256(), pngSize);
    if (result.accepted()) return true;
    if (error != nullptr) {
        *error = tr("已发送快照将超过 %1 MiB 的会话内存上限。"
                    "请缩小选区或关闭此会话后重试。")
            .arg(result.hardLimitBytes / (1024ULL * 1024ULL));
    }
    return false;
}

void AiSessionController::commitSnapshotMemory(
    snapask::SessionMemoryBudget prospective)
{
    const QString canvasKey = QString::fromLatin1(kCanvasCacheKey);
    const QString previewKey = QString::fromLatin1(kPendingPreviewCacheKey);
    const bool evictedCanvas =
        memoryBudget_.containsRebuildableCache(canvasKey)
        && !prospective.containsRebuildableCache(canvasKey);
    const bool evictedPreview =
        memoryBudget_.containsRebuildableCache(previewKey)
        && !prospective.containsRebuildableCache(previewKey);
    memoryBudget_ = std::move(prospective);
    if (evictedCanvas && editor_ != nullptr
        && editor_->canvasWidget() != nullptr) {
        editor_->canvasWidget()->releaseRebuildableCaches();
    }
    if (evictedPreview && answerWindow_ != nullptr) {
        (void)answerWindow_->releasePendingSnapshotPreviewImage();
    }
    (void)conversation_.compactOldSnapshotImages();
    const snapask::SnapshotRevision* latest = conversation_.latestRevision();
    if (latest != nullptr) {
        (void)latest->renderedSnapshot().releaseDecodedImage();
    }
    if (editor_ != nullptr) {
        (void)editor_->currentRenderedSnapshot().releaseDecodedImage();
    }
    if (answerWindow_ != nullptr) {
        (void)answerWindow_->releasePendingSnapshotPreviewImage();
        (void)memoryBudget_.removeRebuildableCache(previewKey);
    }
}

void AiSessionController::positionAnswerCard()
{
    if (!editor_ || answerWindow_ == nullptr) return;
    if (windowLink_ != nullptr && windowLink_->isBound()) {
        windowLink_->reflow();
    }
}

bool AiSessionController::ensureEndpointConsent(
    const snapask::ai::ProviderProfile& profile)
{
    if (endpointConsent_ == nullptr
        || endpointConsent_->isApproved(profile.baseUrl)) {
        return endpointConsent_ != nullptr;
    }

    const QString origin =
        snapask::infrastructure::EndpointConsentStore::normalizedOrigin(
            profile.baseUrl);
    if (origin.isEmpty() || answerWindow_ == nullptr) return false;
    const auto choice = QMessageBox::question(
        answerWindow_, tr("首次使用自定义 AI 服务"),
        tr("截图和问题将发送到：\n%1\n\n数据处理规则由该服务商负责。"
           "确认信任此目标后才会建立网络请求。")
            .arg(origin),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (choice != QMessageBox::Yes) return false;

    QString error;
    if (!endpointConsent_->approve(profile.baseUrl, &error)) {
        QMessageBox::warning(
            answerWindow_, tr("无法保存授权"),
            error.isEmpty() ? tr("自定义服务授权状态无法保存。") : error);
        return false;
    }
    return true;
}

}  // namespace snapask::app
