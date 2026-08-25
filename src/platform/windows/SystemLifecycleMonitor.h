#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

#include <optional>

class QScreen;

namespace snapask::platform::windows {

enum class SystemLifecycleEvent : quint8 {
    None = 0,
    DisplayTopologyChanged,
    DpiChanged,
    SessionLocked,
    SessionUnlocked,
    SystemSuspending,
    SystemResumed,
    ConsoleSessionConnected,
    ConsoleSessionDisconnected,
    RemoteSessionConnected,
    RemoteSessionDisconnected,
    SessionLoggedOn,
    SessionLoggedOff,
    RemoteControlChanged,
};

struct SystemLifecycleNotification final {
    SystemLifecycleEvent event{SystemLifecycleEvent::None};
    quint32 sessionId{0};

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool operator==(const SystemLifecycleNotification& other) const noexcept = default;
};

// A clock-independent trailing-edge debouncer. Keeping this policy separate from
// the native event source makes burst handling deterministic and unit-testable.
class LifecycleEventDebouncer final {
public:
    static constexpr qint64 defaultIntervalMilliseconds = 150;

    explicit LifecycleEventDebouncer(
        qint64 intervalMilliseconds = defaultIntervalMilliseconds) noexcept;

    [[nodiscard]] qint64 intervalMilliseconds() const noexcept;
    [[nodiscard]] bool enqueue(
        const SystemLifecycleNotification& notification,
        qint64 monotonicMilliseconds) noexcept;
    [[nodiscard]] QVector<SystemLifecycleNotification> takeDue(
        qint64 monotonicMilliseconds);
    [[nodiscard]] std::optional<qint64> millisecondsUntilNext(
        qint64 monotonicMilliseconds) const noexcept;
    [[nodiscard]] qsizetype pendingCount() const noexcept;
    void clear() noexcept;

private:
    struct PendingNotification final {
        SystemLifecycleNotification notification;
        qint64 dueAtMilliseconds{0};
        quint64 insertionOrder{0};
    };

    [[nodiscard]] static qint64 saturatedAdd(
        qint64 value,
        qint64 nonNegativeDelta) noexcept;

    qint64 intervalMilliseconds_{defaultIntervalMilliseconds};
    quint64 nextInsertionOrder_{0};
    QVector<PendingNotification> pending_;
};

// Receives real Windows lifecycle broadcasts without requesting elevation. A
// hidden top-level window is used because message-only windows do not receive
// all power/display broadcasts. Qt screen signals complement the Win32 source.
class SystemLifecycleMonitor final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit SystemLifecycleMonitor(
        qint64 debounceIntervalMilliseconds = LifecycleEventDebouncer::defaultIntervalMilliseconds,
        QObject* parent = nullptr);
    ~SystemLifecycleMonitor() override;

    SystemLifecycleMonitor(const SystemLifecycleMonitor&) = delete;
    SystemLifecycleMonitor& operator=(const SystemLifecycleMonitor&) = delete;

    [[nodiscard]] bool start(QString* error = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool sessionNotificationsAvailable() const noexcept;
    [[nodiscard]] qint64 debounceIntervalMilliseconds() const noexcept;

    bool nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        qintptr* result) override;

    // Public for the hidden Win32 window procedure and deterministic adapters.
    // Recognized messages are observed but never consumed from the native loop.
    [[nodiscard]] bool processWindowsMessage(
        quint32 message,
        quintptr wParam,
        qintptr lParam);

    [[nodiscard]] static std::optional<SystemLifecycleNotification> classifyWindowsMessage(
        quint32 message,
        quintptr wParam,
        qintptr lParam) noexcept;

signals:
    void lifecycleEventObserved(
        const snapask::platform::windows::SystemLifecycleNotification& notification);
    void displayTopologyChanged();
    void dpiChanged();
    void sessionChanged(
        snapask::platform::windows::SystemLifecycleEvent event,
        quint32 sessionId);
    void sessionLocked(quint32 sessionId);
    void sessionUnlocked(quint32 sessionId);
    void systemSuspending();
    void systemResumed();
    void remoteSessionChanged(bool connected, quint32 sessionId);

private slots:
    void flushDebouncedEvents();

private:
    void queueNotification(const SystemLifecycleNotification& notification);
    void armDebounceTimer();
    void emitNotification(const SystemLifecycleNotification& notification);
    void connectQtScreenSignals(QScreen* screen);
    void connectQtDisplaySources();
    void disconnectQtDisplaySources() noexcept;
    [[nodiscard]] bool createMessageWindow(QString* error);
    void destroyMessageWindow() noexcept;
    void registerSessionNotifications() noexcept;
    void unregisterSessionNotifications() noexcept;

    bool running_{false};
    bool nativeFilterInstalled_{false};
    bool sessionNotificationsRegistered_{false};
    void* messageWindow_{nullptr};
    void* wtsApiModule_{nullptr};
    LifecycleEventDebouncer debouncer_;
    QElapsedTimer monotonicClock_;
    QTimer debounceTimer_;
    QVector<QMetaObject::Connection> displayConnections_;
    QSet<QScreen*> connectedScreens_;
};

}  // namespace snapask::platform::windows

Q_DECLARE_METATYPE(snapask::platform::windows::SystemLifecycleEvent)
Q_DECLARE_METATYPE(snapask::platform::windows::SystemLifecycleNotification)
