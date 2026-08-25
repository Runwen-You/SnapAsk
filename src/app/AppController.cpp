#include "app/AppController.h"

#include "ai/AiNetworkClient.h"
#include "ai/AiProfileRepository.h"
#include "ai/ProviderProbeClient.h"
#include "app/AiSessionController.h"
#include "infrastructure/EndpointConsentStore.h"
#include "platform/windows/CredentialStore.h"
#include "platform/windows/SystemLifecycleMonitor.h"
#include "platform/windows/WindowBackdrop.h"
#include "ui/answer/AnswerCardWindow.h"
#include "ui/capture/CaptureOverlay.h"
#include "ui/canvas/CanvasWidget.h"
#include "ui/editor/EditorWindow.h"
#include "ui/onboarding/PrivacyNoticeDialog.h"
#include "ui/settings/ProviderSettingsWidget.h"
#include "ui/settings/SettingsDialog.h"

#include <QApplication>
#include <QAction>
#include <QEvent>
#include <QClipboard>
#include <QEventLoop>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QKeyCombination>
#include <QKeySequence>
#include <QMessageBox>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <QtConcurrentRun>

#include <Windows.h>
#include <dwmapi.h>

#include <optional>
#include <utility>

namespace snapask::app {
namespace {

constexpr auto kThemeSettingsKey = "appearance/theme";
constexpr auto kCaptureHotkeySettingsKey = "capture/hotkey";
constexpr auto kDefaultCaptureHotkey = "Ctrl+Shift+Space";
constexpr auto kProviderFileName = "providers.json";
constexpr auto kPrivacyNoticeVersionKey = "privacy/noticeVersion";
constexpr int kPrivacyNoticeVersion = 1;
constexpr auto kDefaultResponsesBaseUrl = "https://api.openai.com/v1";
constexpr auto kDefaultResponsesModel = "gpt-4.1-mini";

struct CaptureWorkerResult final {
    QUuid jobId;
    std::optional<snapask::capture::CaptureFrame> frame;
    QString error;
};

void scrubSensitiveString(QString& value)
{
    value.detach();
    if (!value.isEmpty()) value.fill(QChar{});
    value.clear();
}

snapask::ai::Protocol toAiProtocol(
    const snapask::ui::ProviderUiProtocol protocol)
{
    return protocol == snapask::ui::ProviderUiProtocol::OpenAIResponses
        ? snapask::ai::Protocol::OpenAIResponses
        : snapask::ai::Protocol::ChatCompletions;
}

snapask::ui::ProviderUiProtocol toUiProtocol(
    const snapask::ai::Protocol protocol)
{
    return protocol == snapask::ai::Protocol::OpenAIResponses
        ? snapask::ui::ProviderUiProtocol::OpenAIResponses
        : snapask::ui::ProviderUiProtocol::ChatCompletions;
}

snapask::ai::Capabilities toAiCapabilities(
    const snapask::ui::ProviderUiCapabilities capabilities)
{
    snapask::ai::Capabilities result;
    if (capabilities.testFlag(snapask::ui::ProviderImageInput)) {
        result |= snapask::ai::ImageInput;
    }
    if (capabilities.testFlag(snapask::ui::ProviderStreaming)) {
        result |= snapask::ai::Streaming;
    }
    if (capabilities.testFlag(snapask::ui::ProviderModelList)) {
        result |= snapask::ai::ModelList;
    }
    return result;
}

snapask::ui::ProviderUiCapabilities toUiCapabilities(
    const snapask::ai::Capabilities capabilities)
{
    snapask::ui::ProviderUiCapabilities result;
    if (capabilities.testFlag(snapask::ai::ImageInput)) {
        result |= snapask::ui::ProviderImageInput;
    }
    if (capabilities.testFlag(snapask::ai::Streaming)) {
        result |= snapask::ui::ProviderStreaming;
    }
    if (capabilities.testFlag(snapask::ai::ModelList)) {
        result |= snapask::ui::ProviderModelList;
    }
    return result;
}

snapask::ai::ProviderProfile providerFromDraft(
    const snapask::ui::ProviderProfileDraft& draft)
{
    snapask::ai::ProviderProfile profile;
    profile.id = draft.profileId;
    profile.displayName = draft.displayName.trimmed();
    profile.protocol = toAiProtocol(draft.protocol);
    profile.baseUrl = draft.baseUrl;
    profile.credentialRef = QStringLiteral("SnapAsk/provider/")
        + profile.id.toString(QUuid::WithoutBraces);
    profile.modelId = draft.modelId.trimmed();
    profile.availableModels = draft.availableModels;
    profile.connectTimeoutMs = draft.connectTimeoutMs;
    profile.requestTimeoutMs = draft.requestTimeoutMs;
    profile.capabilities = toAiCapabilities(draft.capabilities);
    profile.proxyUrl = draft.proxyUrl;
    profile.customHeaders = draft.customHeaders;
    return profile;
}

bool sameProbeConfiguration(
    const snapask::ai::ProviderProfile& left,
    const snapask::ai::ProviderProfile& right)
{
    return left.id == right.id && left.protocol == right.protocol
        && left.baseUrl == right.baseUrl
        && left.modelId.trimmed() == right.modelId.trimmed()
        && left.proxyUrl == right.proxyUrl
        && left.customHeaders == right.customHeaders;
}

snapask::ui::ProviderUiOperation toUiOperation(
    const snapask::ai::ProviderProbeOperation operation)
{
    switch (operation) {
    case snapask::ai::ProviderProbeOperation::ModelList:
        return snapask::ui::ProviderUiOperation::FetchModels;
    case snapask::ai::ProviderProbeOperation::TextConnection:
        return snapask::ui::ProviderUiOperation::TestTextConnection;
    case snapask::ai::ProviderProbeOperation::ImageUnderstanding:
        return snapask::ui::ProviderUiOperation::TestImageUnderstanding;
    }
    return snapask::ui::ProviderUiOperation::TestTextConnection;
}

std::optional<snapask::platform::windows::HotkeyChord> toNativeHotkey(
    const QKeySequence& sequence)
{
    if (sequence.count() != 1 || sequence.isEmpty()) return std::nullopt;
    const auto combination = sequence[0];
    const auto qtModifiers = combination.keyboardModifiers();
    quint32 modifiers = 0;
    if (qtModifiers.testFlag(Qt::AltModifier)) modifiers |= snapask::platform::windows::HotkeyAlt;
    if (qtModifiers.testFlag(Qt::ControlModifier)) modifiers |= snapask::platform::windows::HotkeyControl;
    if (qtModifiers.testFlag(Qt::ShiftModifier)) modifiers |= snapask::platform::windows::HotkeyShift;
    if (qtModifiers.testFlag(Qt::MetaModifier)) modifiers |= snapask::platform::windows::HotkeyWindows;
    if (modifiers == 0) return std::nullopt;

    const int key = static_cast<int>(combination.key());
    quint32 virtualKey = 0;
    if ((key >= Qt::Key_A && key <= Qt::Key_Z)
        || (key >= Qt::Key_0 && key <= Qt::Key_9)) {
        virtualKey = static_cast<quint32>(key);
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        virtualKey = VK_F1 + static_cast<quint32>(key - Qt::Key_F1);
    } else {
        switch (key) {
        case Qt::Key_Space: virtualKey = VK_SPACE; break;
        case Qt::Key_Print: virtualKey = VK_SNAPSHOT; break;
        case Qt::Key_Insert: virtualKey = VK_INSERT; break;
        case Qt::Key_Delete: virtualKey = VK_DELETE; break;
        case Qt::Key_Home: virtualKey = VK_HOME; break;
        case Qt::Key_End: virtualKey = VK_END; break;
        case Qt::Key_PageUp: virtualKey = VK_PRIOR; break;
        case Qt::Key_PageDown: virtualKey = VK_NEXT; break;
        case Qt::Key_Left: virtualKey = VK_LEFT; break;
        case Qt::Key_Right: virtualKey = VK_RIGHT; break;
        case Qt::Key_Up: virtualKey = VK_UP; break;
        case Qt::Key_Down: virtualKey = VK_DOWN; break;
        default: return std::nullopt;
        }
    }
    return snapask::platform::windows::HotkeyChord{virtualKey, modifiers};
}

}  // namespace

AppController::AppController(QObject* parent)
    : QObject(parent)
    , trayController_(this)
    , globalHotkey_(this)
    , aiNetworkClient_(std::make_unique<snapask::ai::AiNetworkClient>(this))
    , providerProbeClient_(
          std::make_unique<snapask::ai::ProviderProbeClient>(this))
    , credentialStore_(
          std::make_unique<snapask::platform::windows::CredentialStore>())
    , lifecycleMonitor_(
          std::make_unique<
              snapask::platform::windows::SystemLifecycleMonitor>(150, this))
    , endpointConsentStore_(
          std::make_unique<snapask::infrastructure::EndpointConsentStore>())
{
    connect(
        providerProbeClient_.get(),
        &snapask::ai::ProviderProbeClient::resultReady,
        this,
        &AppController::handleProviderProbeResult,
        Qt::QueuedConnection);
}

AppController::~AppController() = default;

bool AppController::start() {
    qApp->installEventFilter(this);
    QSettings settings;
    themeMode_ = ui::ThemeTokens::fromStorage(
        settings.value(QString::fromLatin1(kThemeSettingsKey), QStringLiteral("system")).toString());
    applyTheme(themeMode_);
    if (!ensurePrivacyNoticeAccepted()) return false;

    connect(&trayController_, &TrayController::settingsRequested, this, &AppController::openSettings);
    connect(&trayController_, &TrayController::captureRequested, this, &AppController::startCapture);
    connect(&trayController_, &TrayController::clipboardImageRequested,
            this, &AppController::openClipboardImage);
    connect(&trayController_, &TrayController::exitRequested, qApp, &QApplication::quit);
    connect(&globalHotkey_, &snapask::platform::windows::GlobalHotkey::activated,
            this, &AppController::startCapture);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
        if (themeMode_ == ui::ThemeMode::System) {
            applyTheme(themeMode_);
        }
    });
