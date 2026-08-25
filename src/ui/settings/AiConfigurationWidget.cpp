#include "ui/settings/AiConfigurationWidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

namespace snapask::ui {
namespace {

constexpr int kMinimumTimeoutMs = 1'000;
constexpr int kMaximumConnectTimeoutMs = 120'000;
constexpr int kMaximumRequestTimeoutMs = 600'000;
constexpr int kDefaultConnectTimeoutMs = 15'000;
constexpr int kDefaultRequestTimeoutMs = 120'000;

QLabel* fieldLabel(const QString& text, QWidget* buddy, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setBuddy(buddy);
    return label;
}

}  // namespace

AiConfigurationWidget::AiConfigurationWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("AiConfigurationWidget"));

    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("SettingsCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 14, 16, 14);
    cardLayout->setSpacing(10);

    auto* title = new QLabel(tr("OpenAI Responses 服务"), card);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    cardLayout->addWidget(title);

    auto* description = new QLabel(
        tr("配置一个支持图片输入与流式输出的 Responses API 服务。"), card);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("SecondaryLabel"));
    cardLayout->addWidget(description);

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 4, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    nameEdit_ = new QLineEdit(card);
    nameEdit_->setObjectName(QStringLiteral("serviceNameEdit"));
    nameEdit_->setAccessibleName(tr("AI 服务名称"));
    nameEdit_->setPlaceholderText(tr("例如：OpenAI"));
    nameEdit_->setMaxLength(128);
    form->addRow(fieldLabel(tr("服务名"), nameEdit_, card), nameEdit_);

    baseUrlEdit_ = new QLineEdit(card);
    baseUrlEdit_->setObjectName(QStringLiteral("responsesBaseUrlEdit"));
    baseUrlEdit_->setAccessibleName(tr("Responses Base URL"));
    baseUrlEdit_->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
    baseUrlEdit_->setMaxLength(2'048);
    form->addRow(fieldLabel(tr("Responses Base URL"), baseUrlEdit_, card),
                 baseUrlEdit_);

    modelEdit_ = new QLineEdit(card);
    modelEdit_->setObjectName(QStringLiteral("modelIdEdit"));
    modelEdit_->setAccessibleName(tr("模型 ID"));
    modelEdit_->setPlaceholderText(tr("输入支持图片的模型 ID"));
    modelEdit_->setMaxLength(256);
    form->addRow(fieldLabel(tr("模型 ID"), modelEdit_, card), modelEdit_);

    apiKeyEdit_ = new QLineEdit(card);
    apiKeyEdit_->setObjectName(QStringLiteral("apiKeyEdit"));
    apiKeyEdit_->setAccessibleName(tr("API Key"));
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setInputMethodHints(
        Qt::ImhSensitiveData | Qt::ImhNoPredictiveText |
        Qt::ImhNoAutoUppercase);
    apiKeyEdit_->setMaxLength(1'024);
    form->addRow(fieldLabel(tr("API Key"), apiKeyEdit_, card), apiKeyEdit_);

    replaceKeyCheck_ = new QCheckBox(card);
    replaceKeyCheck_->setObjectName(QStringLiteral("replaceKeyCheck"));
    replaceKeyCheck_->setAccessibleName(tr("替换已保存的 API Key"));
    form->addRow(QString(), replaceKeyCheck_);

    connectTimeoutSpin_ = new QSpinBox(card);
    connectTimeoutSpin_->setObjectName(QStringLiteral("connectTimeoutSpin"));
    connectTimeoutSpin_->setAccessibleName(tr("连接超时，毫秒"));
    connectTimeoutSpin_->setRange(kMinimumTimeoutMs,
                                  kMaximumConnectTimeoutMs);
    connectTimeoutSpin_->setSingleStep(1'000);
    connectTimeoutSpin_->setSuffix(tr(" ms"));
    connectTimeoutSpin_->setValue(kDefaultConnectTimeoutMs);
    form->addRow(fieldLabel(tr("连接超时"), connectTimeoutSpin_, card),
                 connectTimeoutSpin_);

    requestTimeoutSpin_ = new QSpinBox(card);
    requestTimeoutSpin_->setObjectName(QStringLiteral("requestTimeoutSpin"));
    requestTimeoutSpin_->setAccessibleName(tr("总请求超时，毫秒"));
    requestTimeoutSpin_->setRange(kMinimumTimeoutMs,
                                  kMaximumRequestTimeoutMs);
    requestTimeoutSpin_->setSingleStep(1'000);
    requestTimeoutSpin_->setSuffix(tr(" ms"));
    requestTimeoutSpin_->setValue(kDefaultRequestTimeoutMs);
    form->addRow(fieldLabel(tr("总请求超时"), requestTimeoutSpin_, card),
                 requestTimeoutSpin_);
    cardLayout->addLayout(form);

    auto* credentialNotice = new QLabel(
        tr("密钥由 Windows 凭据管理器保护，不会写入普通配置。"), card);
    credentialNotice->setObjectName(QStringLiteral("credentialNoticeLabel"));
    credentialNotice->setAccessibleName(tr("API Key 存储说明"));
    credentialNotice->setWordWrap(true);
    cardLayout->addWidget(credentialNotice);

    statusLabel_ = new QLabel(card);
    statusLabel_->setObjectName(QStringLiteral("saveStatusLabel"));
    statusLabel_->setAccessibleName(tr("AI 服务配置状态"));
    statusLabel_->setWordWrap(true);
    statusLabel_->setText(tr("尚未保存。"));
    cardLayout->addWidget(statusLabel_);

    auto* deleteRow = new QHBoxLayout();
    deleteCredentialCheck_ = new QCheckBox(
        tr("同时删除 Windows 凭据管理器中的 API Key"), card);
    deleteCredentialCheck_->setObjectName(
        QStringLiteral("deleteCredentialCheck"));
    deleteCredentialCheck_->setAccessibleName(
        tr("删除服务时同时删除系统凭据"));
    deleteRow->addWidget(deleteCredentialCheck_);
    deleteRow->addStretch();
    cardLayout->addLayout(deleteRow);

    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch();
    deleteButton_ = new QPushButton(tr("删除服务"), card);
    deleteButton_->setObjectName(QStringLiteral("deleteButton"));
    deleteButton_->setAccessibleName(tr("删除 AI 服务配置"));
    actionRow->addWidget(deleteButton_);
    saveButton_ = new QPushButton(tr("保存"), card);
    saveButton_->setObjectName(QStringLiteral("saveButton"));
    saveButton_->setAccessibleName(tr("保存 AI 服务配置"));
    saveButton_->setDefault(true);
    actionRow->addWidget(saveButton_);
    cardLayout->addLayout(actionRow);

    pageLayout->addWidget(card);
    updateCredentialControls();

    connect(replaceKeyCheck_, &QCheckBox::toggled, this,
            [this](bool replace) {
                apiKeyEdit_->setEnabled(!hasStoredApiKey_ || replace);
                if (hasStoredApiKey_ && !replace) {
                    scrubApiKeyEditor();
                }
            });
    connect(connectTimeoutSpin_, &QSpinBox::valueChanged, this,
            [this](int connectTimeout) {
                requestTimeoutSpin_->setMinimum(connectTimeout);
            });
    connect(saveButton_, &QPushButton::clicked, this,
            &AiConfigurationWidget::requestSave);
    connect(deleteButton_, &QPushButton::clicked, this,
            &AiConfigurationWidget::requestDelete);
}

AiConfigurationWidget::~AiConfigurationWidget() {
    scrubApiKeyEditor();
}

void AiConfigurationWidget::setConfiguration(
    const QString& name,
    const QUrl& baseUrl,
    const QString& modelId,
    int connectTimeoutMs,
    int requestTimeoutMs,
    bool hasStoredApiKey) {
    scrubApiKeyEditor();
    nameEdit_->setText(name);
    baseUrlEdit_->setText(baseUrl.toString(QUrl::FullyEncoded));
    modelEdit_->setText(modelId);

    connectTimeoutMs = qBound(kMinimumTimeoutMs, connectTimeoutMs,
                              kMaximumConnectTimeoutMs);
    requestTimeoutMs = qBound(connectTimeoutMs, requestTimeoutMs,
                              kMaximumRequestTimeoutMs);
    connectTimeoutSpin_->setValue(connectTimeoutMs);
    requestTimeoutSpin_->setMinimum(connectTimeoutMs);
    requestTimeoutSpin_->setValue(requestTimeoutMs);

    hasStoredApiKey_ = hasStoredApiKey;
    updateCredentialControls();
}

void AiConfigurationWidget::setSaveStatus(bool saved,
                                          const QString& message) {
    if (!message.isEmpty()) {
        statusLabel_->setText(message);
    } else {
        statusLabel_->setText(saved ? tr("配置已保存。")
                                    : tr("配置保存失败，请检查输入。"));
    }
    statusLabel_->setProperty("saveSucceeded", saved);
    statusLabel_->setAccessibleDescription(
        saved ? tr("保存成功") : tr("保存失败"));
}

void AiConfigurationWidget::requestSave() {
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        showValidationError(tr("请输入服务名。"));
        nameEdit_->setFocus();
        return;
    }

    QUrl baseUrl;
    QString urlError;
    if (!validateBaseUrl(baseUrlEdit_->text(), &baseUrl, &urlError)) {
        showValidationError(urlError);
        baseUrlEdit_->setFocus();
        return;
    }

    const QString model = modelEdit_->text().trimmed();
    if (model.isEmpty()) {
        showValidationError(tr("请输入模型 ID。"));
        modelEdit_->setFocus();
        return;
    }

    const int connectTimeout = connectTimeoutSpin_->value();
    const int requestTimeout = requestTimeoutSpin_->value();
    if (requestTimeout < connectTimeout) {
        showValidationError(tr("总请求超时不能短于连接超时。"));
        requestTimeoutSpin_->setFocus();
        return;
    }

    const bool replaceKey =
        !hasStoredApiKey_ || replaceKeyCheck_->isChecked();
    QString apiKey;
    if (replaceKey) {
        apiKey = apiKeyEdit_->text();
        bool containsNonWhitespace = false;
        for (const QChar character : std::as_const(apiKey)) {
            if (!character.isSpace()) {
                containsNonWhitespace = true;
                break;
            }
        }
        if (!containsNonWhitespace) {
            scrubApiKeyEditor();
            overwriteAndClear(apiKey);
            showValidationError(hasStoredApiKey_
                                    ? tr("请输入用于替换的 API Key。")
                                    : tr("请输入 API Key。"));
            apiKeyEdit_->setFocus();
            return;
        }
    }

    statusLabel_->setText(tr("正在保存…"));
    emit saveRequested(name, baseUrl, model, connectTimeout, requestTimeout,
                       apiKey, replaceKey);

    // Keep the editor's original backing buffer alive in apiKey until the
    // widget has replaced its text and cleared its undo history, then overwrite
    // the remaining local copy as well.
    scrubApiKeyEditor();
    overwriteAndClear(apiKey);
    if (hasStoredApiKey_) {
        replaceKeyCheck_->setChecked(false);
    }
}

void AiConfigurationWidget::requestDelete() {
    statusLabel_->setText(tr("正在删除…"));
    emit deleteRequested(deleteCredentialCheck_->isChecked());
    scrubApiKeyEditor();
}

void AiConfigurationWidget::updateCredentialControls() {
    if (hasStoredApiKey_) {
        replaceKeyCheck_->setText(tr("替换已保存的 API Key"));
        replaceKeyCheck_->setEnabled(true);
        replaceKeyCheck_->setChecked(false);
        apiKeyEdit_->setEnabled(false);
        apiKeyEdit_->setPlaceholderText(
            tr("已安全保存；如需更换请勾选下方选项"));
        deleteCredentialCheck_->setEnabled(true);
        deleteCredentialCheck_->setChecked(true);
    } else {
        replaceKeyCheck_->setText(tr("将 API Key 保存到 Windows 凭据管理器"));
        replaceKeyCheck_->setChecked(true);
        replaceKeyCheck_->setEnabled(false);
        apiKeyEdit_->setEnabled(true);
        apiKeyEdit_->setPlaceholderText(tr("输入 API Key"));
        deleteCredentialCheck_->setChecked(false);
        deleteCredentialCheck_->setEnabled(false);
    }
}

void AiConfigurationWidget::scrubApiKeyEditor() {
    if (apiKeyEdit_ == nullptr) {
        return;
    }
    QString editorText = apiKeyEdit_->text();
    const qsizetype characterCount = editorText.size();
    if (characterCount > 0) {
        // setText clears QLineEdit's undo/redo history, preventing recovery of
        // the previous secret with Ctrl+Z. The replacement contains no secret.
        apiKeyEdit_->setText(QString(characterCount, QChar(u'\0')));
    }
    apiKeyEdit_->clear();
    overwriteAndClear(editorText);
}

void AiConfigurationWidget::showValidationError(const QString& message) {
    setSaveStatus(false, message);
}

bool AiConfigurationWidget::isLoopbackHost(const QString& host) {
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
        host == QStringLiteral("::1")) {
        return true;
    }

    const QStringList octets = host.split(QLatin1Char('.'));
    if (octets.size() != 4) {
        return false;
    }
    for (const QString& octet : octets) {
        bool ok = false;
        const int value = octet.toInt(&ok);
        if (!ok || value < 0 || value > 255) {
            return false;
        }
    }
    return octets.constFirst().toInt() == 127;
}

bool AiConfigurationWidget::validateBaseUrl(const QString& text,
                                            QUrl* result,
                                            QString* error) {
    const QString trimmed = text.trimmed();
    const QUrl url(trimmed, QUrl::StrictMode);
    if (url.isEmpty() || !url.isValid() || url.isRelative() ||
        url.host().isEmpty()) {
        if (error != nullptr) {
            *error = tr("请输入完整有效的 Responses Base URL。");
        }
        return false;
    }
    if (!url.userInfo().isEmpty()) {
        if (error != nullptr) {
            *error = tr("Base URL 不能包含用户名、密码或 API Key。");
        }
        return false;
    }
    if (!url.query().isEmpty() || url.hasFragment()) {
        if (error != nullptr) {
            *error = tr("Base URL 不能包含查询参数或片段。");
        }
        return false;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("https") &&
        !(scheme == QStringLiteral("http") && isLoopbackHost(url.host()))) {
        if (error != nullptr) {
            *error = tr("公网服务必须使用 HTTPS；HTTP 仅允许本机地址。");
        }
        return false;
    }

    if (result != nullptr) {
        *result = url;
    }
    return true;
}

void AiConfigurationWidget::overwriteAndClear(QString& sensitiveText) {
    if (!sensitiveText.isEmpty()) {
        sensitiveText.detach();
        QChar* characters = sensitiveText.data();
        for (qsizetype index = 0; index < sensitiveText.size(); ++index) {
            characters[index] = QChar(u'\0');
        }
    }
    sensitiveText.clear();
    sensitiveText.squeeze();
}

}  // namespace snapask::ui
