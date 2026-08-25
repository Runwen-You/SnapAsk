#pragma once

#include <QDateTime>
#include <QFlags>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QWidget>

class QVBoxLayout;

namespace snapask::ui {

// UI-only protocol identity. It is deliberately not a wire-format or JSON
// value; protocol serialization remains owned by the AI profile layer.
enum class ProviderUiProtocol {
    OpenAIResponses,
    ChatCompletions,
};

enum ProviderUiCapability : quint32 {
    ProviderNoCapabilities = 0,
    ProviderImageInput = 1U << 0U,
    ProviderStreaming = 1U << 1U,
    ProviderModelList = 1U << 2U,
};
Q_DECLARE_FLAGS(ProviderUiCapabilities, ProviderUiCapability)

enum class ProviderUiOperation {
    FetchModels,
    TestTextConnection,
    TestImageUnderstanding,
};

// Values safe to display in a service card. API keys, credential references,
// protocol JSON and response/request bodies must never be added to this DTO.
struct ProviderProfileSummary {
    QUuid profileId;
    QString displayName;
    ProviderUiProtocol protocol{ProviderUiProtocol::OpenAIResponses};
    QUrl baseUrl;
    QString modelId;
    QStringList availableModels;
    ProviderUiCapabilities capabilities{
        ProviderImageInput | ProviderStreaming | ProviderModelList};
    int connectTimeoutMs{15'000};
    int requestTimeoutMs{120'000};
    QUrl proxyUrl;
    QJsonObject customHeaders;
    QDateTime lastTestedAt;
    QString lastTestStatus;
    bool hasStoredApiKey{false};
};

// Non-sensitive configuration emitted by the add/edit wizard. The profile ID
// is generated before an add wizard opens so every async operation can be
// correlated without relying on "the currently selected card".
struct ProviderProfileDraft {
    QUuid profileId;
    QString displayName;
    ProviderUiProtocol protocol{ProviderUiProtocol::OpenAIResponses};
    QUrl baseUrl;
    QString modelId;
    QStringList availableModels;
    ProviderUiCapabilities capabilities{
        ProviderImageInput | ProviderStreaming | ProviderModelList};
    int connectTimeoutMs{15'000};
    int requestTimeoutMs{120'000};
    QUrl proxyUrl;
    QJsonObject customHeaders;
};

class ProviderWizardDialog;

class ProviderSettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit ProviderSettingsWidget(QWidget* parent = nullptr);
    ~ProviderSettingsWidget() override;

    ProviderSettingsWidget(const ProviderSettingsWidget&) = delete;
    ProviderSettingsWidget& operator=(const ProviderSettingsWidget&) = delete;

    void setProfiles(QList<ProviderProfileSummary> profiles,
                     const QUuid& defaultProfileId);
    [[nodiscard]] QList<ProviderProfileSummary> profiles() const;
    [[nodiscard]] QUuid defaultProfileId() const noexcept;

    // Results are accepted only when operationId and profileId both match the
    // currently open wizard's outstanding operation. Late/stale results are
    // intentionally ignored.
    void applyModelListResult(const QUuid& operationId,
                              const QUuid& profileId,
                              bool succeeded,
                              QStringList models,
                              const QString& message = {});
    void applyTestResult(const QUuid& operationId,
                         const QUuid& profileId,
                         ProviderUiOperation operation,
                         bool succeeded,
                         const QString& message = {});

signals:
    void addRequested(const QUuid& profileId,
                      const snapask::ui::ProviderProfileDraft& draft,
                      const QString& apiKey);
    void editRequested(const QUuid& profileId,
                       const snapask::ui::ProviderProfileDraft& draft,
                       const QString& replacementApiKey,
                       bool replaceStoredKey);
    void duplicateRequested(const QUuid& profileId);
    void deleteRequested(const QUuid& profileId, bool deleteStoredCredential);
    void setDefaultRequested(const QUuid& profileId);

    void fetchModelsRequested(const QUuid& operationId,
                              const QUuid& profileId,
                              const snapask::ui::ProviderProfileDraft& draft,
                              const QString& transientApiKey,
                              bool useStoredCredential);
    void testTextRequested(const QUuid& operationId,
                           const QUuid& profileId,
                           const snapask::ui::ProviderProfileDraft& draft,
                           const QString& transientApiKey,
                           bool useStoredCredential);
    void testImageRequested(const QUuid& operationId,
                            const QUuid& profileId,
                            const snapask::ui::ProviderProfileDraft& draft,
                            const QString& transientApiKey,
                            bool useStoredCredential);

    // The profile ID identifies the current default at the moment the ordinary,
    // all-profile configuration export is requested. The exported data itself
    // remains the controller/repository's responsibility and excludes keys.
    void exportRequested(const QUuid& defaultProfileId);

protected:
    // Test seams keep modal native dialogs out of offscreen tests while the
    // production implementation still asks locally before emitting an action.
    virtual bool confirmImageUnderstandingTest(QWidget* parent);
    virtual bool confirmDeleteProfile(const ProviderProfileSummary& profile,
                                      bool* deleteStoredCredential,
                                      QWidget* parent);

private:
    void buildUi();
    void rebuildCards();
    void openAddWizard();
    void openEditWizard(const QUuid& profileId);
    void openWizard(const ProviderProfileDraft& draft,
                    bool editing,
                    bool hasStoredApiKey);
    void closeWizard();
    void submitWizard();
    void requestWizardOperation(ProviderUiOperation operation);
    void requestDelete(const QUuid& profileId);
    [[nodiscard]] const ProviderProfileSummary* findProfile(
        const QUuid& profileId) const;

    QList<ProviderProfileSummary> profiles_;
    QUuid defaultProfileId_;
    ProviderWizardDialog* wizard_{nullptr};
    QWidget* cardsContainer_{nullptr};
    QVBoxLayout* cardsLayout_{nullptr};
};

} // namespace snapask::ui

Q_DECLARE_OPERATORS_FOR_FLAGS(snapask::ui::ProviderUiCapabilities)
Q_DECLARE_METATYPE(snapask::ui::ProviderUiProtocol)
Q_DECLARE_METATYPE(snapask::ui::ProviderUiCapabilities)
Q_DECLARE_METATYPE(snapask::ui::ProviderUiOperation)
Q_DECLARE_METATYPE(snapask::ui::ProviderProfileSummary)
Q_DECLARE_METATYPE(snapask::ui::ProviderProfileDraft)
