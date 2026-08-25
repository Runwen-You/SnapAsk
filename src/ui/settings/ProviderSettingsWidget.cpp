#include "ui/settings/ProviderSettingsWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace snapask::ui {
namespace {

constexpr int kMinimumTimeoutMs = 1'000;
constexpr int kMaximumConnectTimeoutMs = 120'000;
constexpr int kMaximumRequestTimeoutMs = 600'000;
constexpr qsizetype kMaximumModels = 1'000;

QString protocolLabel(const ProviderUiProtocol protocol)
{
    return protocol == ProviderUiProtocol::OpenAIResponses
        ? ProviderSettingsWidget::tr("OpenAI Responses")
        : ProviderSettingsWidget::tr("Chat Completions 兼容接口");
}

QString canonicalHostLabel(const QUrl& url)
{
    if (url.host().isEmpty()) {
        return ProviderSettingsWidget::tr("未设置目标域名");
    }
    QString result = url.host();
    if (url.port() >= 0) {
        result += QStringLiteral(":%1").arg(url.port());
    }
    return result;
}

bool isLoopbackHost(const QString& host)
{
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || host == QStringLiteral("::1")) {
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
    return octets.first().toInt() == 127;
}

bool validateBaseUrl(const QString& text, QUrl* result, QString* error)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()) {
        if (error != nullptr) {
            *error = ProviderSettingsWidget::tr("请输入完整有效的 Base URL。");
        }
        return false;
    }
    if (!url.userInfo().isEmpty()) {
        if (error != nullptr) {
            *error = ProviderSettingsWidget::tr(
                "Base URL 不能包含用户名、密码或 API Key。");
        }
        return false;
    }
    if (!url.query().isEmpty() || url.hasFragment()) {
        if (error != nullptr) {
            *error = ProviderSettingsWidget::tr(
                "Base URL 不能包含查询参数或片段。");
        }
        return false;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("https")
        && !(scheme == QStringLiteral("http") && isLoopbackHost(url.host()))) {
        if (error != nullptr) {
            *error = ProviderSettingsWidget::tr(
                "公网服务必须使用 HTTPS；HTTP 仅允许本机地址。");
        }
        return false;
    }
    if (result != nullptr) {
        *result = url;
    }
    return true;
}

bool validateProxyUrl(const QString& text, QUrl* result, QString* error)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (result != nullptr) {
            *result = {};
        }
        return true;
    }
    const QUrl url(trimmed, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        || !url.userInfo().isEmpty() || !url.query().isEmpty()
        || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) {
        if (error != nullptr) {
            *error = ProviderSettingsWidget::tr(
                "代理必须是无凭据、无查询参数的 HTTP/HTTPS 地址。");
        }
        return false;
    }
    if (result != nullptr) {
        *result = url;
    }
    return true;
}

bool parseCustomHeaders(const QString& text, QJsonObject* result, QString* error)
{
    static const QSet<QString> clientHeaderNames{
        QStringLiteral("x-client"),
        QStringLiteral("x-client-name"),
    };
    static const QSet<QString> publicRegionValues{
        QStringLiteral("global"),
        QStringLiteral("test"),
        QStringLiteral("southeast-asia"),
    };

    QJsonObject parsed;
    QSet<QString> normalizedNames;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& originalLine : lines) {
        const QString line = originalLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (parsed.size() >= 3) {
            if (error != nullptr) {
                *error = ProviderSettingsWidget::tr("公开元数据请求头数量不能超过 3 个。");
            }
            return false;
        }
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            if (error != nullptr) {
                *error = ProviderSettingsWidget::tr(
                    "自定义请求头必须每行使用“名称: 值”格式。");
            }
            return false;
        }
        const QString name = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        const QString normalized = name.toLower();
        const bool clientMetadata = clientHeaderNames.contains(normalized);
        const bool regionMetadata = normalized == QStringLiteral("x-region");
        if (!clientMetadata && !regionMetadata) {
            if (error != nullptr) {
                *error = ProviderSettingsWidget::tr(
                    "普通配置仅允许公开元数据请求头 X-Client、X-Client-Name "
                    "或 X-Region；密钥和鉴权信息请使用 API Key 输入框。");
            }
            return false;
        }
        if (normalizedNames.contains(normalized)) {
            if (error != nullptr) {
                *error = ProviderSettingsWidget::tr("自定义请求头名称不能重复。");
            }
            return false;
        }
        const bool valueAllowed = clientMetadata
            ? value == QStringLiteral("SnapAsk")
            : publicRegionValues.contains(value);
        if (!valueAllowed) {
            if (error != nullptr) {
                *error = ProviderSettingsWidget::tr(
                    "只接受公开元数据值：X-Client/X-Client-Name 仅允许 "
                    "SnapAsk；X-Region 仅允许 global、test 或 southeast-asia。"
                    "凭据请使用 API Key 输入框。");
            }
            return false;
        }
        normalizedNames.insert(normalized);
        parsed.insert(name, value);
    }
    if (result != nullptr) {
        *result = parsed;
    }
    return true;
}

QStringList normalizedModels(QStringList models)
{
    QStringList result;
    QSet<QString> seen;
    for (QString model : models) {
        model = model.trimmed();
        if (model.isEmpty() || model.size() > 256 || seen.contains(model)) {
            continue;
        }
        seen.insert(model);
        result.append(std::move(model));
        if (result.size() >= kMaximumModels) {
            break;
        }
    }
    return result;
}

void scrubSensitiveString(QString& value)
{
    if (!value.isEmpty()) {
        value.detach();
        value.fill(QChar{});
    }
    value.clear();
    value.squeeze();
}

void scrubSensitiveEditor(QLineEdit* editor)
{
    if (editor == nullptr) {
        return;
    }
    QString previous = editor->text();
    if (!previous.isEmpty()) {
        editor->setText(QString(previous.size(), QChar{}));
    }
    editor->clear();
    editor->setModified(false);
    scrubSensitiveString(previous);
}

