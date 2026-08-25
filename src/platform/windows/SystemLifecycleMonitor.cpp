#include "platform/windows/SystemLifecycleMonitor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <limits>
#include <mutex>

namespace snapask::platform::windows {

namespace {

constexpr wchar_t lifecycleWindowClassName[] = L"SnapAsk.SystemLifecycleMonitor";
constexpr UINT wmWtsSessionChange = 0x02B1;
constexpr WPARAM wtsConsoleConnect = 0x1;
constexpr WPARAM wtsConsoleDisconnect = 0x2;
constexpr WPARAM wtsRemoteConnect = 0x3;
constexpr WPARAM wtsRemoteDisconnect = 0x4;
constexpr WPARAM wtsSessionLogon = 0x5;
constexpr WPARAM wtsSessionLogoff = 0x6;
constexpr WPARAM wtsSessionLock = 0x7;
constexpr WPARAM wtsSessionUnlock = 0x8;
constexpr WPARAM wtsRemoteControl = 0x9;
constexpr DWORD notifyForThisSession = 0;

using RegisterSessionNotification = BOOL(WINAPI*)(HWND, DWORD);
using UnregisterSessionNotification = BOOL(WINAPI*)(HWND);

[[nodiscard]] LRESULT CALLBACK lifecycleWindowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    SystemLifecycleMonitor* monitor = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        monitor = static_cast<SystemLifecycleMonitor*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(monitor));
    } else {
        monitor = reinterpret_cast<SystemLifecycleMonitor*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (monitor != nullptr) {
        (void)monitor->processWindowsMessage(
            static_cast<quint32>(message),
            static_cast<quintptr>(wParam),
            static_cast<qintptr>(lParam));
    }

    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

[[nodiscard]] bool ensureLifecycleWindowClass(QString* error)
{
    static std::once_flag registrationFlag;
    static bool registered = false;
    static DWORD registrationError = ERROR_SUCCESS;

    std::call_once(registrationFlag, [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = lifecycleWindowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = lifecycleWindowClassName;
        const ATOM atom = RegisterClassExW(&windowClass);
        if (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            registered = true;
            registrationError = ERROR_SUCCESS;
        } else {
            registrationError = GetLastError();
        }
    });

    if (!registered && error != nullptr) {
        *error = QStringLiteral("Unable to register the system lifecycle message window (%1).")
                     .arg(registrationError);
    }
    return registered;
}

}  // namespace

bool SystemLifecycleNotification::isValid() const noexcept
{
    return event != SystemLifecycleEvent::None;
}

LifecycleEventDebouncer::LifecycleEventDebouncer(qint64 intervalMilliseconds) noexcept
    : intervalMilliseconds_(std::max<qint64>(0, intervalMilliseconds))
{
}

qint64 LifecycleEventDebouncer::intervalMilliseconds() const noexcept
{
    return intervalMilliseconds_;
}

bool LifecycleEventDebouncer::enqueue(
    const SystemLifecycleNotification& notification,
    qint64 monotonicMilliseconds) noexcept
{
    if (!notification.isValid() || monotonicMilliseconds < 0) {
        return false;
    }

    const qint64 dueAt = saturatedAdd(monotonicMilliseconds, intervalMilliseconds_);
    const auto existing = std::find_if(
        pending_.begin(),
        pending_.end(),
        [&notification](const PendingNotification& pending) {
            return pending.notification == notification;
        });
    if (existing != pending_.end()) {
        existing->dueAtMilliseconds = dueAt;
        return true;
    }

    if (nextInsertionOrder_ == std::numeric_limits<quint64>::max()) {
        std::sort(
            pending_.begin(),
            pending_.end(),
            [](const PendingNotification& left, const PendingNotification& right) {
                return left.insertionOrder < right.insertionOrder;
            });
        quint64 compactOrder = 0;
        for (PendingNotification& pending : pending_) {
            pending.insertionOrder = compactOrder;
            ++compactOrder;
        }
        nextInsertionOrder_ = compactOrder;
    }

    pending_.append(PendingNotification{
        notification,
        dueAt,
        nextInsertionOrder_,
    });
    ++nextInsertionOrder_;
    return true;
}