#endif

    if (!trayController_.start()) {
        qCritical("The Windows notification area is unavailable.");
        QMessageBox::critical(
            nullptr,
            tr("SnapAsk 无法启动"),
            tr("Windows 通知区域当前不可用。请在 Explorer 恢复后重新启动 SnapAsk。"));
        return false;
    }

    initializeAiConfiguration();

    captureOverlay_ = std::make_unique<snapask::ui::capture::CaptureOverlay>();
    connect(captureOverlay_.get(), &snapask::ui::capture::CaptureOverlay::captureConfirmed,
            this, [this](const snapask::capture::CaptureSelection& crop) {
        captureInProgress_ = false;
        restoreHiddenWindows();
        const auto action = captureOverlay_->takeHandoffAction();
        openCapturedEditor(crop.image, crop.desktopRectPx, action);
    });
    connect(captureOverlay_.get(), &snapask::ui::capture::CaptureOverlay::captureCancelled,
            this, [this] {
        captureInProgress_ = false;
        restoreHiddenWindows();
    });
    connect(captureOverlay_.get(), &snapask::ui::capture::CaptureOverlay::captureFailed,
            this, [this](const QString& error) {
        // The overlay deliberately remains active so the user can adjust or
        // retry the selection after an in-memory crop failure.
        trayController_.showMessage(tr("截图失败"), error);
    });

    connect(
        lifecycleMonitor_.get(),
        &snapask::platform::windows::SystemLifecycleMonitor::displayTopologyChanged,
        this, &AppController::handleSystemEnvironmentChange);
    connect(
        lifecycleMonitor_.get(),
        &snapask::platform::windows::SystemLifecycleMonitor::dpiChanged,
        this, &AppController::handleSystemEnvironmentChange);
    connect(
        lifecycleMonitor_.get(),
        &snapask::platform::windows::SystemLifecycleMonitor::systemResumed,
        this, &AppController::handleSystemEnvironmentChange);
    connect(
        lifecycleMonitor_.get(),
        &snapask::platform::windows::SystemLifecycleMonitor::remoteSessionChanged,
        this, [this](bool, quint32) { handleSystemEnvironmentChange(); });
    connect(
        lifecycleMonitor_.get(),
        &snapask::platform::windows::SystemLifecycleMonitor::systemSuspending,
        this, &AppController::cancelActiveCaptureForEnvironmentChange);
    connect(
        lifecycleMonitor_.get(),
        &snapask::platform::windows::SystemLifecycleMonitor::sessionLocked,
        this, [this](quint32) { cancelActiveCaptureForEnvironmentChange(); });
    QString lifecycleError;
    if (!lifecycleMonitor_->start(&lifecycleError)) {
        trayController_.showMessage(
            tr("系统变化监视不可用"),
            lifecycleError.isEmpty()
                ? tr("显示器或会话变化时，请重新开始当前截图。")
                : lifecycleError);
    }

    QSettings hotkeySettings;
    const auto storedHotkey = hotkeySettings.value(
        QString::fromLatin1(kCaptureHotkeySettingsKey),
        QString::fromLatin1(kDefaultCaptureHotkey)).toString();
    registerCaptureHotkey(QKeySequence::fromString(storedHotkey, QKeySequence::PortableText));

    qInfo("SnapAsk application shell started.");
    return true;
}