QString headersForEditor(const QJsonObject& headers)
{
    QStringList lines;
    QStringList names = headers.keys();
    names.sort(Qt::CaseInsensitive);
    for (const QString& name : names) {
        lines.append(name + QStringLiteral(": ") + headers.value(name).toString());
    }
    return lines.join(QLatin1Char('\n'));
}

QLabel* createBadge(const QString& text, QWidget* parent)
{
    auto* badge = new QLabel(text, parent);
    badge->setObjectName(QStringLiteral("providerCapabilityBadge"));
    badge->setProperty("capabilityBadge", true);
    badge->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    badge->setMargin(4);
    return badge;
}

QString boundedStatus(QString message)
{
    message = message.trimmed();
    if (message.size() > 512) {
        message.truncate(512);
        message.append(QChar(0x2026));
    }
    return message;
}

} // namespace

class ProviderWizardDialog final : public QDialog {
public:
    ProviderWizardDialog(ProviderProfileDraft initialDraft,
                         const bool editing,
                         const bool hasStoredApiKey,
                         QWidget* parent)
        : QDialog(parent)
        , initialDraft_(std::move(initialDraft))
        , editing_(editing)
        , hasStoredApiKey_(hasStoredApiKey)
    {
        setObjectName(QStringLiteral("providerWizardDialog"));
        setWindowTitle(editing_ ? tr("编辑 AI 服务") : tr("添加 AI 服务"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose, false);
        resize(560, 570);
        buildUi();
        populate();
        updateNavigation();
    }

    ~ProviderWizardDialog() override
    {
        scrubCredential();
    }

    [[nodiscard]] bool editing() const noexcept
    {
        return editing_;
    }

    [[nodiscard]] QUuid profileId() const noexcept
    {
        return initialDraft_.profileId;
    }

    [[nodiscard]] QPushButton* submitButton() const noexcept
    {
        return submitButton_;
    }

    [[nodiscard]] QPushButton* fetchModelsButton() const noexcept
    {
        return fetchModelsButton_;
    }

    [[nodiscard]] QPushButton* textTestButton() const noexcept
    {
        return textTestButton_;
    }

    [[nodiscard]] QPushButton* imageTestButton() const noexcept
    {
        return imageTestButton_;
    }

    [[nodiscard]] ProviderProfileDraft draft(
        const bool requireModel,
        QString* error) const
    {
        ProviderProfileDraft result;
        result.profileId = initialDraft_.profileId;
        result.displayName = nameEdit_->text().trimmed();
        if (result.profileId.isNull()) {
            if (error != nullptr) {
                *error = tr("服务档案 ID 无效。");
            }
            return {};
        }
        if (result.displayName.isEmpty() || result.displayName.size() > 128) {
            if (error != nullptr) {
                *error = tr("请输入不超过 128 个字符的服务名称。");
            }
            return {};
        }

        result.protocol = static_cast<ProviderUiProtocol>(
            protocolCombo_->currentData().toInt());
        if (!validateBaseUrl(baseUrlEdit_->text(), &result.baseUrl, error)) {
            return {};
        }

        result.modelId = modelCombo_->currentText().trimmed();
        if (requireModel && (result.modelId.isEmpty() || result.modelId.size() > 256)) {
            if (error != nullptr) {
                *error = tr("请选择模型，或手动输入不超过 256 个字符的模型 ID。");
            }
            return {};
        }
        QStringList models;
        for (int index = 0; index < modelCombo_->count(); ++index) {
            models.append(modelCombo_->itemText(index));
        }
        if (!result.modelId.isEmpty()) {
            models.prepend(result.modelId);
        }
        result.availableModels = normalizedModels(std::move(models));

        result.capabilities = ProviderUiCapabilities{};
        if (imageCapabilityCheck_->isChecked()) {
            result.capabilities |= ProviderImageInput;
        }
        if (streamingCapabilityCheck_->isChecked()) {
            result.capabilities |= ProviderStreaming;
        }
        if (modelListCapabilityCheck_->isChecked()) {
            result.capabilities |= ProviderModelList;
        }

        result.connectTimeoutMs = connectTimeoutSpin_->value();
        result.requestTimeoutMs = requestTimeoutSpin_->value();
        if (result.requestTimeoutMs < result.connectTimeoutMs) {
            if (error != nullptr) {
                *error = tr("总请求超时不能短于连接超时。");
            }
            return {};
        }
        if (!validateProxyUrl(proxyUrlEdit_->text(), &result.proxyUrl, error)) {
            return {};
        }
        if (!parseCustomHeaders(
                customHeadersEdit_->toPlainText(), &result.customHeaders, error)) {
            return {};
        }
        return result;
    }

    [[nodiscard]] QString transientApiKey(
        bool* useStoredCredential,
        bool* replaceStoredKey,
        QString* error) const
    {
        const bool replace = !hasStoredApiKey_ || replaceKeyCheck_->isChecked();
        if (useStoredCredential != nullptr) {
            *useStoredCredential = hasStoredApiKey_ && !replace;
        }
        if (replaceStoredKey != nullptr) {
            *replaceStoredKey = replace;
        }
        if (!replace) {
            return {};
        }

        QString key = apiKeyEdit_->text();
        key.detach();
        bool hasNonWhitespace = false;
        for (const QChar character : std::as_const(key)) {
            if (!character.isSpace()) {
                hasNonWhitespace = true;
                break;
            }
        }
        if (!hasNonWhitespace) {
            scrubSensitiveString(key);
            if (error != nullptr) {
                *error = hasStoredApiKey_
                    ? tr("请输入用于替换的 API Key。")
                    : tr("请输入 API Key。");
            }
            return {};
        }
        return key;
    }

    void showValidationError(const QString& message)
    {
        validationLabel_->setText(message);
        validationLabel_->setProperty("validationError", true);
    }

    void scrubCredential()
    {
        scrubSensitiveEditor(apiKeyEdit_);
    }

    void beginOperation(
        const QUuid& operationId,
        const ProviderUiOperation operation)
    {
        pendingOperationId_ = operationId;
        pendingOperation_ = operation;
        operationPending_ = true;
        fetchModelsButton_->setEnabled(false);
        textTestButton_->setEnabled(false);
        imageTestButton_->setEnabled(false);
        if (operation == ProviderUiOperation::FetchModels) {
            modelStatusLabel_->setText(tr("正在获取模型列表…"));
        } else {
            testStatusLabel_->setText(tr("正在测试服务…"));
        }
    }

    [[nodiscard]] bool matchesOperation(
        const QUuid& operationId,
        const QUuid& profileId,
        const ProviderUiOperation operation) const noexcept
    {
        return operationPending_ && operationId == pendingOperationId_
            && profileId == initialDraft_.profileId
            && operation == pendingOperation_;
    }

    void completeModelList(
        const bool succeeded,
        QStringList models,
        const QString& message)
    {
        completeOperationControls();
        models = normalizedModels(std::move(models));
        if (succeeded && !models.isEmpty()) {
            const QString previous = modelCombo_->currentText().trimmed();
            modelCombo_->clear();
            modelCombo_->addItems(models);
            const int previousIndex = modelCombo_->findText(previous);
            if (previousIndex >= 0) {
                modelCombo_->setCurrentIndex(previousIndex);
            } else {
                modelCombo_->setCurrentIndex(0);
            }
            modelStatusLabel_->setText(
                message.isEmpty()
                    ? tr("已获取 %1 个模型；仍可手动输入模型 ID。").arg(models.size())
                    : boundedStatus(message));
            return;
        }
        modelCombo_->setEditable(true);
        modelCombo_->setFocus(Qt::OtherFocusReason);
        modelStatusLabel_->setText(
            message.isEmpty()
                ? tr("无法获取模型列表，请手动输入模型 ID。")
                : boundedStatus(message));
    }

    void completeTest(const bool succeeded, const QString& message)
    {
        completeOperationControls();
        testStatusLabel_->setText(
            message.isEmpty()
                ? (succeeded ? tr("测试通过。") : tr("测试失败，请检查配置。"))
                : boundedStatus(message));
        testStatusLabel_->setProperty("testSucceeded", succeeded);
    }

private:
    void buildUi()
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(18, 16, 18, 16);
        root->setSpacing(12);

        stepLabel_ = new QLabel(this);
        stepLabel_->setObjectName(QStringLiteral("wizardStepLabel"));
        QFont stepFont = stepLabel_->font();
        stepFont.setWeight(QFont::DemiBold);
        stepLabel_->setFont(stepFont);
        root->addWidget(stepLabel_);

        pages_ = new QStackedWidget(this);
        pages_->setObjectName(QStringLiteral("providerWizardPages"));
        pages_->addWidget(buildBasicsPage());
        pages_->addWidget(buildModelsPage());
        pages_->addWidget(buildTestAndAdvancedPage());
        root->addWidget(pages_, 1);

        validationLabel_ = new QLabel(this);
        validationLabel_->setObjectName(QStringLiteral("wizardValidationLabel"));
        validationLabel_->setWordWrap(true);
        root->addWidget(validationLabel_);

        auto* navigation = new QHBoxLayout();
        cancelButton_ = new QPushButton(tr("取消"), this);
        cancelButton_->setObjectName(QStringLiteral("wizardCancelButton"));
        backButton_ = new QPushButton(tr("上一步"), this);
        backButton_->setObjectName(QStringLiteral("wizardBackButton"));
        nextButton_ = new QPushButton(tr("下一步"), this);
        nextButton_->setObjectName(QStringLiteral("wizardNextButton"));
        submitButton_ = new QPushButton(editing_ ? tr("保存修改") : tr("添加服务"), this);
        submitButton_->setObjectName(QStringLiteral("wizardSubmitButton"));
        submitButton_->setDefault(true);
        navigation->addWidget(cancelButton_);
        navigation->addStretch(1);
        navigation->addWidget(backButton_);
        navigation->addWidget(nextButton_);
        navigation->addWidget(submitButton_);
        root->addLayout(navigation);

        connect(cancelButton_, &QPushButton::clicked, this, [this] {
            scrubCredential();
            reject();
        });
        connect(backButton_, &QPushButton::clicked, this, [this] {
            pages_->setCurrentIndex(std::max(0, pages_->currentIndex() - 1));
            validationLabel_->clear();
            updateNavigation();
        });
        connect(nextButton_, &QPushButton::clicked, this, [this] {
            QString error;
            const bool requireModel = pages_->currentIndex() >= 1;
            const ProviderProfileDraft checked = draft(requireModel, &error);
            if (checked.profileId.isNull()) {
                showValidationError(error);
                return;
            }
            if (pages_->currentIndex() == 0) {
                bool useStored = false;
                bool replace = false;
                QString key = transientApiKey(&useStored, &replace, &error);
                Q_UNUSED(useStored)
                Q_UNUSED(replace)
                const bool credentialAccepted = !key.isEmpty()
                    || (hasStoredApiKey_ && !replaceKeyCheck_->isChecked());
                scrubSensitiveString(key);
                if (!credentialAccepted) {
                    showValidationError(error);
                    return;
                }
            }
            pages_->setCurrentIndex(
                std::min(pages_->count() - 1, pages_->currentIndex() + 1));
            validationLabel_->clear();
            updateNavigation();
        });
        connect(connectTimeoutSpin_, &QSpinBox::valueChanged,
                requestTimeoutSpin_, &QSpinBox::setMinimum);
        connect(replaceKeyCheck_, &QCheckBox::toggled, this, [this](const bool replace) {
            apiKeyEdit_->setEnabled(!hasStoredApiKey_ || replace);
            if (hasStoredApiKey_ && !replace) {
                scrubSensitiveEditor(apiKeyEdit_);
            }
        });
        connect(advancedToggle_, &QToolButton::toggled,
                advancedPanel_, &QWidget::setVisible);
    }

