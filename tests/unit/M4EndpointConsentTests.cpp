#include "infrastructure/EndpointConsentStore.h"

#include <QTemporaryDir>
#include <QtTest>

using snapask::infrastructure::EndpointConsentStore;

class M4EndpointConsentTests final : public QObject {
    Q_OBJECT

private slots:
    void officialOpenAiOriginNeedsNoConsent();
    void customOriginRequiresPersistentApproval();
    void originNormalizationIgnoresPathAndCase();
    void originChangesRequireNewApproval();
    void invalidEndpointsCannotBeApproved();
};

void M4EndpointConsentTests::officialOpenAiOriginNeedsNoConsent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    EndpointConsentStore store(directory.filePath(QStringLiteral("consent.ini")));
    const QUrl official(QStringLiteral("https://api.openai.com/v1"));
    QVERIFY(!EndpointConsentStore::requiresConsent(official));
    QVERIFY(store.isApproved(official));
    QVERIFY(store.approvedOrigins().isEmpty());
}

void M4EndpointConsentTests::customOriginRequiresPersistentApproval()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath(QStringLiteral("consent.ini"));
    const QUrl custom(QStringLiteral("https://vision.example.test/v1"));
    {
        EndpointConsentStore store(filePath);
        QVERIFY(EndpointConsentStore::requiresConsent(custom));
        QVERIFY(!store.isApproved(custom));
        QString error;
        QVERIFY2(store.approve(custom, &error), qPrintable(error));
        QVERIFY(store.isApproved(custom));
    }
    EndpointConsentStore reloaded(filePath);
    QVERIFY(reloaded.isApproved(custom));
    QCOMPARE(reloaded.approvedOrigins(),
             QStringList{QStringLiteral("https://vision.example.test:443")});
}

void M4EndpointConsentTests::originNormalizationIgnoresPathAndCase()
{
    const QUrl first(QStringLiteral("HTTPS://Vision.Example.Test/v1/responses"));
    const QUrl second(QStringLiteral("https://vision.example.test:443/other"));
    QCOMPARE(EndpointConsentStore::normalizedOrigin(first),
             EndpointConsentStore::normalizedOrigin(second));

    QTemporaryDir directory;
    EndpointConsentStore store(directory.filePath(QStringLiteral("consent.ini")));
    QVERIFY(store.approve(first));
    QVERIFY(store.isApproved(second));
}

void M4EndpointConsentTests::originChangesRequireNewApproval()
{
    QTemporaryDir directory;
    EndpointConsentStore store(directory.filePath(QStringLiteral("consent.ini")));
    const QUrl original(QStringLiteral("http://127.0.0.1:31001/v1"));
    QVERIFY(store.approve(original));
    QVERIFY(store.isApproved(QUrl(QStringLiteral("http://127.0.0.1:31001/other"))));
    QVERIFY(!store.isApproved(QUrl(QStringLiteral("http://127.0.0.1:31002/v1"))));
    QVERIFY(!store.isApproved(QUrl(QStringLiteral("https://127.0.0.1:31001/v1"))));
    QVERIFY(!store.isApproved(QUrl(QStringLiteral("http://localhost:31001/v1"))));
}

void M4EndpointConsentTests::invalidEndpointsCannotBeApproved()
{
    QTemporaryDir directory;
    EndpointConsentStore store(directory.filePath(QStringLiteral("consent.ini")));
    QString error;
    QVERIFY(!store.approve(QUrl(QStringLiteral("file:///tmp/model")), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.isApproved(QUrl(QStringLiteral("https:///missing-host"))));
}

QTEST_GUILESS_MAIN(M4EndpointConsentTests)
#include "M4EndpointConsentTests.moc"