void AppController::openClipboardImage() {
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        trayController_.showMessage(
            tr("无可用图片"),
            tr("剪贴板中没有可读取的图片。"));
        return;
    }
    openEditor(image);
}

void AppController::openEditor(QImage image) {
    openCapturedEditor(
        std::move(image),
        {},
        snapask::ui::capture::CaptureHandoffAction::Edit);
}

void AppController::openCapturedEditor(
    QImage image,
    const QRect& desktopRectPx,
    const snapask::ui::capture::CaptureHandoffAction action)
{
    auto* editor = new snapask::ui::editor::EditorWindow(std::move(image));
    editor->setCaptureDesktopRectPx(desktopRectPx);
    auto* aiSession = new AiSessionController(
        editor, aiNetworkClient_.get(), aiProfiles_.get(),
        credentialStore_.get(), endpointConsentStore_.get());
    connect(this, &AppController::aiProfilesChanged,
            aiSession, &AiSessionController::reloadProfileChoices);
    editor->show();
    editor->raise();
    editor->activateWindow();
    const bool dark = ui::ThemeTokens::resolve(themeMode_).dark;
    (void)snapask::platform::windows::WindowBackdrop::apply(
        editor, {snapask::platform::windows::BackdropPreference::Mica,
                 dark, true, true});
    (void)snapask::platform::windows::WindowBackdrop::apply(
        aiSession->answerWindow(),
        {snapask::platform::windows::BackdropPreference::Transient,
         dark, true, true});

    using snapask::ui::canvas::CanvasTool;
    using snapask::ui::capture::CaptureHandoffAction;
    switch (action) {
    case CaptureHandoffAction::Edit:
        editor->activateTool(CanvasTool::Select);
        break;
    case CaptureHandoffAction::Rectangle:
        editor->activateTool(CanvasTool::Rectangle);
        break;
    case CaptureHandoffAction::Arrow:
        editor->activateTool(CanvasTool::Arrow);
        break;
    case CaptureHandoffAction::Text:
        editor->activateTool(CanvasTool::Text);
        break;
    case CaptureHandoffAction::Mosaic:
        editor->activateTool(CanvasTool::Mosaic);
        break;
    case CaptureHandoffAction::Copy:
    case CaptureHandoffAction::Save:
    case CaptureHandoffAction::Pin:
    case CaptureHandoffAction::Ask: {
        const char* objectName = nullptr;
        if (action == CaptureHandoffAction::Copy) {
            objectName = "copySnapshotAction";
        } else if (action == CaptureHandoffAction::Save) {
            objectName = "saveSnapshotAction";
        } else if (action == CaptureHandoffAction::Pin) {
            objectName = "pinSnapshotAction";
        } else {
            objectName = "askSnapshotAction";
        }
        QPointer<snapask::ui::editor::EditorWindow> editorGuard(editor);
        QTimer::singleShot(0, editor, [editorGuard, objectName] {
            if (editorGuard == nullptr) {
                return;
            }
            if (auto* requested = editorGuard->findChild<QAction*>(
                    QString::fromLatin1(objectName));
                requested != nullptr) {
                requested->trigger();
            }
        });
        break;
    }
    }
}

