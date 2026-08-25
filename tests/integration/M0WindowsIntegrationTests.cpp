#include "app/SingleInstance.h"
#include "platform/windows/GlobalHotkey.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QUuid>

#include <cstdio>
#include <optional>

namespace {

constexpr auto kSecondaryMode = "--single-instance-secondary";
constexpr auto kSecondaryNotifiedMarker = "secondary-notified";

[[nodiscard]] QString uniqueInstanceName()
{
    return QStringLiteral("SnapAsk.IntegrationTest.")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] int runSingleInstanceSecondary(const QString& serverName)
{
    snapask::app::SingleInstance secondary(serverName);
    if (secondary.start()
        != snapask::app::SingleInstance::StartResult::SecondaryNotified) {
        return 2;
    }

    const QByteArray marker = QByteArray(kSecondaryNotifiedMarker) + '\n';
    if (fwrite(marker.constData(), 1, static_cast<size_t>(marker.size()), stdout)
        != static_cast<size_t>(marker.size())) {
        return 3;
    }
    return 0;
}

class M0WindowsIntegrationTests final : public QObject {
    Q_OBJECT

private slots:
    void competingGlobalHotkeyIsReportedAndReleased();
    void realSecondProcessActivatesPrimary();
};

void M0WindowsIntegrationTests::competingGlobalHotkeyIsReportedAndReleased()
{
    using snapask::platform::windows::GlobalHotkey;
    using snapask::platform::windows::HotkeyAlt;
    using snapask::platform::windows::HotkeyChord;
    using snapask::platform::windows::HotkeyControl;
    using snapask::platform::windows::HotkeyShift;
    using snapask::platform::windows::HotkeyWindows;

    // F13..F24 with three or four modifiers are deliberately chosen so the
    // test does not take over a shortcut a user is likely to rely on. Probe a
    // small set so a pre-existing registration cannot make the test flaky.
    constexpr quint32 virtualKeyF13 = 0x7C;
    constexpr quint32 virtualKeyF24 = 0x87;
    constexpr quint32 threeModifiers = HotkeyControl | HotkeyAlt | HotkeyShift;
    constexpr quint32 fourModifiers = threeModifiers | HotkeyWindows;

    GlobalHotkey owner;
    std::optional<HotkeyChord> claimedChord;
    QString lastRegistrationError;
    for (const quint32 modifiers : {threeModifiers, fourModifiers}) {
        for (quint32 key = virtualKeyF24; key >= virtualKeyF13; --key) {
            const HotkeyChord candidate{key, modifiers};
            if (owner.registerHotkey(candidate, &lastRegistrationError)) {
                claimedChord = candidate;
                break;
            }
        }
        if (claimedChord.has_value()) {
            break;
        }
    }

    QVERIFY2(claimedChord.has_value(), qPrintable(lastRegistrationError));
    QVERIFY(owner.isRegistered());
    QCOMPARE(owner.chord().virtualKey, claimedChord->virtualKey);
    QCOMPARE(owner.chord().modifiers, claimedChord->modifiers);

    GlobalHotkey competitor;
    QString conflictError;
    QVERIFY(!competitor.registerHotkey(*claimedChord, &conflictError));
    QVERIFY(!competitor.isRegistered());
    QVERIFY(conflictError.contains(QStringLiteral("RegisterHotKey failed")));
    QVERIFY(!conflictError.trimmed().isEmpty());

    owner.unregisterHotkey();
    QVERIFY(!owner.isRegistered());

    QString reclaimedError;
    QVERIFY2(competitor.registerHotkey(*claimedChord, &reclaimedError),
             qPrintable(reclaimedError));
    QVERIFY(competitor.isRegistered());
    competitor.unregisterHotkey();
    QVERIFY(!competitor.isRegistered());
}

void M0WindowsIntegrationTests::realSecondProcessActivatesPrimary()
{
    const QString serverName = uniqueInstanceName();
    snapask::app::SingleInstance primary(serverName);
    QCOMPARE(primary.start(), snapask::app::SingleInstance::StartResult::Primary);
    QVERIFY(primary.isPrimary());

    QSignalSpy activationSpy(&primary, &snapask::app::SingleInstance::activationRequested);
    QProcess secondary;
    secondary.setProgram(QCoreApplication::applicationFilePath());
    secondary.setArguments({QString::fromLatin1(kSecondaryMode), serverName});
    secondary.setProcessChannelMode(QProcess::SeparateChannels);
    secondary.start();
    QVERIFY2(secondary.waitForStarted(3'000), qPrintable(secondary.errorString()));

    const qint64 secondaryPid = secondary.processId();
    QVERIFY(secondaryPid > 0);
    QVERIFY(secondaryPid != QCoreApplication::applicationPid());

    QElapsedTimer timer;
    timer.start();
    while (secondary.state() != QProcess::NotRunning && timer.elapsed() < 5'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }

    const bool finished = secondary.state() == QProcess::NotRunning;
    if (!finished) {
        secondary.kill();
        secondary.waitForFinished(3'000);
    }
    QVERIFY2(finished, "The true secondary process did not finish within five seconds.");

    const QByteArray childDiagnostics = secondary.readAllStandardError();
    QVERIFY2(secondary.exitStatus() == QProcess::NormalExit, childDiagnostics.constData());
    QCOMPARE(secondary.exitCode(), 0);
    QCOMPARE(secondary.readAllStandardOutput().trimmed(),
             QByteArray(kSecondaryNotifiedMarker));
    QTRY_COMPARE_WITH_TIMEOUT(activationSpy.count(), 1, 2'000);
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnapAskTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SnapAskM0WindowsIntegrationTests"));

    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() == 3 && arguments.at(1) == QString::fromLatin1(kSecondaryMode)) {
        return runSingleInstanceSecondary(arguments.at(2));
    }

    M0WindowsIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "M0WindowsIntegrationTests.moc"
