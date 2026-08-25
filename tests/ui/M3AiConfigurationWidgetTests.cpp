#include "ui/settings/AiConfigurationWidget.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QtTest>

namespace snapask::ui {
namespace {

constexpr auto kSecretSentinel = "snapask-ui-secret-sentinel";

template <typename Widget>
Widget* child(QObject& parent, const char* objectName) {
    return parent.findChild<Widget*>(QString::fromLatin1(objectName));
}

void showForInteraction(QWidget& widget) {
    widget.show();
    QCoreApplication::processEvents();
}

void setRequiredFields(AiConfigurationWidget& widget,
                       bool hasStoredApiKey = false) {
    widget.setConfiguration(
        QStringLiteral("OpenAI"),
        QUrl(QStringLiteral("https://api.openai.com/v1")),
        QStringLiteral("gpt-image-model"), 15'000, 120'000,
        hasStoredApiKey);
}

class M3AiConfigurationWidgetTests final : public QObject {
    Q_OBJECT

private slots:
    void showAndEditHaveNoSaveSideEffect();
    void apiKeyIsPasswordAndStoredValueIsNeverFilled();
    void onlySaveClickEmitsExactlyOneRequest();
    void baseUrlPolicy_data();
    void baseUrlPolicy();
    void storedKeyCanBeKeptWithoutDisclosure();
    void submittedKeyAndUndoHistoryAreCleared_data();
    void submittedKeyAndUndoHistoryAreCleared();
    void timeoutControlsEnforceRepositoryBounds();
    void deleteButtonEmitsCredentialChoice();
};

void M3AiConfigurationWidgetTests::showAndEditHaveNoSaveSideEffect() {
    AiConfigurationWidget widget;
    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QSignalSpy deleteSpy(&widget,
                         &AiConfigurationWidget::deleteRequested);
    QVERIFY(saveSpy.isValid());
    QVERIFY(deleteSpy.isValid());

    showForInteraction(widget);
    QVERIFY(widget.isVisible());
    QCOMPARE(saveSpy.count(), 0);
    QCOMPARE(deleteSpy.count(), 0);

    setRequiredFields(widget);
    auto* nameEdit = child<QLineEdit>(widget, "serviceNameEdit");
    auto* baseUrlEdit = child<QLineEdit>(widget, "responsesBaseUrlEdit");
    auto* modelEdit = child<QLineEdit>(widget, "modelIdEdit");
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* connectTimeout =
        child<QSpinBox>(widget, "connectTimeoutSpin");
    QVERIFY(nameEdit != nullptr);
    QVERIFY(baseUrlEdit != nullptr);
    QVERIFY(modelEdit != nullptr);
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(connectTimeout != nullptr);

    nameEdit->setText(QStringLiteral("Edited service"));
    baseUrlEdit->setText(QStringLiteral("https://example.test/v1"));
    modelEdit->setText(QStringLiteral("edited-model"));
    apiKeyEdit->setText(QString::fromLatin1(kSecretSentinel));
    connectTimeout->setValue(20'000);
    QCoreApplication::processEvents();

    QCOMPARE(saveSpy.count(), 0);
    QCOMPARE(deleteSpy.count(), 0);
}

void M3AiConfigurationWidgetTests::apiKeyIsPasswordAndStoredValueIsNeverFilled() {
    AiConfigurationWidget widget;
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* replaceKey = child<QCheckBox>(widget, "replaceKeyCheck");
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(replaceKey != nullptr);
    QCOMPARE(apiKeyEdit->echoMode(), QLineEdit::Password);

    apiKeyEdit->setText(QString::fromLatin1(kSecretSentinel));
    setRequiredFields(widget, true);
    QVERIFY(apiKeyEdit->text().isEmpty());
    QVERIFY(!apiKeyEdit->isEnabled());
    QVERIFY(!replaceKey->isChecked());
    QVERIFY(replaceKey->isEnabled());
    QVERIFY(!apiKeyEdit->placeholderText().isEmpty());

    apiKeyEdit->undo();
    QVERIFY(apiKeyEdit->text().isEmpty());
    QVERIFY(!apiKeyEdit->isUndoAvailable());
}

void M3AiConfigurationWidgetTests::onlySaveClickEmitsExactlyOneRequest() {
    AiConfigurationWidget widget;
    setRequiredFields(widget);
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* saveButton = child<QPushButton>(widget, "saveButton");
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(saveButton != nullptr);
    apiKeyEdit->setText(QString::fromLatin1(kSecretSentinel));

    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QVERIFY(saveSpy.isValid());
    showForInteraction(widget);
    QCoreApplication::processEvents();
    QCOMPARE(saveSpy.count(), 0);

    QTest::mouseClick(saveButton, Qt::LeftButton);
    QCOMPARE(saveSpy.count(), 1);
    const QList<QVariant> arguments = saveSpy.constFirst();
    QCOMPARE(arguments.size(), 7);
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("OpenAI"));
    QCOMPARE(arguments.at(1).toUrl(),
             QUrl(QStringLiteral("https://api.openai.com/v1")));
    QCOMPARE(arguments.at(2).toString(),
             QStringLiteral("gpt-image-model"));
    QCOMPARE(arguments.at(3).toInt(), 15'000);
    QCOMPARE(arguments.at(4).toInt(), 120'000);
    QCOMPARE(arguments.at(5).toString(),
             QString::fromLatin1(kSecretSentinel));
    QVERIFY(arguments.at(6).toBool());

    QCoreApplication::processEvents();
    QCOMPARE(saveSpy.count(), 1);
}

void M3AiConfigurationWidgetTests::baseUrlPolicy_data() {
    QTest::addColumn<QString>("baseUrl");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("public-https")
        << QStringLiteral("https://api.example.test/v1") << true;
    QTest::newRow("public-http")
        << QStringLiteral("http://api.example.test/v1") << false;
    QTest::newRow("userinfo")
        << QStringLiteral("https://user:secret@api.example.test/v1")
        << false;
    QTest::newRow("query")
        << QStringLiteral("https://api.example.test/v1?api_key=secret")
        << false;
    QTest::newRow("localhost-http")
        << QStringLiteral("http://localhost:8080/v1") << true;
    QTest::newRow("ipv4-loopback-http")
        << QStringLiteral("http://127.0.0.1:8080/v1") << true;
    QTest::newRow("ipv6-loopback-http")
        << QStringLiteral("http://[::1]:8080/v1") << true;
}

void M3AiConfigurationWidgetTests::baseUrlPolicy() {
    QFETCH(QString, baseUrl);
    QFETCH(bool, accepted);

    AiConfigurationWidget widget;
    setRequiredFields(widget);
    auto* baseUrlEdit = child<QLineEdit>(widget, "responsesBaseUrlEdit");
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* saveButton = child<QPushButton>(widget, "saveButton");
    QVERIFY(baseUrlEdit != nullptr);
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(saveButton != nullptr);
    baseUrlEdit->setText(baseUrl);
    apiKeyEdit->setText(QString::fromLatin1(kSecretSentinel));

    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QCOMPARE(saveSpy.count(), accepted ? 1 : 0);
    if (accepted) {
        QCOMPARE(saveSpy.constFirst().at(1).toUrl(), QUrl(baseUrl));
    } else {
        auto* status = child<QLabel>(widget, "saveStatusLabel");
        QVERIFY(status != nullptr);
        QVERIFY(!status->text().isEmpty());
    }
}

void M3AiConfigurationWidgetTests::storedKeyCanBeKeptWithoutDisclosure() {
    AiConfigurationWidget widget;
    setRequiredFields(widget, true);
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* replaceKey = child<QCheckBox>(widget, "replaceKeyCheck");
    auto* saveButton = child<QPushButton>(widget, "saveButton");
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(replaceKey != nullptr);
    QVERIFY(saveButton != nullptr);
    QVERIFY(apiKeyEdit->text().isEmpty());
    QVERIFY(!replaceKey->isChecked());

    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QCOMPARE(saveSpy.count(), 1);
    const QList<QVariant> arguments = saveSpy.constFirst();
    QVERIFY(arguments.at(5).toString().isEmpty());
    QVERIFY(!arguments.at(6).toBool());
}

void M3AiConfigurationWidgetTests::submittedKeyAndUndoHistoryAreCleared_data() {
    QTest::addColumn<bool>("hasStoredKey");
    QTest::newRow("new-key") << false;
    QTest::newRow("replace-key") << true;
}

void M3AiConfigurationWidgetTests::submittedKeyAndUndoHistoryAreCleared() {
    QFETCH(bool, hasStoredKey);

    AiConfigurationWidget widget;
    setRequiredFields(widget, hasStoredKey);
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* replaceKey = child<QCheckBox>(widget, "replaceKeyCheck");
    auto* saveButton = child<QPushButton>(widget, "saveButton");
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(replaceKey != nullptr);
    QVERIFY(saveButton != nullptr);
    if (hasStoredKey) {
        replaceKey->click();
        QVERIFY(replaceKey->isChecked());
        QVERIFY(apiKeyEdit->isEnabled());
    }

    showForInteraction(widget);
    apiKeyEdit->setFocus();
    QTest::keyClicks(apiKeyEdit, QString::fromLatin1(kSecretSentinel));
    QCOMPARE(apiKeyEdit->text(), QString::fromLatin1(kSecretSentinel));
    QVERIFY(apiKeyEdit->isUndoAvailable());

    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QCOMPARE(saveSpy.count(), 1);
    QCOMPARE(saveSpy.constFirst().at(5).toString(),
             QString::fromLatin1(kSecretSentinel));
    QVERIFY(saveSpy.constFirst().at(6).toBool());
    QVERIFY(apiKeyEdit->text().isEmpty());

    apiKeyEdit->undo();
    QVERIFY(apiKeyEdit->text().isEmpty());
    QVERIFY(!apiKeyEdit->isUndoAvailable());
}

void M3AiConfigurationWidgetTests::timeoutControlsEnforceRepositoryBounds() {
    AiConfigurationWidget widget;
    widget.setConfiguration(
        QStringLiteral("OpenAI"),
        QUrl(QStringLiteral("https://api.openai.com/v1")),
        QStringLiteral("model"), 30'000, 10'000, false);
    auto* connectTimeout =
        child<QSpinBox>(widget, "connectTimeoutSpin");
    auto* requestTimeout =
        child<QSpinBox>(widget, "requestTimeoutSpin");
    auto* apiKeyEdit = child<QLineEdit>(widget, "apiKeyEdit");
    auto* saveButton = child<QPushButton>(widget, "saveButton");
    QVERIFY(connectTimeout != nullptr);
    QVERIFY(requestTimeout != nullptr);
    QVERIFY(apiKeyEdit != nullptr);
    QVERIFY(saveButton != nullptr);

    QCOMPARE(connectTimeout->minimum(), 1'000);
    QCOMPARE(connectTimeout->maximum(), 120'000);
    QCOMPARE(requestTimeout->maximum(), 600'000);
    QCOMPARE(connectTimeout->value(), 30'000);
    QCOMPARE(requestTimeout->minimum(), 30'000);
    QCOMPARE(requestTimeout->value(), 30'000);

    connectTimeout->setValue(90'000);
    QCOMPARE(requestTimeout->minimum(), 90'000);
    QVERIFY(requestTimeout->value() >= connectTimeout->value());
    requestTimeout->setValue(200'000);
    apiKeyEdit->setText(QString::fromLatin1(kSecretSentinel));

    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QCOMPARE(saveSpy.count(), 1);
    QCOMPARE(saveSpy.constFirst().at(3).toInt(), 90'000);
    QCOMPARE(saveSpy.constFirst().at(4).toInt(), 200'000);
}

void M3AiConfigurationWidgetTests::deleteButtonEmitsCredentialChoice() {
    AiConfigurationWidget widget;
    setRequiredFields(widget, true);
    auto* deleteCredential =
        child<QCheckBox>(widget, "deleteCredentialCheck");
    auto* deleteButton = child<QPushButton>(widget, "deleteButton");
    QVERIFY(deleteCredential != nullptr);
    QVERIFY(deleteButton != nullptr);
    QVERIFY(deleteCredential->isEnabled());
    QVERIFY(deleteCredential->isChecked());

    QSignalSpy deleteSpy(&widget,
                         &AiConfigurationWidget::deleteRequested);
    QSignalSpy saveSpy(&widget,
                       &AiConfigurationWidget::saveRequested);
    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCOMPARE(deleteSpy.count(), 1);
    QVERIFY(deleteSpy.constFirst().constFirst().toBool());
    QCOMPARE(saveSpy.count(), 0);

    deleteCredential->setChecked(false);
    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCOMPARE(deleteSpy.count(), 2);
    QVERIFY(!deleteSpy.at(1).constFirst().toBool());
    QCOMPARE(saveSpy.count(), 0);
}

}  // namespace
}  // namespace snapask::ui

QTEST_MAIN(snapask::ui::M3AiConfigurationWidgetTests)

#include "M3AiConfigurationWidgetTests.moc"