void AppController::activate() {
    openSettings();
}

void AppController::openSettings() {
    if (!settingsDialog_) {
        settingsDialog_ = std::make_unique<ui::SettingsDialog>();
        connect(
            settingsDialog_.get(),
            &ui::SettingsDialog::themeModeChanged,
            this,
            &AppController::applyTheme);
        connect(settingsDialog_.get(), &ui::SettingsDialog::captureHotkeyChanged,
                this, &AppController::registerCaptureHotkey);
        connect(settingsDialog_.get(), &ui::SettingsDialog::captureNowRequested,
                this, &AppController::startCapture);
        auto* providers = settingsDialog_->providerSettingsWidget();
        connect(providers, &ui::ProviderSettingsWidget::addRequested,
                this, [this](const QUuid&, const ui::ProviderProfileDraft& draft,
                             const QString& key) {
            saveProviderProfile(draft, key, true, false);
        });
        connect(providers, &ui::ProviderSettingsWidget::editRequested,
                this, [this](const QUuid&, const ui::ProviderProfileDraft& draft,
                             const QString& key, const bool replaceKey) {
            saveProviderProfile(draft, key, replaceKey, true);
        });
        connect(providers, &ui::ProviderSettingsWidget::duplicateRequested,
                this, &AppController::duplicateProviderProfile);
        connect(providers, &ui::ProviderSettingsWidget::deleteRequested,
                this, &AppController::deleteProviderProfile);
        connect(providers, &ui::ProviderSettingsWidget::setDefaultRequested,
                this, &AppController::setDefaultProviderProfile);
        connect(providers, &ui::ProviderSettingsWidget::exportRequested,
                this, [this](const QUuid&) { exportProviderProfiles(); });
        connect(providers, &ui::ProviderSettingsWidget::fetchModelsRequested,
                this, [this](const QUuid& operationId, const QUuid&,
                             const ui::ProviderProfileDraft& draft,
                             const QString& key, const bool useStored) {
            startProviderProbe(
                operationId, draft, key, useStored,
                snapask::ai::ProviderProbeOperation::ModelList);
        });
        connect(providers, &ui::ProviderSettingsWidget::testTextRequested,
                this, [this](const QUuid& operationId, const QUuid&,
                             const ui::ProviderProfileDraft& draft,
                             const QString& key, const bool useStored) {
            startProviderProbe(
                operationId, draft, key, useStored,
                snapask::ai::ProviderProbeOperation::TextConnection);
        });
        connect(providers, &ui::ProviderSettingsWidget::testImageRequested,
                this, [this](const QUuid& operationId, const QUuid&,
                             const ui::ProviderProfileDraft& draft,
                             const QString& key, const bool useStored) {
            startProviderProbe(
                operationId, draft, key, useStored,
                snapask::ai::ProviderProbeOperation::ImageUnderstanding);
        });
    }

    settingsDialog_->setHotkeyStatus(globalHotkey_.isRegistered(), hotkeyError_);
    refreshProviderSettingsUi();

    settingsDialog_->show();
    (void)snapask::platform::windows::WindowBackdrop::apply(
        settingsDialog_.get(),
        {snapask::platform::windows::BackdropPreference::Mica,
         ui::ThemeTokens::resolve(themeMode_).dark, true, true});
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}

void AppController::applyTheme(ui::ThemeMode mode) {
    themeMode_ = mode;
    ui::ThemeTokens::apply(*qApp, themeMode_);
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        applyGlassBackdrop(widget);
    }
}

bool AppController::eventFilter(QObject* watched, QEvent* event)
{
    if (event != nullptr && event->type() == QEvent::Show) {
        auto* window = qobject_cast<QWidget*>(watched);
        if (window != nullptr && window->isWindow()
            && window != captureOverlay_.get()) {
            QPointer<QWidget> guard(window);
            QTimer::singleShot(0, this, [this, guard] {
                if (guard != nullptr && guard->isVisible()) {
                    applyGlassBackdrop(guard);
                }
            });
        }
    }
    return QObject::eventFilter(watched, event);
}

