#include "app/SingleInstance.h"
#include "infrastructure/RedactingLogger.h"
#include "ui/common/ThemeTokens.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QThread>
#include <QTest>
#include <QUuid>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

class RedactingLoggerTests final : public QObject {
    Q_OBJECT

private slots:
    void keepsOrdinaryDiagnostics() {
        const QString message = QStringLiteral("capture subsystem initialized in 12 ms");
        QCOMPARE(snapask::infrastructure::RedactingLogger::redact(message), message);
    }

    void redactsCredentialsAndAuthorization() {
        const QString message = QStringLiteral(
            R"({"Authorization":"Bearer sk-live-secret123","api_key":"another-secret","status":401})");
        const QString safe = snapask::infrastructure::RedactingLogger::redact(message);

        QVERIFY(!safe.contains(QStringLiteral("sk-live-secret123")));
        QVERIFY(!safe.contains(QStringLiteral("another-secret")));
        QVERIFY(safe.count(QStringLiteral("<redacted>")) >= 2);
        QVERIFY(safe.contains(QStringLiteral("401")));
    }

    void redactsUserAndModelContent() {
        const QString message = QStringLiteral(
            R"({"question":"这张截图是什么？","answer":"完整回答","request_body":"raw payload"})");
        const QString safe = snapask::infrastructure::RedactingLogger::redact(message);

        QVERIFY(!safe.contains(QStringLiteral("这张截图是什么")));
        QVERIFY(!safe.contains(QStringLiteral("完整回答")));
        QVERIFY(!safe.contains(QStringLiteral("raw payload")));
    }

    void redactsImagesAndQuerySecrets() {
        const QString encoded(180, QLatin1Char('A'));
        const QString message = QStringLiteral("url=https://example.test/v1?api_key=url-secret image=data:image/png;base64,")
            + encoded;
        const QString safe = snapask::infrastructure::RedactingLogger::redact(message);

        QVERIFY(!safe.contains(QStringLiteral("url-secret")));
        QVERIFY(!safe.contains(encoded));
        QVERIFY(safe.contains(QStringLiteral("data:image/png;base64,<redacted>")));
    }
};

class ThemeTokenTests final : public QObject {
    Q_OBJECT

private slots:
    void storageValuesRoundTrip() {
        using snapask::ui::ThemeMode;
        using snapask::ui::ThemeTokens;

        QCOMPARE(ThemeTokens::fromStorage(ThemeTokens::toStorage(ThemeMode::System)), ThemeMode::System);
        QCOMPARE(ThemeTokens::fromStorage(ThemeTokens::toStorage(ThemeMode::Light)), ThemeMode::Light);
        QCOMPARE(ThemeTokens::fromStorage(ThemeTokens::toStorage(ThemeMode::Dark)), ThemeMode::Dark);
        QCOMPARE(ThemeTokens::fromStorage(QStringLiteral("unexpected")), ThemeMode::System);
    }

    void lightAndDarkTokensRemainReadable() {
        const auto light = snapask::ui::ThemeTokens::resolve(snapask::ui::ThemeMode::Light);
        const auto dark = snapask::ui::ThemeTokens::resolve(snapask::ui::ThemeMode::Dark);

        QVERIFY(light.textPrimary.lightness() < light.surface.lightness());
        QVERIFY(dark.textPrimary.lightness() > dark.surface.lightness());
        QCOMPARE(light.panelRadius, 18);
        QCOMPARE(dark.animationDurationMs, 180);
        QVERIFY(snapask::ui::ThemeTokens::styleSheet(light).contains(QStringLiteral("border-radius: 18px")));
    }
};

class SingleInstanceTests final : public QObject {
    Q_OBJECT

private slots:
    void secondInstanceActivatesPrimary() {
        const QString serverName = QStringLiteral("SnapAsk.UnitTest.")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);

        snapask::app::SingleInstance primary(serverName);
        QCOMPARE(primary.start(), snapask::app::SingleInstance::StartResult::Primary);
        QVERIFY(primary.isPrimary());

        QSignalSpy activationSpy(&primary, &snapask::app::SingleInstance::activationRequested);

        std::atomic<bool> secondaryFinished = false;
        std::atomic secondaryResult = snapask::app::SingleInstance::StartResult::Error;
        std::thread secondaryThread([&]() {
            snapask::app::SingleInstance secondary(serverName);
            secondaryResult.store(secondary.start());
            secondaryFinished.store(true);
        });

        QElapsedTimer waitTimer;
        waitTimer.start();
        while (!secondaryFinished.load() && waitTimer.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(2);
        }
        secondaryThread.join();

        QVERIFY(secondaryFinished.load());
        QCOMPARE(secondaryResult.load(), snapask::app::SingleInstance::StartResult::SecondaryNotified);

        QTRY_COMPARE_WITH_TIMEOUT(activationSpy.count(), 1, 2000);
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnapAskTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SnapAskM0Tests"));

    int status = 0;
    RedactingLoggerTests loggerTests;
    const int loggerStatus = QTest::qExec(&loggerTests, argc, argv);
    std::fprintf(stderr, "RedactingLoggerTests status: %d\n", loggerStatus);
    status |= loggerStatus;

    ThemeTokenTests themeTests;
    const int themeStatus = QTest::qExec(&themeTests, argc, argv);
    std::fprintf(stderr, "ThemeTokenTests status: %d\n", themeStatus);
    status |= themeStatus;

    SingleInstanceTests singleInstanceTests;
    const int singleInstanceStatus = QTest::qExec(&singleInstanceTests, argc, argv);
    std::fprintf(stderr, "SingleInstanceTests status: %d\n", singleInstanceStatus);
    status |= singleInstanceStatus;
    return status;
}

#include "M0Tests.moc"
