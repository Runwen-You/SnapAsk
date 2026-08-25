#include "platform/windows/SystemLifecycleMonitor.h"
#include "services/SessionMemoryBudget.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QSignalSpy>
#include <QtTest>

#include <limits>

namespace {

using snapask::MemoryBudgetStatus;
using snapask::SessionMemoryBudget;
using snapask::platform::windows::LifecycleEventDebouncer;
using snapask::platform::windows::SystemLifecycleEvent;
using snapask::platform::windows::SystemLifecycleMonitor;
using snapask::platform::windows::SystemLifecycleNotification;

constexpr quint32 wmWtsSessionChange = 0x02B1;
constexpr quintptr wtsRemoteConnect = 0x3;
constexpr quintptr wtsSessionLock = 0x7;

class M6HardeningTests final : public QObject {
    Q_OBJECT

private slots:
    void classifiesWindowsLifecycleMessages();
    void debouncesBurstsWithoutLosingTransitions();
    void monitorProcessesInjectedNativeMessages();
    void memoryBudgetEvictsLeastRecentlyUsedCache();
    void rejectedBudgetChangesAreTransactional();
    void reportsUnreclaimableLimitAndOverflow();
    void calculatesImageStorageWithoutOverflow();
};

void M6HardeningTests::classifiesWindowsLifecycleMessages()
{
    const auto topology = SystemLifecycleMonitor::classifyWindowsMessage(
        WM_DISPLAYCHANGE,
        0,
        0);
    QVERIFY(topology.has_value());
    QCOMPARE(topology->event, SystemLifecycleEvent::DisplayTopologyChanged);
    QCOMPARE(topology->sessionId, 0U);

    const auto dpi = SystemLifecycleMonitor::classifyWindowsMessage(WM_DPICHANGED, 0, 0);
    QVERIFY(dpi.has_value());
    QCOMPARE(dpi->event, SystemLifecycleEvent::DpiChanged);

    const auto suspend = SystemLifecycleMonitor::classifyWindowsMessage(
        WM_POWERBROADCAST,
        PBT_APMSUSPEND,
        0);
    QVERIFY(suspend.has_value());
    QCOMPARE(suspend->event, SystemLifecycleEvent::SystemSuspending);

    const auto resume = SystemLifecycleMonitor::classifyWindowsMessage(
        WM_POWERBROADCAST,
        PBT_APMRESUMEAUTOMATIC,
        0);
    QVERIFY(resume.has_value());
    QCOMPARE(resume->event, SystemLifecycleEvent::SystemResumed);

    const auto locked = SystemLifecycleMonitor::classifyWindowsMessage(
        wmWtsSessionChange,
        wtsSessionLock,
        41);
    QVERIFY(locked.has_value());
    QCOMPARE(locked->event, SystemLifecycleEvent::SessionLocked);
    QCOMPARE(locked->sessionId, 41U);

    const auto remote = SystemLifecycleMonitor::classifyWindowsMessage(
        wmWtsSessionChange,
        wtsRemoteConnect,
        77);
    QVERIFY(remote.has_value());
    QCOMPARE(remote->event, SystemLifecycleEvent::RemoteSessionConnected);
    QCOMPARE(remote->sessionId, 77U);

    QVERIFY(!SystemLifecycleMonitor::classifyWindowsMessage(WM_CLOSE, 0, 0).has_value());
    QVERIFY(!SystemLifecycleMonitor::classifyWindowsMessage(
                 WM_POWERBROADCAST,
                 PBT_APMPOWERSTATUSCHANGE,
                 0)
                 .has_value());
}

void M6HardeningTests::debouncesBurstsWithoutLosingTransitions()
{
    LifecycleEventDebouncer debouncer(100);
    const SystemLifecycleNotification topology{
        SystemLifecycleEvent::DisplayTopologyChanged,
        0};
    const SystemLifecycleNotification dpi{SystemLifecycleEvent::DpiChanged, 0};
    const SystemLifecycleNotification locked{SystemLifecycleEvent::SessionLocked, 8};
    const SystemLifecycleNotification unlocked{SystemLifecycleEvent::SessionUnlocked, 8};

    QVERIFY(debouncer.enqueue(topology, 0));
    QVERIFY(debouncer.enqueue(dpi, 40));
    QVERIFY(debouncer.enqueue(topology, 50));
    QVERIFY(debouncer.enqueue(locked, 60));
    QVERIFY(debouncer.enqueue(unlocked, 61));
    QCOMPARE(debouncer.pendingCount(), 4);
    QCOMPARE(debouncer.millisecondsUntilNext(100), std::optional<qint64>(40));

    QVERIFY(debouncer.takeDue(139).isEmpty());
    const QVector<SystemLifecycleNotification> firstDue = debouncer.takeDue(140);
    QCOMPARE(firstDue, QVector<SystemLifecycleNotification>{dpi});
    QVERIFY(debouncer.takeDue(149).isEmpty());
    QCOMPARE(debouncer.takeDue(150), QVector<SystemLifecycleNotification>{topology});
    QCOMPARE(debouncer.takeDue(160), QVector<SystemLifecycleNotification>{locked});
    QCOMPARE(debouncer.takeDue(161), QVector<SystemLifecycleNotification>{unlocked});
    QCOMPARE(debouncer.pendingCount(), 0);
    QVERIFY(!debouncer.millisecondsUntilNext(161).has_value());

    QVERIFY(!debouncer.enqueue({}, 200));
    QVERIFY(!debouncer.enqueue(topology, -1));
}

void M6HardeningTests::monitorProcessesInjectedNativeMessages()
{
    SystemLifecycleMonitor monitor(0);
    QString error;
    QVERIFY2(monitor.start(&error), qPrintable(error));
    QVERIFY(monitor.isRunning());

    QSignalSpy topologySpy(&monitor, &SystemLifecycleMonitor::displayTopologyChanged);
    QSignalSpy remoteSpy(&monitor, &SystemLifecycleMonitor::remoteSessionChanged);
    QVERIFY(monitor.processWindowsMessage(WM_DISPLAYCHANGE, 0, 0));
    QVERIFY(monitor.processWindowsMessage(wmWtsSessionChange, wtsRemoteConnect, 99));
    QVERIFY(!monitor.processWindowsMessage(WM_CLOSE, 0, 0));

    QTRY_COMPARE(topologySpy.count(), 1);
    QTRY_COMPARE(remoteSpy.count(), 1);
    QCOMPARE(remoteSpy.at(0).at(0).toBool(), true);
    QCOMPARE(remoteSpy.at(0).at(1).toUInt(), 99U);

    monitor.stop();
    QVERIFY(!monitor.isRunning());
}

void M6HardeningTests::memoryBudgetEvictsLeastRecentlyUsedCache()
{
    SessionMemoryBudget budget(1'000);
    QVERIFY(budget.setOriginalImageBytes(500).accepted());
    QVERIFY(budget.upsertRebuildableCache(QStringLiteral("preview-a"), 200).accepted());
    QVERIFY(budget.upsertRebuildableCache(QStringLiteral("preview-b"), 200).accepted());
    QVERIFY(budget.touchRebuildableCache(QStringLiteral("preview-a")));

    const auto result = budget.upsertSentSnapshotPng(QByteArrayLiteral("sent-v1"), 300);
    QVERIFY(result.accepted());
    QCOMPARE(result.status, MemoryBudgetStatus::AcceptedAfterCacheEviction);
    QCOMPARE(result.evictedCacheKeys, QStringList{QStringLiteral("preview-b")});
    QCOMPARE(result.usedBytesAfter, 1'000ULL);
    QCOMPARE(budget.usedBytes(), 1'000ULL);
    QVERIFY(budget.containsRebuildableCache(QStringLiteral("preview-a")));
    QVERIFY(!budget.containsRebuildableCache(QStringLiteral("preview-b")));
    QVERIFY(budget.containsSentSnapshot(QByteArrayLiteral("sent-v1")));
    QCOMPARE(budget.unreclaimableBytes(), 800ULL);
    QCOMPARE(budget.reclaimableBytes(), 200ULL);
}

void M6HardeningTests::rejectedBudgetChangesAreTransactional()
{
    SessionMemoryBudget budget(1'000);
    QVERIFY(budget.setOriginalImageBytes(500).accepted());
    QVERIFY(budget.upsertSentSnapshotPng(QByteArrayLiteral("sent-v1"), 300).accepted());
    QVERIFY(budget.upsertRebuildableCache(QStringLiteral("preview"), 200).accepted());

    const auto replacement = budget.setOriginalImageBytes(800);
    QVERIFY(!replacement.accepted());
    QCOMPARE(replacement.status, MemoryBudgetStatus::RejectedUnreclaimableFootprint);
    QCOMPARE(replacement.minimumRequiredBytes, 1'100ULL);
    QCOMPARE(budget.originalImageBytes(), 500ULL);
    QCOMPARE(budget.usedBytes(), 1'000ULL);
    QVERIFY(budget.containsRebuildableCache(QStringLiteral("preview")));

    const auto limitChange = budget.setHardLimitBytes(700);
    QVERIFY(!limitChange.accepted());
    QCOMPARE(limitChange.status, MemoryBudgetStatus::RejectedUnreclaimableFootprint);
    QCOMPARE(limitChange.minimumRequiredBytes, 800ULL);
    QCOMPARE(budget.hardLimitBytes(), 1'000ULL);
    QCOMPARE(budget.usedBytes(), 1'000ULL);
    QVERIFY(budget.containsRebuildableCache(QStringLiteral("preview")));

    const auto oversizedCache = budget.upsertRebuildableCache(
        QStringLiteral("oversized"),
        500);
    QVERIFY(!oversizedCache.accepted());
    QCOMPARE(oversizedCache.status, MemoryBudgetStatus::RejectedHardLimit);
    QVERIFY(!budget.containsRebuildableCache(QStringLiteral("oversized")));
    QCOMPARE(budget.usedBytes(), 1'000ULL);
}

void M6HardeningTests::reportsUnreclaimableLimitAndOverflow()
{
    SessionMemoryBudget constrained(600);
    QVERIFY(constrained.setOriginalImageBytes(500).accepted());
    const auto unreclaimable = constrained.upsertSentSnapshotPng(
        QByteArrayLiteral("sent-v1"),
        150);
    QVERIFY(!unreclaimable.accepted());
    QCOMPARE(unreclaimable.status, MemoryBudgetStatus::RejectedUnreclaimableFootprint);
    QCOMPARE(unreclaimable.minimumRequiredBytes, 650ULL);
    QCOMPARE(constrained.usedBytes(), 500ULL);
    QCOMPARE(constrained.sentSnapshotCount(), 0);

    SessionMemoryBudget overflowBudget(std::numeric_limits<quint64>::max());
    QVERIFY(overflowBudget
                .setOriginalImageBytes(std::numeric_limits<quint64>::max() - 4)
                .accepted());
    const auto overflow = overflowBudget.upsertSentSnapshotPng(
        QByteArrayLiteral("overflow"),
        8);
    QVERIFY(!overflow.accepted());
    QCOMPARE(overflow.status, MemoryBudgetStatus::RejectedArithmeticOverflow);
    QCOMPARE(overflowBudget.sentSnapshotCount(), 0);
    QCOMPARE(
        overflowBudget.usedBytes(),
        std::numeric_limits<quint64>::max() - 4);
}

void M6HardeningTests::calculatesImageStorageWithoutOverflow()
{
    QCOMPARE(SessionMemoryBudget::checkedImageByteSize(15'360, 2'160),
             std::optional<quint64>(33'177'600));
    QCOMPARE(SessionMemoryBudget::checkedImageByteSize(0, 2'160),
             std::optional<quint64>(0));
    QVERIFY(!SessionMemoryBudget::checkedImageByteSize(-1, 2'160).has_value());
    QVERIFY(!SessionMemoryBudget::checkedImageByteSize(
                 std::numeric_limits<qint64>::max(),
                 std::numeric_limits<qint64>::max())
                 .has_value());
}

}  // namespace

QTEST_GUILESS_MAIN(M6HardeningTests)

#include "M6HardeningTests.moc"