void AppController::applyGlassBackdrop(QWidget* window)
{
    if (window == nullptr || window == captureOverlay_.get()
        || !window->isWindow()) {
        return;
    }
    const Qt::WindowType type = window->windowType();
    if (type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen) {
        return;
    }
    const bool transient = type == Qt::Tool
        || window->objectName() == QStringLiteral("answerCardWindow");
    (void)snapask::platform::windows::WindowBackdrop::apply(
        window,
        {transient
             ? snapask::platform::windows::BackdropPreference::Transient
             : snapask::platform::windows::BackdropPreference::Mica,
         ui::ThemeTokens::resolve(themeMode_).dark,
         true,
         true});
}

void AppController::startCapture() {
    if (captureInProgress_ || (captureOverlay_ && captureOverlay_->isCaptureActive())) {
        return;
    }
    captureInProgress_ = true;
    hiddenForCapture_.clear();
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget != nullptr && widget->isVisible()
            && widget != captureOverlay_.get()) {
            hiddenForCapture_.append(widget);
            widget->hide();
        }
    }
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    DwmFlush();

    const QUuid jobId = QUuid::createUuid();
    activeCaptureJobId_ = jobId;
    auto* watcher = new QFutureWatcher<CaptureWorkerResult>(this);
    connect(
        watcher,
        &QFutureWatcher<CaptureWorkerResult>::finished,
        this,
        [this, watcher]() {
            CaptureWorkerResult result = watcher->result();
            watcher->deleteLater();
            if (!captureInProgress_ || result.jobId != activeCaptureJobId_) {
                return;
            }
            activeCaptureJobId_ = {};
            if (!result.frame.has_value()
                || !captureOverlay_->beginCapture(
                    std::move(*result.frame), &result.error)) {
                captureInProgress_ = false;
                restoreHiddenWindows();
                trayController_.showMessage(
                    tr("截图失败"),
                    result.error.isEmpty()
                        ? tr("当前屏幕无法捕获。受保护内容和安全桌面不受支持。")
                        : result.error);
            }
        });
    watcher->setFuture(QtConcurrent::run([jobId]() {
        CaptureWorkerResult result;
        result.jobId = jobId;
        const snapask::platform::windows::GdiScreenCapture backend;
        result.frame = backend.captureVirtualDesktop(&result.error);
        return result;
    }));
}

void AppController::registerCaptureHotkey(const QKeySequence& sequence) {
    const auto chord = toNativeHotkey(sequence);
    if (!chord.has_value()) {
        hotkeyError_ = tr("请选择一个带 Ctrl、Alt、Shift 或 Win 的单段快捷键。");
        globalHotkey_.unregisterHotkey();
    } else {
        QString platformError;
        if (globalHotkey_.registerHotkey(*chord, &platformError)) {
            hotkeyError_.clear();
        } else {
            hotkeyError_ = tr("快捷键注册失败，可能与其他应用冲突。%1")
                .arg(platformError.isEmpty() ? QString{} : QStringLiteral("\n") + platformError);
        }
    }
    if (settingsDialog_) {
        settingsDialog_->setHotkeyStatus(globalHotkey_.isRegistered(), hotkeyError_);
    }
}

void AppController::restoreHiddenWindows() {
    for (const auto& widget : std::as_const(hiddenForCapture_)) {
        if (widget) widget->show();
    }
    hiddenForCapture_.clear();
}

bool AppController::ensurePrivacyNoticeAccepted()
{
    QSettings settings;
    if (settings.value(QString::fromLatin1(kPrivacyNoticeVersionKey), 0).toInt()
        >= kPrivacyNoticeVersion) {
        return true;
    }

    snapask::ui::onboarding::PrivacyNoticeDialog notice;
    (void)snapask::platform::windows::WindowBackdrop::apply(
        &notice,
        {snapask::platform::windows::BackdropPreference::Transient,
         ui::ThemeTokens::resolve(themeMode_).dark, true, true});
    if (notice.exec() != QDialog::Accepted) return false;

    settings.setValue(
        QString::fromLatin1(kPrivacyNoticeVersionKey), kPrivacyNoticeVersion);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::warning(
            nullptr, tr("隐私选择未能保存"),
            tr("本次仍可使用 SnapAsk；下次启动时会再次显示隐私说明。"));
    }
    return true;
}

void AppController::cancelActiveCaptureForEnvironmentChange()
{
    if (captureOverlay_ != nullptr && captureOverlay_->isCaptureActive()) {
        captureOverlay_->cancelCapture();
        return;
    }
    if (captureInProgress_) {
        activeCaptureJobId_ = {};
        captureInProgress_ = false;
        restoreHiddenWindows();
    }
}

void AppController::handleSystemEnvironmentChange()
{
    cancelActiveCaptureForEnvironmentChange();
    // Re-evaluate DWM/high-contrast/RDP fallback policy and force Qt to
    // re-polish top-level windows after a topology or DPI transition.
    applyTheme(themeMode_);
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* editor = qobject_cast<snapask::ui::editor::EditorWindow*>(widget);
        if (editor == nullptr) continue;
        if (auto* session = editor->findChild<AiSessionController*>();
            session != nullptr) {
            session->reflowLinkedWindows();
        }
    }
}