QVector<SystemLifecycleNotification> LifecycleEventDebouncer::takeDue(
    qint64 monotonicMilliseconds)
{
    QVector<PendingNotification> due;
    QVector<PendingNotification> remaining;
    due.reserve(pending_.size());
    remaining.reserve(pending_.size());
    for (const PendingNotification& pending : std::as_const(pending_)) {
        if (monotonicMilliseconds >= 0 && pending.dueAtMilliseconds <= monotonicMilliseconds) {
            due.append(pending);
        } else {
            remaining.append(pending);
        }
    }
    pending_ = std::move(remaining);

    std::sort(
        due.begin(),
        due.end(),
        [](const PendingNotification& left, const PendingNotification& right) {
            if (left.dueAtMilliseconds != right.dueAtMilliseconds) {
                return left.dueAtMilliseconds < right.dueAtMilliseconds;
            }
            return left.insertionOrder < right.insertionOrder;
        });

    QVector<SystemLifecycleNotification> notifications;
    notifications.reserve(due.size());
    for (const PendingNotification& pending : std::as_const(due)) {
        notifications.append(pending.notification);
    }
    return notifications;
}

std::optional<qint64> LifecycleEventDebouncer::millisecondsUntilNext(
    qint64 monotonicMilliseconds) const noexcept
{
    if (pending_.isEmpty() || monotonicMilliseconds < 0) {
        return std::nullopt;
    }

    qint64 earliest = std::numeric_limits<qint64>::max();
    for (const PendingNotification& pending : pending_) {
        earliest = std::min(earliest, pending.dueAtMilliseconds);
    }
    if (earliest <= monotonicMilliseconds) {
        return 0;
    }
    return earliest - monotonicMilliseconds;
}

qsizetype LifecycleEventDebouncer::pendingCount() const noexcept
{
    return pending_.size();
}

void LifecycleEventDebouncer::clear() noexcept
{
    pending_.clear();
    nextInsertionOrder_ = 0;
}

qint64 LifecycleEventDebouncer::saturatedAdd(
    qint64 value,
    qint64 nonNegativeDelta) noexcept
{
    const qint64 maximum = std::numeric_limits<qint64>::max();
    if (nonNegativeDelta > maximum - value) {
        return maximum;
    }
    return value + nonNegativeDelta;
}

SystemLifecycleMonitor::SystemLifecycleMonitor(
    qint64 debounceIntervalMilliseconds,
    QObject* parent)
    : QObject(parent)
    , debouncer_(debounceIntervalMilliseconds)
{
    qRegisterMetaType<SystemLifecycleEvent>();
    qRegisterMetaType<SystemLifecycleNotification>();
    debounceTimer_.setSingleShot(true);
    connect(&debounceTimer_, &QTimer::timeout, this, &SystemLifecycleMonitor::flushDebouncedEvents);
}

SystemLifecycleMonitor::~SystemLifecycleMonitor()
{
    stop();
}

bool SystemLifecycleMonitor::start(QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (running_) {
        return true;
    }
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A Qt application is required before monitoring system lifecycle events.");
        }
        return false;
    }
    if (!createMessageWindow(error)) {
        return false;
    }

    application->installNativeEventFilter(this);
    nativeFilterInstalled_ = true;
    monotonicClock_.start();
    running_ = true;
    registerSessionNotifications();
    connectQtDisplaySources();
    return true;
}

void SystemLifecycleMonitor::stop() noexcept
{
    if (!running_ && messageWindow_ == nullptr && !nativeFilterInstalled_) {
        return;
    }

    running_ = false;
    debounceTimer_.stop();
    debouncer_.clear();
    disconnectQtDisplaySources();
    unregisterSessionNotifications();
    if (nativeFilterInstalled_) {
        if (QCoreApplication* application = QCoreApplication::instance(); application != nullptr) {
            application->removeNativeEventFilter(this);
        }
        nativeFilterInstalled_ = false;
    }
    destroyMessageWindow();
    monotonicClock_.invalidate();
}

