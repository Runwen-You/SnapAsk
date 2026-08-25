#include "ui/settings/ProviderSettingsWidget.h"
#include "ui/settings/SettingsDialog.h"
#include "ui/common/ThemeTokens.h"

#include "SnapAskVersion.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>

using namespace snapask::ui;

namespace {

class TestProviderSettingsWidget final : public ProviderSettingsWidget {
public:
    using ProviderSettingsWidget::ProviderSettingsWidget;

    bool imageTestDecision{false};
    int imageConfirmationCount{0};
    bool deleteDecision{true};
    bool deleteCredentialDecision{true};
    int deleteConfirmationCount{0};

protected:
    bool confirmImageUnderstandingTest(QWidget*) override
    {
        ++imageConfirmationCount;
        return imageTestDecision;
    }

    bool confirmDeleteProfile(const ProviderProfileSummary&,
                              bool* deleteStoredCredential,
                              QWidget*) override
    {
        ++deleteConfirmationCount;
        if (deleteStoredCredential != nullptr) {
            *deleteStoredCredential = deleteCredentialDecision;
        }
        return deleteDecision;
    }
};

ProviderProfileSummary profile(
    const QString& name,
    const ProviderUiProtocol protocol,
    const QString& host,
    const QString& model,
    const ProviderUiCapabilities capabilities,
    const bool hasKey = true)
{
    ProviderProfileSummary result;
    result.profileId = QUuid::createUuid();
    result.displayName = name;
    result.protocol = protocol;
    result.baseUrl = QUrl(QStringLiteral("https://%1/v1").arg(host));
    result.modelId = model;
    result.availableModels = {model, model + QStringLiteral("-large")};
    result.capabilities = capabilities;
    result.lastTestedAt = QDateTime::currentDateTimeUtc();
    result.lastTestStatus = QStringLiteral("图片测试通过");
    result.hasStoredApiKey = hasKey;
    return result;
}

template <typename Widget>
Widget* requiredChild(QObject& parent, const char* objectName)
{
    Widget* child = parent.findChild<Widget*>(QString::fromLatin1(objectName));
    if (child == nullptr) {
        qFatal("Required child was not found: %s", objectName);
    }
    return child;
}

QDialog* currentWizard(QObject& parent)
{
    const auto dialogs = parent.findChildren<QDialog*>(
        QStringLiteral("providerWizardDialog"));
    for (QDialog* dialog : dialogs) {
        if (dialog->isVisible()) {
            return dialog;
        }
    }
    return dialogs.isEmpty() ? nullptr : dialogs.last();
}

QPushButton* profileButton(
    QObject& parent,
    const char* objectName,
    const QUuid& profileId)
{
    const QString expected = profileId.toString(QUuid::WithoutBraces);
    const auto buttons = parent.findChildren<QPushButton*>(
        QString::fromLatin1(objectName));
    for (QPushButton* button : buttons) {
        if (button->property("profileId").toString() == expected) {
            return button;
        }
    }
    return nullptr;
}

void showWidget(ProviderSettingsWidget& widget)
{
    widget.resize(760, 700);
    widget.show();
    QTest::qWait(20);
}

QDialog* openAddWizard(ProviderSettingsWidget& widget)
{
    QPushButton* addButton =
        requiredChild<QPushButton>(widget, "addProviderButton");
    QTest::mouseClick(addButton, Qt::LeftButton);
    QTest::qWait(10);
    return currentWizard(widget);
}

void fillBasicPage(
    QDialog& wizard,
    const QString& name = QStringLiteral("兼容服务"),
    const QUrl& baseUrl = QUrl(QStringLiteral("https://compatible.example/v1")),
    const QString& apiKey = QStringLiteral("m4-test-secret-key"),
    const ProviderUiProtocol protocol = ProviderUiProtocol::ChatCompletions)
{
    requiredChild<QLineEdit>(wizard, "providerNameEdit")->setText(name);
    requiredChild<QLineEdit>(wizard, "providerBaseUrlEdit")
        ->setText(baseUrl.toString(QUrl::FullyEncoded));
    QComboBox* protocolCombo =
        requiredChild<QComboBox>(wizard, "providerProtocolCombo");
    const int index = protocolCombo->findData(static_cast<int>(protocol));
    QVERIFY(index >= 0);
    protocolCombo->setCurrentIndex(index);
    QLineEdit* keyEdit = requiredChild<QLineEdit>(wizard, "providerApiKeyEdit");
    if (keyEdit->isEnabled()) {
        keyEdit->setText(apiKey);
    }
}

void goToModelsPage(QDialog& wizard)
{
    QPushButton* next = requiredChild<QPushButton>(wizard, "wizardNextButton");
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(requiredChild<QStackedWidget>(wizard, "providerWizardPages")
                 ->currentIndex(),
             1);
}

void goToFinalPage(QDialog& wizard, const QString& model)
{
    QComboBox* combo = requiredChild<QComboBox>(wizard, "providerModelCombo");
    combo->setEditText(model);
    QPushButton* next = requiredChild<QPushButton>(wizard, "wizardNextButton");
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(requiredChild<QStackedWidget>(wizard, "providerWizardPages")
                 ->currentIndex(),
             2);
}

} // namespace

class M4ProviderSettingsWidgetTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cardsShowThreeProfilesAndEveryActionCarriesProfileId();
    void addWizardIsProgressiveAndEmitsNonSecretDraft();
    void editNeverBackfillsStoredKeyAndCanExplicitlyReplaceIt();
    void modelListFailureEnablesManualFallbackAndStaleResultsAreIgnored();
    void imageTestRequiresConfirmationAndTestResultsAreCorrelated();
    void advancedValidationRejectsUnsafeUrlsAndSecretHeaders();
    void cancellingWizardScrubsCredentialEditor();
    void settingsUseSidebarAndSharedGlassTokens();
};

void M4ProviderSettingsWidgetTests::initTestCase()
{
    qRegisterMetaType<ProviderUiProtocol>();
    qRegisterMetaType<ProviderUiCapabilities>();
    qRegisterMetaType<ProviderUiOperation>();
    qRegisterMetaType<ProviderProfileSummary>();
    qRegisterMetaType<ProviderProfileDraft>();
}

void M4ProviderSettingsWidgetTests::cardsShowThreeProfilesAndEveryActionCarriesProfileId()
{
    TestProviderSettingsWidget widget;
    const ProviderProfileSummary first = profile(
        QStringLiteral("OpenAI 主服务"), ProviderUiProtocol::OpenAIResponses,
        QStringLiteral("api.openai.com"), QStringLiteral("gpt-main"),
        ProviderUiCapabilities(ProviderImageInput | ProviderStreaming
                               | ProviderModelList));
    const ProviderProfileSummary second = profile(
        QStringLiteral("团队兼容服务"), ProviderUiProtocol::ChatCompletions,
        QStringLiteral("team.example"), QStringLiteral("vision-a"),
        ProviderUiCapabilities(ProviderImageInput | ProviderStreaming));
    const ProviderProfileSummary third = profile(
        QStringLiteral("备用服务"), ProviderUiProtocol::OpenAIResponses,
        QStringLiteral("backup.example"), QStringLiteral("vision-b"),
        ProviderUiCapabilities(ProviderImageInput | ProviderStreaming
                               | ProviderModelList));
    widget.setProfiles({first, second, third}, first.profileId);
    showWidget(widget);

    QCOMPARE(widget.findChildren<QFrame*>(QStringLiteral("providerCard")).size(), 3);
    QCOMPARE(widget.findChildren<QLabel*>(QStringLiteral("providerCardName")).size(), 3);
    QCOMPARE(widget.findChildren<QLabel*>(QStringLiteral("providerDefaultBadge")).size(), 1);
    QVERIFY(widget.findChildren<QLabel*>(QStringLiteral("providerCapabilityBadge")).size()
            >= 7);
    QCOMPARE(widget.defaultProfileId(), first.profileId);

    QSignalSpy duplicateSpy(&widget, &ProviderSettingsWidget::duplicateRequested);
    QSignalSpy defaultSpy(&widget, &ProviderSettingsWidget::setDefaultRequested);
    QSignalSpy deleteSpy(&widget, &ProviderSettingsWidget::deleteRequested);
    QSignalSpy exportSpy(&widget, &ProviderSettingsWidget::exportRequested);

    QPushButton* duplicate = profileButton(
        widget, "duplicateProviderButton", second.profileId);
    QPushButton* makeDefault = profileButton(
        widget, "setDefaultProviderButton", second.profileId);
    QPushButton* remove = profileButton(
        widget, "deleteProviderButton", third.profileId);
    QVERIFY(duplicate != nullptr);
    QVERIFY(makeDefault != nullptr);
    QVERIFY(remove != nullptr);
    QTest::mouseClick(duplicate, Qt::LeftButton);
    QTest::mouseClick(makeDefault, Qt::LeftButton);
    widget.deleteCredentialDecision = false;
    QTest::mouseClick(remove, Qt::LeftButton);
    QTest::mouseClick(
        requiredChild<QPushButton>(widget, "exportProviderConfigurationButton"),
        Qt::LeftButton);

    QCOMPARE(duplicateSpy.size(), 1);
    QCOMPARE(duplicateSpy.at(0).at(0).toUuid(), second.profileId);
    QCOMPARE(defaultSpy.size(), 1);
    QCOMPARE(defaultSpy.at(0).at(0).toUuid(), second.profileId);
    QCOMPARE(deleteSpy.size(), 1);
    QCOMPARE(deleteSpy.at(0).at(0).toUuid(), third.profileId);
    QCOMPARE(deleteSpy.at(0).at(1).toBool(), false);
    QCOMPARE(widget.deleteConfirmationCount, 1);
    QCOMPARE(exportSpy.size(), 1);
    QCOMPARE(exportSpy.at(0).at(0).toUuid(), first.profileId);
}

