#pragma once

#include <QUrl>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace snapask::ui {

class AiConfigurationWidget final : public QWidget {
    Q_OBJECT

public:
    explicit AiConfigurationWidget(QWidget* parent = nullptr);
    ~AiConfigurationWidget() override;

    // The persisted API key is intentionally not part of this API. Callers may
    // disclose only whether a Credential Manager entry already exists.
    void setConfiguration(const QString& name,
                          const QUrl& baseUrl,
                          const QString& modelId,
                          int connectTimeoutMs,
                          int requestTimeoutMs,
                          bool hasStoredApiKey);
    void setSaveStatus(bool saved, const QString& message = {});

signals:
    void saveRequested(const QString& name,
                       const QUrl& baseUrl,
                       const QString& model,
                       int connectTimeout,
                       int requestTimeout,
                       const QString& apiKey,
                       bool replaceKey);
    void deleteRequested(bool deleteCredential);

private:
    void requestSave();
    void requestDelete();
    void updateCredentialControls();
    void scrubApiKeyEditor();
    void showValidationError(const QString& message);

    [[nodiscard]] static bool isLoopbackHost(const QString& host);
    [[nodiscard]] static bool validateBaseUrl(const QString& text,
                                              QUrl* result,
                                              QString* error);
    static void overwriteAndClear(QString& sensitiveText);

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* baseUrlEdit_ = nullptr;
    QLineEdit* modelEdit_ = nullptr;
    QLineEdit* apiKeyEdit_ = nullptr;
    QCheckBox* replaceKeyCheck_ = nullptr;
    QSpinBox* connectTimeoutSpin_ = nullptr;
    QSpinBox* requestTimeoutSpin_ = nullptr;
    QCheckBox* deleteCredentialCheck_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    bool hasStoredApiKey_{false};
};

}  // namespace snapask::ui