bool SystemLifecycleMonitor::isRunning() const noexcept
{
    return running_;
}

bool SystemLifecycleMonitor::sessionNotificationsAvailable() const noexcept
{
    return sessionNotificationsRegistered_;
}

qint64 SystemLifecycleMonitor::debounceIntervalMilliseconds() const noexcept
{
    return debouncer_.intervalMilliseconds();
}

bool SystemLifecycleMonitor::nativeEventFilter(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)
    if (!running_ || message == nullptr) {
        return false;
    }

    const auto* nativeMessage = static_cast<const MSG*>(message);
    if (nativeMessage->hwnd == static_cast<HWND>(messageWindow_)) {
        // The hidden window procedure is the guaranteed delivery path for its
        // broadcasts; skipping it here avoids observing the same MSG twice.
        return false;
    }
    (void)processWindowsMessage(
        static_cast<quint32>(nativeMessage->message),
        static_cast<quintptr>(nativeMessage->wParam),
        static_cast<qintptr>(nativeMessage->lParam));
    return false;
}

bool SystemLifecycleMonitor::processWindowsMessage(
    quint32 message,
    quintptr wParam,
    qintptr lParam)
{
    const std::optional<SystemLifecycleNotification> notification =
        classifyWindowsMessage(message, wParam, lParam);
    if (!notification.has_value()) {
        return false;
    }
    queueNotification(*notification);
    return true;
}

std::optional<SystemLifecycleNotification> SystemLifecycleMonitor::classifyWindowsMessage(
    quint32 message,
    quintptr wParam,
    qintptr lParam) noexcept
{
    switch (message) {
    case WM_DISPLAYCHANGE:
        return SystemLifecycleNotification{SystemLifecycleEvent::DisplayTopologyChanged, 0};
    case WM_DPICHANGED:
        return SystemLifecycleNotification{SystemLifecycleEvent::DpiChanged, 0};
    case WM_POWERBROADCAST:
        switch (wParam) {
        case PBT_APMSUSPEND:
            return SystemLifecycleNotification{SystemLifecycleEvent::SystemSuspending, 0};
        case PBT_APMRESUMECRITICAL:
        case PBT_APMRESUMESUSPEND:
        case PBT_APMRESUMEAUTOMATIC:
            return SystemLifecycleNotification{SystemLifecycleEvent::SystemResumed, 0};
        default:
            return std::nullopt;
        }
    case wmWtsSessionChange: {
        const quint32 sessionId = static_cast<quint32>(static_cast<quintptr>(lParam));
        switch (wParam) {
        case wtsConsoleConnect:
            return SystemLifecycleNotification{
                SystemLifecycleEvent::ConsoleSessionConnected,
                sessionId};
        case wtsConsoleDisconnect:
            return SystemLifecycleNotification{
                SystemLifecycleEvent::ConsoleSessionDisconnected,
                sessionId};
        case wtsRemoteConnect:
            return SystemLifecycleNotification{
                SystemLifecycleEvent::RemoteSessionConnected,
                sessionId};
        case wtsRemoteDisconnect:
            return SystemLifecycleNotification{
                SystemLifecycleEvent::RemoteSessionDisconnected,
                sessionId};
        case wtsSessionLogon:
            return SystemLifecycleNotification{SystemLifecycleEvent::SessionLoggedOn, sessionId};
        case wtsSessionLogoff:
            return SystemLifecycleNotification{SystemLifecycleEvent::SessionLoggedOff, sessionId};
        case wtsSessionLock:
            return SystemLifecycleNotification{SystemLifecycleEvent::SessionLocked, sessionId};
        case wtsSessionUnlock:
            return SystemLifecycleNotification{SystemLifecycleEvent::SessionUnlocked, sessionId};
        case wtsRemoteControl:
            return SystemLifecycleNotification{SystemLifecycleEvent::RemoteControlChanged, sessionId};
        default:
            return std::nullopt;
        }
    }
    default:
        return std::nullopt;
    }
}