    QWidget* buildBasicsPage()
    {
        auto* page = new QWidget(this);
        page->setObjectName(QStringLiteral("providerBasicsPage"));
        auto* layout = new QVBoxLayout(page);
        auto* description = new QLabel(
            tr("选择协议并填写服务地址。仅在你明确获取模型、测试或发送时才会联网。"),
            page);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* form = new QFormLayout();
        nameEdit_ = new QLineEdit(page);
        nameEdit_->setObjectName(QStringLiteral("providerNameEdit"));
        nameEdit_->setMaxLength(128);
        form->addRow(tr("服务名称"), nameEdit_);

        protocolCombo_ = new QComboBox(page);
        protocolCombo_->setObjectName(QStringLiteral("providerProtocolCombo"));
        protocolCombo_->addItem(
            tr("OpenAI Responses API"),
            static_cast<int>(ProviderUiProtocol::OpenAIResponses));
        protocolCombo_->addItem(
            tr("OpenAI-compatible Chat Completions"),
            static_cast<int>(ProviderUiProtocol::ChatCompletions));
        form->addRow(tr("协议"), protocolCombo_);

        baseUrlEdit_ = new QLineEdit(page);
        baseUrlEdit_->setObjectName(QStringLiteral("providerBaseUrlEdit"));
        baseUrlEdit_->setMaxLength(2'048);
        baseUrlEdit_->setPlaceholderText(QStringLiteral("https://api.example/v1"));
        form->addRow(tr("Base URL"), baseUrlEdit_);

        apiKeyEdit_ = new QLineEdit(page);
        apiKeyEdit_->setObjectName(QStringLiteral("providerApiKeyEdit"));
        apiKeyEdit_->setEchoMode(QLineEdit::Password);
        apiKeyEdit_->setMaxLength(1'024);
        apiKeyEdit_->setInputMethodHints(
            Qt::ImhSensitiveData | Qt::ImhNoPredictiveText
            | Qt::ImhNoAutoUppercase);
        form->addRow(tr("API Key"), apiKeyEdit_);

        replaceKeyCheck_ = new QCheckBox(tr("替换已安全保存的 API Key"), page);
        replaceKeyCheck_->setObjectName(QStringLiteral("replaceStoredKeyCheck"));
        form->addRow(QString(), replaceKeyCheck_);
        layout->addLayout(form);

        auto* credentialNotice = new QLabel(
            tr("密钥不会进入服务档案、普通配置或日志；保存由上层控制器交给 Windows 凭据管理器。"),
            page);
        credentialNotice->setObjectName(QStringLiteral("providerCredentialNotice"));
        credentialNotice->setWordWrap(true);
        layout->addWidget(credentialNotice);
        layout->addStretch(1);
        return page;
    }

    QWidget* buildModelsPage()
    {
        auto* page = new QWidget(this);
        page->setObjectName(QStringLiteral("providerModelsPage"));
        auto* layout = new QVBoxLayout(page);
        auto* description = new QLabel(
            tr("可自动尝试获取模型列表；兼容服务不支持时可直接手动输入模型 ID。"),
            page);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* modelRow = new QHBoxLayout();
        modelCombo_ = new QComboBox(page);
        modelCombo_->setObjectName(QStringLiteral("providerModelCombo"));
        modelCombo_->setEditable(true);
        modelCombo_->setInsertPolicy(QComboBox::NoInsert);
        modelCombo_->setMaxVisibleItems(20);
        modelRow->addWidget(modelCombo_, 1);
        fetchModelsButton_ = new QPushButton(tr("获取模型列表"), page);
        fetchModelsButton_->setObjectName(QStringLiteral("fetchModelsButton"));
        modelRow->addWidget(fetchModelsButton_);
        layout->addLayout(modelRow);

        modelStatusLabel_ = new QLabel(
            tr("尚未获取；模型 ID 可手动输入。"), page);
        modelStatusLabel_->setObjectName(QStringLiteral("modelOperationStatus"));
        modelStatusLabel_->setWordWrap(true);
        layout->addWidget(modelStatusLabel_);

        auto* capabilityTitle = new QLabel(tr("服务能力标记"), page);
        QFont capabilityFont = capabilityTitle->font();
        capabilityFont.setWeight(QFont::DemiBold);
        capabilityTitle->setFont(capabilityFont);
        layout->addWidget(capabilityTitle);
        imageCapabilityCheck_ = new QCheckBox(tr("图片输入"), page);
        imageCapabilityCheck_->setObjectName(QStringLiteral("imageCapabilityCheck"));
        streamingCapabilityCheck_ = new QCheckBox(tr("流式输出"), page);
        streamingCapabilityCheck_->setObjectName(QStringLiteral("streamingCapabilityCheck"));
        modelListCapabilityCheck_ = new QCheckBox(tr("模型列表"), page);
        modelListCapabilityCheck_->setObjectName(QStringLiteral("modelListCapabilityCheck"));
        layout->addWidget(imageCapabilityCheck_);
        layout->addWidget(streamingCapabilityCheck_);
        layout->addWidget(modelListCapabilityCheck_);
        layout->addStretch(1);
        return page;
    }

    QWidget* buildTestAndAdvancedPage()
    {
        auto* page = new QWidget(this);
        page->setObjectName(QStringLiteral("providerTestPage"));
        auto* layout = new QVBoxLayout(page);
        auto* description = new QLabel(
            tr("测试只使用固定文本或内置无敏感图片，不会使用当前截图。"), page);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* testRow = new QHBoxLayout();
        textTestButton_ = new QPushButton(tr("测试连接"), page);
        textTestButton_->setObjectName(QStringLiteral("testTextButton"));
        imageTestButton_ = new QPushButton(tr("测试图片理解"), page);
        imageTestButton_->setObjectName(QStringLiteral("testImageButton"));
        testRow->addWidget(textTestButton_);
        testRow->addWidget(imageTestButton_);
        testRow->addStretch(1);
        layout->addLayout(testRow);

        testStatusLabel_ = new QLabel(tr("尚未测试。"), page);
        testStatusLabel_->setObjectName(QStringLiteral("providerTestStatus"));
        testStatusLabel_->setWordWrap(true);
        layout->addWidget(testStatusLabel_);

        advancedToggle_ = new QToolButton(page);
        advancedToggle_->setObjectName(QStringLiteral("advancedOptionsToggle"));
        advancedToggle_->setText(tr("高级选项"));
        advancedToggle_->setCheckable(true);
        advancedToggle_->setChecked(false);
        advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        advancedToggle_->setArrowType(Qt::RightArrow);
        connect(advancedToggle_, &QToolButton::toggled, advancedToggle_,
                [this](const bool expanded) {
            advancedToggle_->setArrowType(
                expanded ? Qt::DownArrow : Qt::RightArrow);
        });
        layout->addWidget(advancedToggle_);

        advancedPanel_ = new QFrame(page);
        advancedPanel_->setObjectName(QStringLiteral("advancedOptionsPanel"));
        advancedPanel_->setFrameShape(QFrame::StyledPanel);
        auto* advancedForm = new QFormLayout(advancedPanel_);
        connectTimeoutSpin_ = new QSpinBox(advancedPanel_);
        connectTimeoutSpin_->setObjectName(QStringLiteral("providerConnectTimeoutSpin"));
        connectTimeoutSpin_->setRange(kMinimumTimeoutMs, kMaximumConnectTimeoutMs);
        connectTimeoutSpin_->setSuffix(tr(" ms"));
        connectTimeoutSpin_->setSingleStep(1'000);
        advancedForm->addRow(tr("连接超时"), connectTimeoutSpin_);
        requestTimeoutSpin_ = new QSpinBox(advancedPanel_);
        requestTimeoutSpin_->setObjectName(QStringLiteral("providerRequestTimeoutSpin"));
        requestTimeoutSpin_->setRange(kMinimumTimeoutMs, kMaximumRequestTimeoutMs);
        requestTimeoutSpin_->setSuffix(tr(" ms"));
        requestTimeoutSpin_->setSingleStep(1'000);
        advancedForm->addRow(tr("总请求超时"), requestTimeoutSpin_);
        proxyUrlEdit_ = new QLineEdit(advancedPanel_);
        proxyUrlEdit_->setObjectName(QStringLiteral("providerProxyUrlEdit"));
        proxyUrlEdit_->setPlaceholderText(QStringLiteral("http://127.0.0.1:8080"));
        advancedForm->addRow(tr("代理"), proxyUrlEdit_);
        customHeadersEdit_ = new QPlainTextEdit(advancedPanel_);
        customHeadersEdit_->setObjectName(QStringLiteral("providerCustomHeadersEdit"));
        customHeadersEdit_->setPlaceholderText(tr(
            "仅公开元数据：X-Client: SnapAsk；X-Region: global / test / "
            "southeast-asia"));
        customHeadersEdit_->setToolTip(tr(
            "只允许 X-Client/X-Client-Name: SnapAsk，以及 X-Region: "
            "global、test 或 southeast-asia。凭据请使用 API Key 输入框。"));
        customHeadersEdit_->setMaximumHeight(100);
        advancedForm->addRow(tr("自定义请求头"), customHeadersEdit_);
        advancedPanel_->hide();
        layout->addWidget(advancedPanel_);
        layout->addStretch(1);
        return page;
    }

    void populate()
    {
        nameEdit_->setText(initialDraft_.displayName);
        const int protocolIndex = protocolCombo_->findData(
            static_cast<int>(initialDraft_.protocol));
        protocolCombo_->setCurrentIndex(protocolIndex >= 0 ? protocolIndex : 0);
        baseUrlEdit_->setText(initialDraft_.baseUrl.toString(QUrl::FullyEncoded));

        QStringList models = normalizedModels(initialDraft_.availableModels);
        if (!initialDraft_.modelId.trimmed().isEmpty()) {
            models.prepend(initialDraft_.modelId.trimmed());
            models = normalizedModels(std::move(models));
        }
        modelCombo_->addItems(models);
        modelCombo_->setCurrentText(initialDraft_.modelId);
        imageCapabilityCheck_->setChecked(
            initialDraft_.capabilities.testFlag(ProviderImageInput));
        streamingCapabilityCheck_->setChecked(
            initialDraft_.capabilities.testFlag(ProviderStreaming));
        modelListCapabilityCheck_->setChecked(
            initialDraft_.capabilities.testFlag(ProviderModelList));

        connectTimeoutSpin_->setValue(std::clamp(
            initialDraft_.connectTimeoutMs,
            kMinimumTimeoutMs,
            kMaximumConnectTimeoutMs));
        requestTimeoutSpin_->setMinimum(connectTimeoutSpin_->value());
        requestTimeoutSpin_->setValue(std::clamp(
            initialDraft_.requestTimeoutMs,
            connectTimeoutSpin_->value(),
            kMaximumRequestTimeoutMs));
        proxyUrlEdit_->setText(initialDraft_.proxyUrl.toString(QUrl::FullyEncoded));
        customHeadersEdit_->setPlainText(headersForEditor(initialDraft_.customHeaders));

        if (hasStoredApiKey_) {
            apiKeyEdit_->setEnabled(false);
            apiKeyEdit_->setPlaceholderText(tr("已安全保存；不会回填到界面"));
            replaceKeyCheck_->setChecked(false);
            replaceKeyCheck_->setEnabled(true);
        } else {
            apiKeyEdit_->setEnabled(true);
            apiKeyEdit_->setPlaceholderText(tr("输入 API Key"));
            replaceKeyCheck_->setChecked(true);
            replaceKeyCheck_->setEnabled(false);
            replaceKeyCheck_->setText(tr("保存到 Windows 凭据管理器"));
        }
    }

    void updateNavigation()
    {
        const int index = pages_->currentIndex();
        stepLabel_->setText(
            tr("第 %1 / %2 步 · %3")
                .arg(index + 1)
                .arg(pages_->count())
                .arg(index == 0 ? tr("服务")
                                : (index == 1 ? tr("模型") : tr("测试与确认"))));
        backButton_->setEnabled(index > 0);
        nextButton_->setVisible(index < pages_->count() - 1);
        submitButton_->setVisible(index == pages_->count() - 1);
    }

    void completeOperationControls()
    {
        operationPending_ = false;
        pendingOperationId_ = {};
        fetchModelsButton_->setEnabled(true);
        textTestButton_->setEnabled(true);
        imageTestButton_->setEnabled(true);
    }

    ProviderProfileDraft initialDraft_;
    bool editing_{false};
    bool hasStoredApiKey_{false};
    bool operationPending_{false};
    QUuid pendingOperationId_;
    ProviderUiOperation pendingOperation_{ProviderUiOperation::FetchModels};

    QLabel* stepLabel_{nullptr};
    QStackedWidget* pages_{nullptr};
    QLabel* validationLabel_{nullptr};
    QPushButton* cancelButton_{nullptr};
    QPushButton* backButton_{nullptr};
    QPushButton* nextButton_{nullptr};
    QPushButton* submitButton_{nullptr};

    QLineEdit* nameEdit_{nullptr};
    QComboBox* protocolCombo_{nullptr};
    QLineEdit* baseUrlEdit_{nullptr};
    QLineEdit* apiKeyEdit_{nullptr};
    QCheckBox* replaceKeyCheck_{nullptr};

    QComboBox* modelCombo_{nullptr};
    QPushButton* fetchModelsButton_{nullptr};
    QLabel* modelStatusLabel_{nullptr};
    QCheckBox* imageCapabilityCheck_{nullptr};
    QCheckBox* streamingCapabilityCheck_{nullptr};
    QCheckBox* modelListCapabilityCheck_{nullptr};

    QPushButton* textTestButton_{nullptr};
    QPushButton* imageTestButton_{nullptr};
    QLabel* testStatusLabel_{nullptr};
    QToolButton* advancedToggle_{nullptr};
    QFrame* advancedPanel_{nullptr};
    QSpinBox* connectTimeoutSpin_{nullptr};
    QSpinBox* requestTimeoutSpin_{nullptr};
    QLineEdit* proxyUrlEdit_{nullptr};
    QPlainTextEdit* customHeadersEdit_{nullptr};
};

ProviderSettingsWidget::ProviderSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ProviderSettingsWidget"));
    buildUi();
    rebuildCards();
}

ProviderSettingsWidget::~ProviderSettingsWidget()
{
    if (wizard_ != nullptr) {
        wizard_->scrubCredential();
    }
}

void ProviderSettingsWidget::setProfiles(
    QList<ProviderProfileSummary> profiles,
    const QUuid& defaultProfileId)
{
    profiles.erase(
        std::remove_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.profileId.isNull();
        }),
        profiles.end());
    profiles_ = std::move(profiles);
    defaultProfileId_ = defaultProfileId;
    if (findProfile(defaultProfileId_) == nullptr) {
        defaultProfileId_ = profiles_.isEmpty() ? QUuid{} : profiles_.first().profileId;
    }
    rebuildCards();
}

QList<ProviderProfileSummary> ProviderSettingsWidget::profiles() const
{
    return profiles_;
}

QUuid ProviderSettingsWidget::defaultProfileId() const noexcept
{
    return defaultProfileId_;
}

void ProviderSettingsWidget::applyModelListResult(
    const QUuid& operationId,
    const QUuid& profileId,
    const bool succeeded,
    QStringList models,
    const QString& message)
{
    if (wizard_ == nullptr
        || !wizard_->matchesOperation(
            operationId, profileId, ProviderUiOperation::FetchModels)) {
        return;
    }
    wizard_->completeModelList(succeeded, std::move(models), message);
}

void ProviderSettingsWidget::applyTestResult(
    const QUuid& operationId,
    const QUuid& profileId,
    const ProviderUiOperation operation,
    const bool succeeded,
    const QString& message)
{
    if (operation == ProviderUiOperation::FetchModels || wizard_ == nullptr
        || !wizard_->matchesOperation(operationId, profileId, operation)) {
        return;
    }
    wizard_->completeTest(succeeded, message);
}

bool ProviderSettingsWidget::confirmImageUnderstandingTest(QWidget* parent)
{
    return QMessageBox::question(
               parent,
               tr("测试图片理解？"),
               tr("将发送 SnapAsk 内置的无敏感测试图片，不会使用当前截图。"
                  "此操作可能产生少量 API 费用。是否继续？"),
               QMessageBox::Yes | QMessageBox::Cancel,
               QMessageBox::Cancel)
        == QMessageBox::Yes;
}

bool ProviderSettingsWidget::confirmDeleteProfile(
    const ProviderProfileSummary& profile,
    bool* deleteStoredCredential,
    QWidget* parent)
{
    QMessageBox box(
        QMessageBox::Question,
        tr("删除 AI 服务？"),
        tr("将删除服务“%1”的普通配置。").arg(profile.displayName),
        QMessageBox::Yes | QMessageBox::Cancel,
        parent);
    box.setDefaultButton(QMessageBox::Cancel);
    auto* credentialCheck = new QCheckBox(
        tr("同时删除 Windows 凭据管理器中的 API Key"), &box);
    credentialCheck->setChecked(profile.hasStoredApiKey);
    credentialCheck->setEnabled(profile.hasStoredApiKey);
    box.setCheckBox(credentialCheck);
    if (box.exec() != QMessageBox::Yes) {
        return false;
    }
    if (deleteStoredCredential != nullptr) {
        *deleteStoredCredential = credentialCheck->isChecked();
    }
    return true;
}

void ProviderSettingsWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(tr("AI 服务"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    header->addWidget(title);
    header->addStretch(1);
    auto* exportButton = new QPushButton(tr("导出普通配置"), this);
    exportButton->setObjectName(QStringLiteral("exportProviderConfigurationButton"));
    auto* addButton = new QPushButton(tr("添加服务"), this);
    addButton->setObjectName(QStringLiteral("addProviderButton"));
    header->addWidget(exportButton);
    header->addWidget(addButton);
    root->addLayout(header);

    auto* explanation = new QLabel(
        tr("每个服务独立保存协议、地址和模型；API Key 仅由 Windows 凭据管理器保存。"),
        this);
    explanation->setWordWrap(true);
    root->addWidget(explanation);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("providerCardsScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    cardsContainer_ = new QWidget(scrollArea);
    cardsContainer_->setObjectName(QStringLiteral("providerCardsContainer"));
    cardsLayout_ = new QVBoxLayout(cardsContainer_);
    cardsLayout_->setContentsMargins(0, 0, 4, 0);
    cardsLayout_->setSpacing(10);
    cardsLayout_->setAlignment(Qt::AlignTop);
    scrollArea->setWidget(cardsContainer_);
    root->addWidget(scrollArea, 1);

    connect(addButton, &QPushButton::clicked,
            this, &ProviderSettingsWidget::openAddWizard);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        emit exportRequested(defaultProfileId_);
    });
}

void ProviderSettingsWidget::rebuildCards()
{
    while (QLayoutItem* item = cardsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    if (profiles_.isEmpty()) {
        auto* empty = new QLabel(
            tr("尚未配置 AI 服务。使用“添加服务”向导开始。"),
            cardsContainer_);
        empty->setObjectName(QStringLiteral("providerEmptyState"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        cardsLayout_->addWidget(empty);
        cardsLayout_->addStretch(1);
        return;
    }

    for (const ProviderProfileSummary& profile : std::as_const(profiles_)) {
        auto* card = new QFrame(cardsContainer_);
        card->setObjectName(QStringLiteral("providerCard"));
        card->setProperty(
            "profileId", profile.profileId.toString(QUuid::WithoutBraces));
        card->setFrameShape(QFrame::StyledPanel);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(7);

        auto* titleRow = new QHBoxLayout();
        auto* name = new QLabel(profile.displayName, card);
        name->setObjectName(QStringLiteral("providerCardName"));
        name->setTextFormat(Qt::PlainText);
        QFont nameFont = name->font();
        nameFont.setWeight(QFont::DemiBold);
        name->setFont(nameFont);
        titleRow->addWidget(name, 1);
        if (profile.profileId == defaultProfileId_) {
            auto* defaultBadge = createBadge(tr("默认"), card);
            defaultBadge->setObjectName(QStringLiteral("providerDefaultBadge"));
            titleRow->addWidget(defaultBadge);
        }
        cardLayout->addLayout(titleRow);

        auto* details = new QLabel(
            tr("%1 · %2\n模型：%3")
                .arg(protocolLabel(profile.protocol),
                     canonicalHostLabel(profile.baseUrl),
                     profile.modelId.isEmpty() ? tr("未选择") : profile.modelId),
            card);
        details->setObjectName(QStringLiteral("providerCardDetails"));
        details->setTextFormat(Qt::PlainText);
        details->setWordWrap(true);
        cardLayout->addWidget(details);

        auto* badgeRow = new QHBoxLayout();
        if (profile.capabilities.testFlag(ProviderImageInput)) {
            badgeRow->addWidget(createBadge(tr("图片"), card));
        }
        if (profile.capabilities.testFlag(ProviderStreaming)) {
            badgeRow->addWidget(createBadge(tr("流式"), card));
        }
        if (profile.capabilities.testFlag(ProviderModelList)) {
            badgeRow->addWidget(createBadge(tr("模型列表"), card));
        }
        badgeRow->addStretch(1);
        cardLayout->addLayout(badgeRow);

        QString testText = tr("尚未测试");
        if (profile.lastTestedAt.isValid()) {
            const QString status = profile.lastTestStatus.trimmed().isEmpty()
                ? tr("已测试") : boundedStatus(profile.lastTestStatus);
            testText = tr("%1 · %2")
                           .arg(status,
                                profile.lastTestedAt.toLocalTime().toString(
                                    QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        auto* testStatus = new QLabel(testText, card);
        testStatus->setObjectName(QStringLiteral("providerCardTestStatus"));
        testStatus->setTextFormat(Qt::PlainText);
        cardLayout->addWidget(testStatus);

        auto* actions = new QHBoxLayout();
        auto addAction = [card, &actions, &profile](const QString& text,
                                                    const QString& objectName) {
            auto* button = new QPushButton(text, card);
            button->setObjectName(objectName);
            button->setProperty(
                "profileId", profile.profileId.toString(QUuid::WithoutBraces));
            actions->addWidget(button);
            return button;
        };
        auto* editButton = addAction(tr("编辑"), QStringLiteral("editProviderButton"));
        auto* duplicateButton = addAction(
            tr("复制"), QStringLiteral("duplicateProviderButton"));
        auto* defaultButton = addAction(
            tr("设为默认"), QStringLiteral("setDefaultProviderButton"));
        auto* deleteButton = addAction(
            tr("删除"), QStringLiteral("deleteProviderButton"));
        defaultButton->setEnabled(profile.profileId != defaultProfileId_);
        actions->addStretch(1);
        cardLayout->addLayout(actions);

        const QUuid profileId = profile.profileId;
        connect(editButton, &QPushButton::clicked, this, [this, profileId] {
            openEditWizard(profileId);
        });
        connect(duplicateButton, &QPushButton::clicked, this, [this, profileId] {
            emit duplicateRequested(profileId);
        });
        connect(defaultButton, &QPushButton::clicked, this, [this, profileId] {
            emit setDefaultRequested(profileId);
        });
        connect(deleteButton, &QPushButton::clicked, this, [this, profileId] {
            requestDelete(profileId);
        });
        cardsLayout_->addWidget(card);
    }
    cardsLayout_->addStretch(1);
}

void ProviderSettingsWidget::openAddWizard()
{
    ProviderProfileDraft draft;
    draft.profileId = QUuid::createUuid();
    draft.displayName = tr("新 AI 服务");
    draft.protocol = ProviderUiProtocol::OpenAIResponses;
    draft.baseUrl = QUrl(QStringLiteral("https://api.openai.com/v1"));
    openWizard(draft, false, false);
}

void ProviderSettingsWidget::openEditWizard(const QUuid& profileId)
{
    const ProviderProfileSummary* profile = findProfile(profileId);
    if (profile == nullptr) {
        return;
    }
    ProviderProfileDraft draft;
    draft.profileId = profile->profileId;
    draft.displayName = profile->displayName;
    draft.protocol = profile->protocol;
    draft.baseUrl = profile->baseUrl;
    draft.modelId = profile->modelId;
    draft.availableModels = profile->availableModels;
    draft.capabilities = profile->capabilities;
    draft.connectTimeoutMs = profile->connectTimeoutMs;
    draft.requestTimeoutMs = profile->requestTimeoutMs;
    draft.proxyUrl = profile->proxyUrl;
    draft.customHeaders = profile->customHeaders;
    openWizard(draft, true, profile->hasStoredApiKey);
}

void ProviderSettingsWidget::openWizard(
    const ProviderProfileDraft& draft,
    const bool editing,
    const bool hasStoredApiKey)
{
    closeWizard();
    wizard_ = new ProviderWizardDialog(draft, editing, hasStoredApiKey, this);
    ProviderWizardDialog* openedWizard = wizard_;
    connect(openedWizard, &QDialog::finished, this, [this, openedWizard] {
        openedWizard->scrubCredential();
        if (wizard_ == openedWizard) {
            wizard_ = nullptr;
        }
        openedWizard->deleteLater();
    });
    connect(openedWizard->submitButton(), &QPushButton::clicked,
            this, &ProviderSettingsWidget::submitWizard);
    connect(openedWizard->fetchModelsButton(), &QPushButton::clicked,
            this, [this] {
        requestWizardOperation(ProviderUiOperation::FetchModels);
    });
    connect(openedWizard->textTestButton(), &QPushButton::clicked,
            this, [this] {
        requestWizardOperation(ProviderUiOperation::TestTextConnection);
    });
    connect(openedWizard->imageTestButton(), &QPushButton::clicked,
            this, [this] {
        requestWizardOperation(ProviderUiOperation::TestImageUnderstanding);
    });
    openedWizard->open();
}

void ProviderSettingsWidget::closeWizard()
{
    if (wizard_ == nullptr) {
        return;
    }
    ProviderWizardDialog* closing = wizard_;
    wizard_ = nullptr;
    closing->scrubCredential();
    closing->reject();
}

void ProviderSettingsWidget::submitWizard()
{
    if (wizard_ == nullptr) {
        return;
    }
    QString error;
    const ProviderProfileDraft draft = wizard_->draft(true, &error);
    if (draft.profileId.isNull()) {
        wizard_->showValidationError(error);
        return;
    }
    bool useStoredCredential = false;
    bool replaceStoredKey = false;
    QString apiKey = wizard_->transientApiKey(
        &useStoredCredential, &replaceStoredKey, &error);
    if (apiKey.isEmpty() && !useStoredCredential) {
        wizard_->showValidationError(error);
        return;
    }

    ProviderWizardDialog* submitted = wizard_;
    if (submitted->editing()) {
        emit editRequested(
            draft.profileId, draft, apiKey, replaceStoredKey);
    } else {
        emit addRequested(draft.profileId, draft, apiKey);
    }
    scrubSensitiveString(apiKey);
    submitted->scrubCredential();
    submitted->accept();
}

void ProviderSettingsWidget::requestWizardOperation(
    const ProviderUiOperation operation)
{
    if (wizard_ == nullptr) {
        return;
    }
    QString error;
    const bool requireModel = operation != ProviderUiOperation::FetchModels;
    const ProviderProfileDraft draft = wizard_->draft(requireModel, &error);
    if (draft.profileId.isNull()) {
        wizard_->showValidationError(error);
        return;
    }
    bool useStoredCredential = false;
    bool replaceStoredKey = false;
    QString apiKey = wizard_->transientApiKey(
        &useStoredCredential, &replaceStoredKey, &error);
    Q_UNUSED(replaceStoredKey)
    if (apiKey.isEmpty() && !useStoredCredential) {
        wizard_->showValidationError(error);
        return;
    }
    if (operation == ProviderUiOperation::TestImageUnderstanding
        && !confirmImageUnderstandingTest(wizard_)) {
        scrubSensitiveString(apiKey);
        return;
    }

    const QUuid operationId = QUuid::createUuid();
    wizard_->beginOperation(operationId, operation);
    switch (operation) {
    case ProviderUiOperation::FetchModels:
        emit fetchModelsRequested(
            operationId, draft.profileId, draft, apiKey, useStoredCredential);
        break;
    case ProviderUiOperation::TestTextConnection:
        emit testTextRequested(
            operationId, draft.profileId, draft, apiKey, useStoredCredential);
        break;
    case ProviderUiOperation::TestImageUnderstanding:
        emit testImageRequested(
            operationId, draft.profileId, draft, apiKey, useStoredCredential);
        break;
    }
    scrubSensitiveString(apiKey);
}

void ProviderSettingsWidget::requestDelete(const QUuid& profileId)
{
    const ProviderProfileSummary* profile = findProfile(profileId);
    if (profile == nullptr) {
        return;
    }
    bool deleteStoredCredential = profile->hasStoredApiKey;
    if (!confirmDeleteProfile(
            *profile, &deleteStoredCredential, this)) {
        return;
    }
    emit deleteRequested(profileId, deleteStoredCredential);
}

const ProviderProfileSummary* ProviderSettingsWidget::findProfile(
    const QUuid& profileId) const
{
    for (const ProviderProfileSummary& profile : profiles_) {
        if (profile.profileId == profileId) {
            return &profile;
        }
    }
    return nullptr;
}

} // namespace snapask::ui