void AppController::initializeAiConfiguration() {
    const QString configurationDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    aiProfiles_ = std::make_unique<snapask::ai::AiProfileRepository>(
        configurationDirectory + QLatin1Char('/')
        + QString::fromLatin1(kProviderFileName));

    QString error;
    if (!aiProfiles_->load(&error)) {
        trayController_.showMessage(
            tr("AI 配置未载入"),
            error.isEmpty() ? tr("服务配置无法读取，请在设置中重新保存。") : error);
        return;
    }
    if (!aiProfiles_->profiles().isEmpty()) return;

    snapask::ai::ProviderProfile profile;
    profile.id = QUuid::createUuid();
    profile.displayName = tr("OpenAI");
    profile.protocol = snapask::ai::Protocol::OpenAIResponses;
    profile.baseUrl = QUrl(QString::fromLatin1(kDefaultResponsesBaseUrl));
    profile.credentialRef = QStringLiteral("SnapAsk/provider/")
        + profile.id.toString(QUuid::WithoutBraces);
    profile.modelId = QString::fromLatin1(kDefaultResponsesModel);
    profile.capabilities = snapask::ai::Capabilities(
        snapask::ai::ImageInput | snapask::ai::Streaming
        | snapask::ai::ModelList);

    if (!aiProfiles_->upsert(profile, &error)
        || !aiProfiles_->setDefault(profile.id, &error)
        || !aiProfiles_->save(&error)) {
        trayController_.showMessage(
            tr("AI 配置未保存"),
            error.isEmpty() ? tr("默认服务配置无法保存。") : error);
    }
}

void AppController::refreshProviderSettingsUi()
{
    if (!settingsDialog_ || aiProfiles_ == nullptr
        || credentialStore_ == nullptr) {
        return;
    }

    QList<ui::ProviderProfileSummary> summaries;
    const QList<snapask::ai::ProviderProfile> profiles = aiProfiles_->profiles();
    summaries.reserve(profiles.size());
    for (const auto& profile : profiles) {
        QString credentialError;
        const bool hasCredential = credentialStore_->contains(
            profile.credentialRef, &credentialError);
        ui::ProviderProfileSummary summary;
        summary.profileId = profile.id;
        summary.displayName = profile.displayName;
        summary.protocol = toUiProtocol(profile.protocol);
        summary.baseUrl = profile.baseUrl;
        summary.modelId = profile.modelId;
        summary.availableModels = profile.availableModels;
        summary.capabilities = toUiCapabilities(profile.capabilities);
        summary.connectTimeoutMs = profile.connectTimeoutMs;
        summary.requestTimeoutMs = profile.requestTimeoutMs;
        summary.proxyUrl = profile.proxyUrl;
        summary.customHeaders = profile.customHeaders;
        summary.lastTestedAt = profile.lastTestedAt;
        summary.lastTestStatus = profile.lastTestStatus;
        summary.hasStoredApiKey = hasCredential;
        if (!credentialError.isEmpty() && summary.lastTestStatus.isEmpty()) {
            summary.lastTestStatus = credentialError;
        }
        summaries.append(std::move(summary));
    }
    settingsDialog_->providerSettingsWidget()->setProfiles(
        std::move(summaries), aiProfiles_->defaultProfileId());
}

void AppController::saveProviderProfile(
    const ui::ProviderProfileDraft& draft,
    const QString& apiKey,
    const bool replaceKey,
    const bool editing)
{
    if (!settingsDialog_ || aiProfiles_ == nullptr
        || credentialStore_ == nullptr) {
        return;
    }

    snapask::ai::ProviderProfile candidate = providerFromDraft(draft);
    const auto previous = aiProfiles_->profile(candidate.id);
    if ((editing && !previous.has_value())
        || (!editing && previous.has_value())) {
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法保存服务"),
            editing ? tr("要编辑的服务档案已不存在。")
                    : tr("服务档案 ID 已存在，请重新添加。"));
        return;
    }

    if (previous.has_value()) {
        candidate.lastTestedAt = previous->lastTestedAt;
        candidate.lastTestStatus = previous->lastTestStatus;
    }
    const auto completed = completedProviderProbes_.constFind(candidate.id);
    if (completed != completedProviderProbes_.cend()
        && sameProbeConfiguration(completed->candidate, candidate)) {
        candidate.lastTestedAt = QDateTime::currentDateTimeUtc();
        candidate.lastTestStatus = completed->result.message;
        if (completed->result.operation
                == snapask::ai::ProviderProbeOperation::ModelList
            && completed->result.success) {
            candidate.availableModels = completed->result.modelIds;
            if (!candidate.modelId.isEmpty()
                && !candidate.availableModels.contains(candidate.modelId)) {
                candidate.availableModels.prepend(candidate.modelId);
            }
        }
    }

    QString error;
    snapask::ai::AiProfileRepository validator(QString{});
    if (!validator.upsert(candidate, &error)) {
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法保存服务"), error);
        return;
    }

    if (!replaceKey
        && !credentialStore_->contains(candidate.credentialRef, &error)) {
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法保存服务"),
            error.isEmpty() ? tr("请先保存 API Key。") : error);
        return;
    }
    if (replaceKey && apiKey.trimmed().isEmpty()) {
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法保存服务"),
            tr("API Key 不能为空。"));
        return;
    }

    const QUuid previousDefault = aiProfiles_->defaultProfileId();
    if (!aiProfiles_->upsert(candidate, &error) || !aiProfiles_->save(&error)) {
        if (previous.has_value()) {
            (void)aiProfiles_->upsert(*previous);
        } else {
            (void)aiProfiles_->remove(candidate.id);
        }
        if (!previousDefault.isNull()) {
            (void)aiProfiles_->setDefault(previousDefault);
        }
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法保存服务"),
            error.isEmpty() ? tr("服务配置无法原子保存。") : error);
        return;
    }

    if (replaceKey) {
        QString keyCopy = apiKey;
        keyCopy.detach();
        const bool credentialSaved = credentialStore_->write(
            candidate.credentialRef, keyCopy, &error);
        scrubSensitiveString(keyCopy);
        if (!credentialSaved) {
            if (previous.has_value()) {
                (void)aiProfiles_->upsert(*previous);
            } else {
                (void)aiProfiles_->remove(candidate.id);
            }
            if (!previousDefault.isNull()) {
                (void)aiProfiles_->setDefault(previousDefault);
            }
            QString rollbackError;
            (void)aiProfiles_->save(&rollbackError);
            QMessageBox::warning(
                settingsDialog_.get(), tr("API Key 未保存"), error);
            return;
        }
    }

    completedProviderProbes_.remove(candidate.id);
    refreshProviderSettingsUi();
    emit aiProfilesChanged();
    trayController_.showMessage(
        tr("AI 服务已保存"),
        tr("“%1”已立即可用于回答卡片。")
            .arg(candidate.displayName));
}