void SystemLifecycleMonitor::flushDebouncedEvents()
{
    if (!running_ || !monotonicClock_.isValid()) {
        return;
    }
    const QVector<SystemLifecycleNotification> due = debouncer_.takeDue(monotonicClock_.elapsed());
    for (const SystemLifecycleNotification& notification : due) {
        emitNotification(notification);
    }
    armDebounceTimer();
}

void SystemLifecycleMonitor::queueNotification(
    const SystemLifecycleNotification& notification)
{
    if (!running_ || !monotonicClock_.isValid()) {
        return;
    }
    if (debouncer_.enqueue(notification, monotonicClock_.elapsed())) {
        armDebounceTimer();
    }
}

void SystemLifecycleMonitor::armDebounceTimer()
{
    if (!running_ || !monotonicClock_.isValid()) {
        debounceTimer_.stop();
        return;
    }
    const std::optional<qint64> delay = debouncer_.millisecondsUntilNext(monotonicClock_.elapsed());
    if (!delay.has_value()) {
        debounceTimer_.stop();
        return;
    }
    const qint64 boundedDelay = std::min<qint64>(*delay, std::numeric_limits<int>::max());
    debounceTimer_.start(static_cast<int>(boundedDelay));
}

void SystemLifecycleMonitor::emitNotification(
    const SystemLifecycleNotification& notification)
{
    emit lifecycleEventObserved(notification);
    switch (notification.event) {
    case SystemLifecycleEvent::DisplayTopologyChanged:
        emit displayTopologyChanged();
        return;
    case SystemLifecycleEvent::DpiChanged:
        emit dpiChanged();
        return;
    case SystemLifecycleEvent::SessionLocked:
        emit sessionChanged(notification.event, notification.sessionId);
        emit sessionLocked(notification.sessionId);
        return;
    case SystemLifecycleEvent::SessionUnlocked:
        emit sessionChanged(notification.event, notification.sessionId);
        emit sessionUnlocked(notification.sessionId);
        return;
    case SystemLifecycleEvent::SystemSuspending:
        emit systemSuspending();
        return;
    case SystemLifecycleEvent::SystemResumed:
        emit systemResumed();
        return;
    case SystemLifecycleEvent::RemoteSessionConnected:
        emit sessionChanged(notification.event, notification.sessionId);
        emit remoteSessionChanged(true, notification.sessionId);
        return;
    case SystemLifecycleEvent::RemoteSessionDisconnected:
        emit sessionChanged(notification.event, notification.sessionId);
        emit remoteSessionChanged(false, notification.sessionId);
        return;
    case SystemLifecycleEvent::ConsoleSessionConnected:
    case SystemLifecycleEvent::ConsoleSessionDisconnected:
    case SystemLifecycleEvent::SessionLoggedOn:
    case SystemLifecycleEvent::SessionLoggedOff:
    case SystemLifecycleEvent::RemoteControlChanged:
        emit sessionChanged(notification.event, notification.sessionId);
        return;
    case SystemLifecycleEvent::None:
        return;
    }
}

void SystemLifecycleMonitor::connectQtScreenSignals(QScreen* screen)
{
    if (screen == nullptr || connectedScreens_.contains(screen)) {
        return;
    }
    connectedScreens_.insert(screen);

    const auto queueTopology = [this] {
        queueNotification({SystemLifecycleEvent::DisplayTopologyChanged, 0});
    };
    const auto queueDpi = [this] {
        queueNotification({SystemLifecycleEvent::DpiChanged, 0});
    };
    displayConnections_.append(connect(screen, &QScreen::geometryChanged, this, queueTopology));
    displayConnections_.append(connect(screen, &QScreen::availableGeometryChanged, this, queueTopology));
    displayConnections_.append(connect(screen, &QScreen::virtualGeometryChanged, this, queueTopology));
    displayConnections_.append(connect(screen, &QScreen::orientationChanged, this, queueTopology));
    displayConnections_.append(connect(screen, &QScreen::logicalDotsPerInchChanged, this, queueDpi));
    displayConnections_.append(connect(screen, &QScreen::physicalDotsPerInchChanged, this, queueDpi));
    displayConnections_.append(connect(screen, &QObject::destroyed, this, [this, screen] {
        connectedScreens_.remove(screen);
    }));
}