void M4ProviderSettingsWidgetTests::addWizardIsProgressiveAndEmitsNonSecretDraft()
{
    TestProviderSettingsWidget widget;
    showWidget(widget);
    QSignalSpy addSpy(&widget, &ProviderSettingsWidget::addRequested);

    QDialog* wizard = openAddWizard(widget);
    QVERIFY(wizard != nullptr);
    QStackedWidget* pages =
        requiredChild<QStackedWidget>(*wizard, "providerWizardPages");
    QCOMPARE(pages->count(), 3);
    QCOMPARE(pages->currentIndex(), 0);
    QCOMPARE(requiredChild<QComboBox>(*wizard, "providerProtocolCombo")->count(), 2);

    fillBasicPage(*wizard);
    QPointer<QLineEdit> sensitiveEditor =
        requiredChild<QLineEdit>(*wizard, "providerApiKeyEdit");
    goToModelsPage(*wizard);
    goToFinalPage(*wizard, QStringLiteral("vision-manual"));

    QFrame* advanced = requiredChild<QFrame>(*wizard, "advancedOptionsPanel");
    QVERIFY(!advanced->isVisible());
    QToolButton* toggle =
        requiredChild<QToolButton>(*wizard, "advancedOptionsToggle");
    QTest::mouseClick(toggle, Qt::LeftButton);
    QVERIFY(advanced->isVisible());
    requiredChild<QSpinBox>(*wizard, "providerConnectTimeoutSpin")->setValue(3'000);
    requiredChild<QSpinBox>(*wizard, "providerRequestTimeoutSpin")->setValue(9'000);
    requiredChild<QLineEdit>(*wizard, "providerProxyUrlEdit")
        ->setText(QStringLiteral("https://proxy.example:8443"));
    QPlainTextEdit* customHeaders =
        requiredChild<QPlainTextEdit>(*wizard, "providerCustomHeadersEdit");
    QVERIFY(customHeaders->placeholderText().contains(QStringLiteral("公开")));
    QVERIFY(customHeaders->placeholderText().contains(QStringLiteral("test")));
    QVERIFY(customHeaders->placeholderText().contains(
        QStringLiteral("southeast-asia")));
    customHeaders->setPlainText(
        QStringLiteral("X-Client-Name: SnapAsk\nX-Region: test"));

    QTest::mouseClick(
        requiredChild<QPushButton>(*wizard, "wizardSubmitButton"),
        Qt::LeftButton);
    QCOMPARE(addSpy.size(), 1);
    const QUuid profileId = addSpy.at(0).at(0).toUuid();
    QVERIFY(!profileId.isNull());
    const ProviderProfileDraft draft =
        qvariant_cast<ProviderProfileDraft>(addSpy.at(0).at(1));
    QCOMPARE(draft.profileId, profileId);
    QCOMPARE(draft.displayName, QStringLiteral("兼容服务"));
    QCOMPARE(draft.protocol, ProviderUiProtocol::ChatCompletions);
    QCOMPARE(draft.baseUrl, QUrl(QStringLiteral("https://compatible.example/v1")));
    QCOMPARE(draft.modelId, QStringLiteral("vision-manual"));
    QCOMPARE(draft.connectTimeoutMs, 3'000);
    QCOMPARE(draft.requestTimeoutMs, 9'000);
    QCOMPARE(draft.proxyUrl, QUrl(QStringLiteral("https://proxy.example:8443")));
    QCOMPARE(draft.customHeaders.value(QStringLiteral("X-Region")).toString(),
             QStringLiteral("test"));
    QCOMPARE(addSpy.at(0).at(2).toString(), QStringLiteral("m4-test-secret-key"));
    QVERIFY(!sensitiveEditor.isNull());
    QCOMPARE(sensitiveEditor->text(), QString());
}

void M4ProviderSettingsWidgetTests::editNeverBackfillsStoredKeyAndCanExplicitlyReplaceIt()
{
    TestProviderSettingsWidget widget;
    const ProviderProfileSummary existing = profile(
        QStringLiteral("已保存服务"), ProviderUiProtocol::OpenAIResponses,
        QStringLiteral("api.example"), QStringLiteral("model-a"),
        ProviderUiCapabilities(ProviderImageInput | ProviderStreaming
                               | ProviderModelList),
        true);
    widget.setProfiles({existing}, existing.profileId);
    showWidget(widget);
    QSignalSpy editSpy(&widget, &ProviderSettingsWidget::editRequested);

    QPushButton* edit = profileButton(
        widget, "editProviderButton", existing.profileId);
    QVERIFY(edit != nullptr);
    QTest::mouseClick(edit, Qt::LeftButton);
    QDialog* wizard = currentWizard(widget);
    QVERIFY(wizard != nullptr);
    QLineEdit* keyEdit = requiredChild<QLineEdit>(*wizard, "providerApiKeyEdit");
    QCheckBox* replace =
        requiredChild<QCheckBox>(*wizard, "replaceStoredKeyCheck");
    QVERIFY(!keyEdit->isEnabled());
    QVERIFY(keyEdit->text().isEmpty());
    QVERIFY(keyEdit->placeholderText().contains(QStringLiteral("不会回填")));
    QVERIFY(!replace->isChecked());
    goToModelsPage(*wizard);
    goToFinalPage(*wizard, QStringLiteral("model-a"));
    QTest::mouseClick(
        requiredChild<QPushButton>(*wizard, "wizardSubmitButton"),
        Qt::LeftButton);

    QCOMPARE(editSpy.size(), 1);
    QCOMPARE(editSpy.at(0).at(0).toUuid(), existing.profileId);
    QCOMPARE(editSpy.at(0).at(2).toString(), QString());
    QCOMPARE(editSpy.at(0).at(3).toBool(), false);

    QCoreApplication::processEvents();
    edit = profileButton(widget, "editProviderButton", existing.profileId);
    QTest::mouseClick(edit, Qt::LeftButton);
    wizard = currentWizard(widget);
    QVERIFY(wizard != nullptr);
    keyEdit = requiredChild<QLineEdit>(*wizard, "providerApiKeyEdit");
    replace = requiredChild<QCheckBox>(*wizard, "replaceStoredKeyCheck");
    replace->setChecked(true);
    QVERIFY(keyEdit->isEnabled());
    keyEdit->setText(QStringLiteral("replacement-secret"));
    QPointer<QLineEdit> guardedKeyEdit(keyEdit);
    goToModelsPage(*wizard);
    goToFinalPage(*wizard, QStringLiteral("model-a"));
    QTest::mouseClick(
        requiredChild<QPushButton>(*wizard, "wizardSubmitButton"),
        Qt::LeftButton);

    QCOMPARE(editSpy.size(), 2);
    QCOMPARE(editSpy.at(1).at(0).toUuid(), existing.profileId);
    QCOMPARE(editSpy.at(1).at(2).toString(), QStringLiteral("replacement-secret"));
    QCOMPARE(editSpy.at(1).at(3).toBool(), true);
    QVERIFY(!guardedKeyEdit.isNull());
    QCOMPARE(guardedKeyEdit->text(), QString());
}

void M4ProviderSettingsWidgetTests::modelListFailureEnablesManualFallbackAndStaleResultsAreIgnored()
{
    TestProviderSettingsWidget widget;
    showWidget(widget);
    QSignalSpy fetchSpy(&widget, &ProviderSettingsWidget::fetchModelsRequested);

    QDialog* wizard = openAddWizard(widget);
    fillBasicPage(*wizard);
    goToModelsPage(*wizard);
    QPushButton* fetch = requiredChild<QPushButton>(*wizard, "fetchModelsButton");
    QComboBox* models = requiredChild<QComboBox>(*wizard, "providerModelCombo");
    QLabel* status = requiredChild<QLabel>(*wizard, "modelOperationStatus");
    QTest::mouseClick(fetch, Qt::LeftButton);
    QCOMPARE(fetchSpy.size(), 1);
    const QUuid firstOperation = fetchSpy.at(0).at(0).toUuid();
    const QUuid profileId = fetchSpy.at(0).at(1).toUuid();
    QVERIFY(!firstOperation.isNull());
    QVERIFY(!profileId.isNull());
    QVERIFY(!fetch->isEnabled());

    widget.applyModelListResult(
        QUuid::createUuid(), profileId, true,
        {QStringLiteral("stale-model")}, QStringLiteral("迟到"));
    QCOMPARE(models->findText(QStringLiteral("stale-model")), -1);
    QVERIFY(!fetch->isEnabled());
    widget.applyModelListResult(
        firstOperation, QUuid::createUuid(), true,
        {QStringLiteral("wrong-profile")}, QStringLiteral("错误档案"));
    QCOMPARE(models->findText(QStringLiteral("wrong-profile")), -1);

    widget.applyModelListResult(
        firstOperation, profileId, false, {},
        QStringLiteral("获取失败，请手动输入模型 ID"));
    QVERIFY(fetch->isEnabled());
    QVERIFY(models->isEditable());
    QVERIFY(status->text().contains(QStringLiteral("手动")));
    models->setEditText(QStringLiteral("manual-fallback"));
    QCOMPARE(models->currentText(), QStringLiteral("manual-fallback"));

    QTest::mouseClick(fetch, Qt::LeftButton);
    QCOMPARE(fetchSpy.size(), 2);
    const QUuid secondOperation = fetchSpy.at(1).at(0).toUuid();
    widget.applyModelListResult(
        secondOperation, profileId, true,
        {QStringLiteral("model-z"), QStringLiteral("model-a"),
         QStringLiteral("model-z"), QStringLiteral(" ")});
    QCOMPARE(models->count(), 2);
    QVERIFY(models->findText(QStringLiteral("model-a")) >= 0);
    QVERIFY(models->findText(QStringLiteral("model-z")) >= 0);

    widget.applyModelListResult(
        firstOperation, profileId, true,
        {QStringLiteral("late-after-completion")});
    QCOMPARE(models->findText(QStringLiteral("late-after-completion")), -1);
}

void M4ProviderSettingsWidgetTests::imageTestRequiresConfirmationAndTestResultsAreCorrelated()
{
    TestProviderSettingsWidget widget;
    showWidget(widget);
    QSignalSpy imageSpy(&widget, &ProviderSettingsWidget::testImageRequested);
    QSignalSpy textSpy(&widget, &ProviderSettingsWidget::testTextRequested);

    QDialog* wizard = openAddWizard(widget);
    fillBasicPage(*wizard);
    goToModelsPage(*wizard);
    goToFinalPage(*wizard, QStringLiteral("vision-test"));
    QPushButton* imageButton = requiredChild<QPushButton>(*wizard, "testImageButton");
    QPushButton* textButton = requiredChild<QPushButton>(*wizard, "testTextButton");
    QLabel* status = requiredChild<QLabel>(*wizard, "providerTestStatus");

    widget.imageTestDecision = false;
    QTest::mouseClick(imageButton, Qt::LeftButton);
    QCOMPARE(widget.imageConfirmationCount, 1);
    QCOMPARE(imageSpy.size(), 0);
    QVERIFY(imageButton->isEnabled());

    widget.imageTestDecision = true;
    QTest::mouseClick(imageButton, Qt::LeftButton);
    QCOMPARE(widget.imageConfirmationCount, 2);
    QCOMPARE(imageSpy.size(), 1);
    const QUuid imageOperation = imageSpy.at(0).at(0).toUuid();
    const QUuid profileId = imageSpy.at(0).at(1).toUuid();
    QVERIFY(!imageButton->isEnabled());
    QCOMPARE(imageSpy.at(0).at(4).toBool(), false);
    QCOMPARE(imageSpy.at(0).at(3).toString(), QStringLiteral("m4-test-secret-key"));

    widget.applyTestResult(
        QUuid::createUuid(), profileId,
        ProviderUiOperation::TestImageUnderstanding, true,
        QStringLiteral("不应显示"));
    QVERIFY(!imageButton->isEnabled());
    QVERIFY(!status->text().contains(QStringLiteral("不应显示")));
    widget.applyTestResult(
        imageOperation, profileId,
        ProviderUiOperation::TestImageUnderstanding, true,
        QStringLiteral("内置图片理解测试通过"));
    QVERIFY(imageButton->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("通过")));

    QTest::mouseClick(textButton, Qt::LeftButton);
    QCOMPARE(textSpy.size(), 1);
    const QUuid textOperation = textSpy.at(0).at(0).toUuid();
    widget.applyTestResult(
        textOperation, profileId,
        ProviderUiOperation::TestTextConnection, false,
        QStringLiteral("连接测试失败"));
    QVERIFY(status->text().contains(QStringLiteral("失败")));
}

void M4ProviderSettingsWidgetTests::advancedValidationRejectsUnsafeUrlsAndSecretHeaders()
{
    TestProviderSettingsWidget widget;
    showWidget(widget);
    QSignalSpy addSpy(&widget, &ProviderSettingsWidget::addRequested);
    QDialog* wizard = openAddWizard(widget);
    fillBasicPage(
        *wizard, QStringLiteral("安全校验"),
        QUrl(QStringLiteral("http://public.example/v1")));
    QPushButton* next = requiredChild<QPushButton>(*wizard, "wizardNextButton");
    QLabel* validation = requiredChild<QLabel>(*wizard, "wizardValidationLabel");
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(requiredChild<QStackedWidget>(*wizard, "providerWizardPages")
                 ->currentIndex(),
             0);
    QVERIFY(validation->text().contains(QStringLiteral("HTTPS")));

    requiredChild<QLineEdit>(*wizard, "providerBaseUrlEdit")
        ->setText(QStringLiteral("https://safe.example/v1"));
    goToModelsPage(*wizard);
    goToFinalPage(*wizard, QStringLiteral("safe-model"));
    QTest::mouseClick(
        requiredChild<QToolButton>(*wizard, "advancedOptionsToggle"),
        Qt::LeftButton);
    QPlainTextEdit* headers =
        requiredChild<QPlainTextEdit>(*wizard, "providerCustomHeadersEdit");
    headers->setPlainText(QStringLiteral("Authorization: Bearer should-never-persist"));
    QPushButton* submit = requiredChild<QPushButton>(*wizard, "wizardSubmitButton");
    QTest::mouseClick(submit, Qt::LeftButton);
    QCOMPARE(addSpy.size(), 0);
    QVERIFY(validation->text().contains(QStringLiteral("密钥"))
            || validation->text().contains(QStringLiteral("鉴权")));

    headers->setPlainText(QStringLiteral(
        "X-Region: 0123456789abcdef0123456789abcdef"));
    QTest::mouseClick(submit, Qt::LeftButton);
    QCOMPARE(addSpy.size(), 0);
    QVERIFY(validation->text().contains(QStringLiteral("公开")));
    QVERIFY(!validation->text().contains(
        QStringLiteral("0123456789abcdef0123456789abcdef")));

    headers->setPlainText(QStringLiteral("X-Client-Name: SnapAsk"));
    QLineEdit* proxy = requiredChild<QLineEdit>(*wizard, "providerProxyUrlEdit");
    proxy->setText(QStringLiteral("https://user:password@proxy.example"));
    QTest::mouseClick(submit, Qt::LeftButton);
    QCOMPARE(addSpy.size(), 0);
    QVERIFY(validation->text().contains(QStringLiteral("代理")));

    proxy->setText(QStringLiteral("https://proxy.example:8443"));
    QTest::mouseClick(submit, Qt::LeftButton);
    QCOMPARE(addSpy.size(), 1);
    const ProviderProfileDraft draft =
        qvariant_cast<ProviderProfileDraft>(addSpy.at(0).at(1));
    QCOMPARE(draft.customHeaders.value(QStringLiteral("X-Client-Name")).toString(),
             QStringLiteral("SnapAsk"));
}

void M4ProviderSettingsWidgetTests::cancellingWizardScrubsCredentialEditor()
{
    TestProviderSettingsWidget widget;
    showWidget(widget);
    QSignalSpy addSpy(&widget, &ProviderSettingsWidget::addRequested);
    QDialog* wizard = openAddWizard(widget);
    QLineEdit* keyEdit = requiredChild<QLineEdit>(*wizard, "providerApiKeyEdit");
    keyEdit->setText(QStringLiteral("cancelled-secret-sentinel"));
    QPointer<QLineEdit> guardedEditor(keyEdit);

    QTest::mouseClick(
        requiredChild<QPushButton>(*wizard, "wizardCancelButton"),
        Qt::LeftButton);
    QVERIFY(!guardedEditor.isNull());
    QCOMPARE(guardedEditor->text(), QString());
    QCOMPARE(addSpy.size(), 0);
}

void M4ProviderSettingsWidgetTests::settingsUseSidebarAndSharedGlassTokens()
{
    SettingsDialog dialog;
    auto* sidebar = requiredChild<QListWidget>(dialog, "settingsSidebar");
    auto* pages = requiredChild<QStackedWidget>(dialog, "settingsPages");
    QCOMPARE(sidebar->count(), 3);
    QCOMPARE(pages->count(), 3);
    QCOMPARE(sidebar->item(0)->text(), QStringLiteral("通用"));
    QCOMPARE(sidebar->item(1)->text(), QStringLiteral("AI 服务"));
    QVERIFY(dialog.providerSettingsWidget() != nullptr);

    sidebar->setCurrentRow(1);
    QCOMPARE(pages->currentIndex(), 1);
    auto* theme = requiredChild<QComboBox>(dialog, "themeModeCombo");
    QCOMPARE(theme->count(), 3);

    bool versionShown = false;
    const auto labels = dialog.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        if (label->text().contains(QStringLiteral(SNAPASK_VERSION_STRING))) {
            versionShown = true;
            break;
        }
    }
    QVERIFY(versionShown);

    const ThemeTokenSet light = ThemeTokens::resolve(ThemeMode::Light);
    QVERIFY(light.window.alpha() < 255);
    QVERIFY(light.surface.alpha() < 255);
    const QString style = ThemeTokens::styleSheet(light);
    QVERIFY(style.contains(QStringLiteral("CaptureActionBar")));
    QVERIFY(style.contains(QStringLiteral("settingsSidebar")));
}

QTEST_MAIN(M4ProviderSettingsWidgetTests)
#include "M4ProviderSettingsWidgetTests.moc"