void AppController::duplicateProviderProfile(const QUuid& profileId)
{
    if (!settingsDialog_ || aiProfiles_ == nullptr) return;
    QString error;
    const auto duplicate = aiProfiles_->duplicate(profileId, &error);
    if (!duplicate.has_value() || !aiProfiles_->save(&error)) {
        if (duplicate.has_value()) (void)aiProfiles_->remove(duplicate->id);
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法复制服务"),
            error.isEmpty() ? tr("服务档案复制失败。") : error);
        return;
    }
    refreshProviderSettingsUi();
    emit aiProfilesChanged();
    trayController_.showMessage(
        tr("已复制服务"), tr("副本不会复制原 API Key，请单独设置。"));
}

void AppController::deleteProviderProfile(
    const QUuid& profileId,
    const bool deleteCredential)
{
    if (!settingsDialog_ || aiProfiles_ == nullptr
        || credentialStore_ == nullptr) {
        return;
    }
    const auto profile = aiProfiles_->profile(profileId);
    if (!profile.has_value()) return;
    const QUuid previousDefault = aiProfiles_->defaultProfileId();
    QString error;
    if (!aiProfiles_->remove(profileId, &error) || !aiProfiles_->save(&error)) {
        (void)aiProfiles_->upsert(*profile);
        if (!previousDefault.isNull()) {
            (void)aiProfiles_->setDefault(previousDefault);
        }
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法删除服务"),
            error.isEmpty() ? tr("服务配置删除失败。") : error);
        return;
    }
    if (deleteCredential
        && !credentialStore_->remove(profile->credentialRef, &error)) {
        (void)aiProfiles_->upsert(*profile);
        if (!previousDefault.isNull()) {
            (void)aiProfiles_->setDefault(previousDefault);
        }
        QString rollbackError;
        (void)aiProfiles_->save(&rollbackError);
        QMessageBox::warning(
            settingsDialog_.get(), tr("凭据未删除"), error);
        return;
    }
    completedProviderProbes_.remove(profileId);
    refreshProviderSettingsUi();
    emit aiProfilesChanged();
}

void AppController::setDefaultProviderProfile(const QUuid& profileId)
{
    if (!settingsDialog_ || aiProfiles_ == nullptr) return;
    const QUuid previousDefault = aiProfiles_->defaultProfileId();
    QString error;
    if (!aiProfiles_->setDefault(profileId, &error)
        || !aiProfiles_->save(&error)) {
        if (!previousDefault.isNull()) {
            (void)aiProfiles_->setDefault(previousDefault);
        }
        QMessageBox::warning(
            settingsDialog_.get(), tr("无法设为默认"),
            error.isEmpty() ? tr("默认服务无法保存。") : error);
        return;
    }
    refreshProviderSettingsUi();
    emit aiProfilesChanged();
}

void AppController::exportProviderProfiles()
{
    if (!settingsDialog_ || aiProfiles_ == nullptr) return;
    QString filePath = QFileDialog::getSaveFileName(
        settingsDialog_.get(), tr("导出 AI 服务配置"),
        QStringLiteral("SnapAsk-providers.json"),
        tr("JSON 配置 (*.json)"));
    if (filePath.isEmpty()) return;
    if (!filePath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        filePath += QStringLiteral(".json");
    }

    const QByteArray bytes = QJsonDocument(aiProfiles_->exportConfiguration())
        .toJson(QJsonDocument::Indented);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(
            settingsDialog_.get(), tr("导出失败"),
            tr("配置文件无法写入所选位置。"));
        return;
    }
    trayController_.showMessage(
        tr("配置已导出"), tr("导出文件不包含任何 API Key。"));
}

