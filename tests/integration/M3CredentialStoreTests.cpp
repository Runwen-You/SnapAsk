#include "platform/windows/CredentialStore.h"

#include <QtTest>
#include <QScopeGuard>
#include <QUuid>

using snapask::platform::windows::CredentialStore;

class M3CredentialStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void writeReadDelete();
};

void M3CredentialStoreTests::writeReadDelete()
{
    CredentialStore store;
    const auto reference = QStringLiteral("SnapAsk/provider/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto secret = QStringLiteral("credential-test-sentinel-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto cleanup = qScopeGuard([&store, &reference] {
        store.remove(reference);
    });
    QString error;
    QVERIFY(!store.contains(reference, &error));
    QVERIFY(error.isEmpty());
    QVERIFY2(store.write(reference, secret, &error), qPrintable(error));
    QVERIFY2(store.contains(reference, &error), qPrintable(error));
    const auto loaded = store.read(reference, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(*loaded, secret);
    QVERIFY2(store.remove(reference, &error), qPrintable(error));
    QVERIFY(!store.contains(reference, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!store.read(reference).has_value());
}

QTEST_GUILESS_MAIN(M3CredentialStoreTests)
#include "M3CredentialStoreTests.moc"