void SystemLifecycleMonitor::connectQtDisplaySources()
{
    auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (application == nullptr) {
        return;
    }

    displayConnections_.append(connect(
        application,
        &QGuiApplication::screenAdded,
        this,
        [this](QScreen* screen) {
            connectQtScreenSignals(screen);
            queueNotification({SystemLifecycleEvent::DisplayTopologyChanged, 0});
        }));
    displayConnections_.append(connect(
        application,
        &QGuiApplication::screenRemoved,
        this,
        [this](QScreen*) {
            queueNotification({SystemLifecycleEvent::DisplayTopologyChanged, 0});
        }));
    displayConnections_.append(connect(
        application,
        &QGuiApplication::primaryScreenChanged,
        this,
        [this](QScreen*) {
            queueNotification({SystemLifecycleEvent::DisplayTopologyChanged, 0});
        }));

    const QList<QScreen*> screens = application->screens();
    for (QScreen* screen : screens) {
        connectQtScreenSignals(screen);
    }
}

void SystemLifecycleMonitor::disconnectQtDisplaySources() noexcept
{
    for (const QMetaObject::Connection& connection : std::as_const(displayConnections_)) {
        QObject::disconnect(connection);
    }
    displayConnections_.clear();
    connectedScreens_.clear();
}

bool SystemLifecycleMonitor::createMessageWindow(QString* error)
{
    if (!ensureLifecycleWindowClass(error)) {
        return false;
    }
    const HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        lifecycleWindowClassName,
        L"SnapAsk lifecycle events",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    if (window == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the system lifecycle message window (%1).")
                         .arg(GetLastError());
        }
        return false;
    }
    messageWindow_ = window;
    return true;
}

void SystemLifecycleMonitor::destroyMessageWindow() noexcept
{
    if (messageWindow_ == nullptr) {
        return;
    }
    DestroyWindow(static_cast<HWND>(messageWindow_));
    messageWindow_ = nullptr;
}

void SystemLifecycleMonitor::registerSessionNotifications() noexcept
{
    if (messageWindow_ == nullptr || sessionNotificationsRegistered_) {
        return;
    }
    HMODULE module = LoadLibraryW(L"wtsapi32.dll");
    if (module == nullptr) {
        return;
    }
    const auto registerNotification = reinterpret_cast<RegisterSessionNotification>(
        GetProcAddress(module, "WTSRegisterSessionNotification"));
    if (registerNotification == nullptr
        || registerNotification(static_cast<HWND>(messageWindow_), notifyForThisSession) == FALSE) {
        FreeLibrary(module);
        return;
    }
    wtsApiModule_ = module;
    sessionNotificationsRegistered_ = true;
}

void SystemLifecycleMonitor::unregisterSessionNotifications() noexcept
{
    HMODULE module = static_cast<HMODULE>(wtsApiModule_);
    if (module != nullptr) {
        if (sessionNotificationsRegistered_ && messageWindow_ != nullptr) {
            const auto unregisterNotification = reinterpret_cast<UnregisterSessionNotification>(
                GetProcAddress(module, "WTSUnRegisterSessionNotification"));
            if (unregisterNotification != nullptr) {
                (void)unregisterNotification(static_cast<HWND>(messageWindow_));
            }
        }
        FreeLibrary(module);
    }
    wtsApiModule_ = nullptr;
    sessionNotificationsRegistered_ = false;
}

}  // namespace snapask::platform::windows