void AppController::startProviderProbe(
    const QUuid& uiOperationId,
    const ui::ProviderProfileDraft& draft,
    const QString& transientApiKey,
    const bool useStoredCredential,
    const snapask::ai::ProviderProbeOperation operation)
{
    if (!settingsDialog_ || providerProbeClient_ == nullptr
        || credentialStore_ == nullptr) {
        return;
    }
    auto* widget = settingsDialog_->providerSettingsWidget();
    const auto reportFailure = [widget, uiOperationId, profileId = draft.profileId,
                                operation](const QString& message) {
        if (operation == snapask::ai::ProviderProbeOperation::ModelList) {
            widget->applyModelListResult(
                uiOperationId, profileId, false, {}, message);
        } else {
            widget->applyTestResult(
                uiOperationId, profileId, toUiOperation(operation), false,
                message);
        }
    };

    snapask::ai::ProviderProfile candidate = providerFromDraft(draft);
    QString error;
    snapask::ai::AiProfileRepository validator(QString{});
    if (!validator.upsert(candidate, &error)) {
        reportFailure(error);
        return;
    }
    if (!ensureProviderEndpointConsent(candidate.baseUrl, settingsDialog_.get())) {
        reportFailure(tr("已取消；未向此端点发送任何测试数据。"));
        return;
    }

    QString key;
    if (useStoredCredential) {
        const auto stored = credentialStore_->read(candidate.credentialRef, &error);
        if (!stored.has_value()) {
            reportFailure(error.isEmpty() ? tr("未找到系统凭据。") : error);
            return;
        }
        key = std::move(*stored);
    } else {
        key = transientApiKey;
        key.detach();
        if (key.trimmed().isEmpty()) {
            scrubSensitiveString(key);
            reportFailure(tr("请输入 API Key。"));
            return;
        }
    }

    QUuid clientOperationId;
    switch (operation) {
    case snapask::ai::ProviderProbeOperation::ModelList:
        clientOperationId = providerProbeClient_->fetchModels(
            candidate, std::move(key));
        break;
    case snapask::ai::ProviderProbeOperation::TextConnection:
        clientOperationId = providerProbeClient_->testTextConnection(
            candidate, std::move(key));
        break;
    case snapask::ai::ProviderProbeOperation::ImageUnderstanding:
        clientOperationId = providerProbeClient_->testImageUnderstanding(
            candidate, std::move(key));
        break;
    }
    scrubSensitiveString(key);
    pendingProviderProbes_.insert(
        clientOperationId, PendingProviderProbe{uiOperationId, candidate});
}

void AppController::handleProviderProbeResult(
    const snapask::ai::ProviderProbeResult& result)
{
    const auto pending = pendingProviderProbes_.take(result.operationId);
    if (pending.uiOperationId.isNull()
        || pending.candidate.id != result.providerProfileId) {
        return;
    }
    if (settingsDialog_) {
        auto* widget = settingsDialog_->providerSettingsWidget();
        if (result.operation == snapask::ai::ProviderProbeOperation::ModelList) {
            widget->applyModelListResult(
                pending.uiOperationId, result.providerProfileId,
                result.success, result.modelIds, result.message);
        } else {
            widget->applyTestResult(
                pending.uiOperationId, result.providerProfileId,
                toUiOperation(result.operation), result.success,
                result.message);
        }
    }

    CompletedProviderProbe completed{pending.candidate, result};
    if (result.operation == snapask::ai::ProviderProbeOperation::ModelList
        && result.success) {
        completed.candidate.availableModels = result.modelIds;
    }
    completedProviderProbes_.insert(result.providerProfileId, completed);

    if (aiProfiles_ == nullptr) return;
    auto stored = aiProfiles_->profile(result.providerProfileId);
    if (!stored.has_value()
        || !sameProbeConfiguration(*stored, pending.candidate)) {
        return;
    }
    stored->lastTestedAt = QDateTime::currentDateTimeUtc();
    stored->lastTestStatus = result.message;
    if (result.operation == snapask::ai::ProviderProbeOperation::ModelList
        && result.success) {
        stored->availableModels = result.modelIds;
        if (!stored->modelId.isEmpty()
            && !stored->availableModels.contains(stored->modelId)) {
            stored->availableModels.prepend(stored->modelId);
        }
    }
    QString error;
    if (aiProfiles_->upsert(*stored, &error) && aiProfiles_->save(&error)) {
        refreshProviderSettingsUi();
        emit aiProfilesChanged();
    }
}

bool AppController::ensureProviderEndpointConsent(
    const QUrl& endpoint,
    QWidget* parent)
{
    if (endpointConsentStore_ == nullptr) return false;
    if (endpointConsentStore_->isApproved(endpoint)) return true;
    const QString origin =
        snapask::infrastructure::EndpointConsentStore::normalizedOrigin(
            endpoint);
    if (origin.isEmpty()) return false;
    const auto choice = QMessageBox::question(
        parent, tr("首次使用自定义 AI 服务"),
        tr("测试请求将发送到：\n%1\n\n数据处理规则由该服务商负责。"
           "确认信任此目标后才会读取凭据并建立连接。")
            .arg(origin),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (choice != QMessageBox::Yes) return false;
    QString error;
    if (!endpointConsentStore_->approve(endpoint, &error)) {
        QMessageBox::warning(
            parent, tr("无法保存授权"),
            error.isEmpty() ? tr("自定义服务授权状态无法保存。") : error);
        return false;
    }
    return true;
}

}  // namespace snapask::app
