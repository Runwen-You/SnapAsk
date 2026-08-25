#pragma once

#include "ai/AiTypes.h"
#include "app/TrayController.h"
#include "platform/windows/GlobalHotkey.h"
#include "platform/windows/ScreenCapture.h"
#include "ui/common/ThemeTokens.h"

#include <QList>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <memory>

class QKeySequence;
class QImage;
class QUrl;
class QWidget;

namespace snapask::ai {
class AiNetworkClient;
class AiProfileRepository;
class ProviderProbeClient;
}

namespace snapask::platform::windows {
class CredentialStore;
class SystemLifecycleMonitor;
}

namespace snapask::infrastructure {
class EndpointConsentStore;
}

namespace snapask::ui {
class SettingsDialog;
enum class ProviderUiOperation;
struct ProviderProfileDraft;
}

namespace snapask::ui::capture {
class CaptureOverlay;
enum class CaptureHandoffAction;
}

namespace snapask::app {

class AppController final : public QObject {
    Q_OBJECT

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    [[nodiscard]] bool start();

public slots:
    void activate();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void aiProfilesChanged();

private slots:
    void openSettings();
    void applyTheme(snapask::ui::ThemeMode mode);
    void startCapture();
    void openClipboardImage();
    void registerCaptureHotkey(const QKeySequence& sequence);

private:
    void openEditor(QImage image);
    void openCapturedEditor(
        QImage image,
        const QRect& desktopRectPx,
        snapask::ui::capture::CaptureHandoffAction action);
    void applyGlassBackdrop(QWidget* window);
    void restoreHiddenWindows();
    [[nodiscard]] bool ensurePrivacyNoticeAccepted();
    void handleSystemEnvironmentChange();
    void cancelActiveCaptureForEnvironmentChange();
    void initializeAiConfiguration();
    void refreshProviderSettingsUi();
    void saveProviderProfile(
        const snapask::ui::ProviderProfileDraft& draft,
        const QString& apiKey,
        bool replaceKey,
        bool editing);
    void duplicateProviderProfile(const QUuid& profileId);
    void deleteProviderProfile(
        const QUuid& profileId,
        bool deleteCredential);
    void setDefaultProviderProfile(const QUuid& profileId);
    void exportProviderProfiles();
    void startProviderProbe(
        const QUuid& uiOperationId,
        const snapask::ui::ProviderProfileDraft& draft,
        const QString& transientApiKey,
        bool useStoredCredential,
        snapask::ai::ProviderProbeOperation operation);
    void handleProviderProbeResult(
        const snapask::ai::ProviderProbeResult& result);
    [[nodiscard]] bool ensureProviderEndpointConsent(
        const QUrl& endpoint,
        QWidget* parent);

    struct PendingProviderProbe {
        QUuid uiOperationId;
        snapask::ai::ProviderProfile candidate;
    };

    struct CompletedProviderProbe {
        snapask::ai::ProviderProfile candidate;
        snapask::ai::ProviderProbeResult result;
    };

    TrayController trayController_;
    snapask::platform::windows::GlobalHotkey globalHotkey_;
    std::unique_ptr<snapask::ui::SettingsDialog> settingsDialog_;
    std::unique_ptr<snapask::ui::capture::CaptureOverlay> captureOverlay_;
    std::unique_ptr<snapask::ai::AiNetworkClient> aiNetworkClient_;
    std::unique_ptr<snapask::ai::ProviderProbeClient> providerProbeClient_;
    std::unique_ptr<snapask::ai::AiProfileRepository> aiProfiles_;
    std::unique_ptr<snapask::platform::windows::CredentialStore> credentialStore_;
    std::unique_ptr<snapask::platform::windows::SystemLifecycleMonitor>
        lifecycleMonitor_;
    std::unique_ptr<snapask::infrastructure::EndpointConsentStore>
        endpointConsentStore_;
    QHash<QUuid, PendingProviderProbe> pendingProviderProbes_;
    QHash<QUuid, CompletedProviderProbe> completedProviderProbes_;
    QList<QPointer<QWidget>> hiddenForCapture_;
    snapask::ui::ThemeMode themeMode_ = snapask::ui::ThemeMode::System;
    QString hotkeyError_;
    QUuid activeCaptureJobId_;
    bool captureInProgress_ = false;
};

}  // namespace snapask::app
