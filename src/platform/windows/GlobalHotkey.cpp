#include "platform/windows/GlobalHotkey.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QCoreApplication>

#include <atomic>

namespace snapask::platform::windows {

namespace {

constexpr UINT noRepeatModifier = 0x4000;
std::atomic<int> nextHotkeyId{0x5300};

[[nodiscard]] int allocateHotkeyId() noexcept
{
    // RegisterHotKey reserves 0x0000..0xBFFF for application IDs.
    const int id = nextHotkeyId.fetch_add(1, std::memory_order_relaxed);
    return id <= 0xBFFF ? id : 0x5300 + (id % 0x6000);
}

[[nodiscard]] QString windowsError(DWORD errorCode)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    const QString result = length > 0 && message != nullptr
        ? QString::fromWCharArray(message, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Unknown Windows error");
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}

}  // namespace

GlobalHotkey::GlobalHotkey(QObject* parent)
    : QObject(parent)
    , nativeId_(allocateHotkeyId())
{
}

GlobalHotkey::~GlobalHotkey()
{
    unregisterHotkey();
    if (eventFilterInstalled_) {
        if (QCoreApplication* application = QCoreApplication::instance(); application != nullptr) {
            application->removeNativeEventFilter(this);
        }
    }
}

bool GlobalHotkey::registerHotkey(const HotkeyChord& chord, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!chord.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("The global shortcut has no virtual key.");
        }
        return false;
    }
    if (!ensureEventFilter(error)) {
        return false;
    }
    if (registered_ && chord.virtualKey == chord_.virtualKey && chord.modifiers == chord_.modifiers) {
        return true;
    }

    unregisterHotkey();
    SetLastError(ERROR_SUCCESS);
    const UINT modifiers = static_cast<UINT>(chord.modifiers) | noRepeatModifier;
    if (RegisterHotKey(nullptr, nativeId_, modifiers, static_cast<UINT>(chord.virtualKey)) == FALSE) {
        const DWORD code = GetLastError();
        if (error != nullptr) {
            *error = QStringLiteral("RegisterHotKey failed (%1): %2")
                         .arg(code)
                         .arg(windowsError(code));
        }
        return false;
    }

    chord_ = chord;
    registered_ = true;
    return true;
}

void GlobalHotkey::unregisterHotkey() noexcept
{
    if (!registered_) {
        return;
    }
    UnregisterHotKey(nullptr, nativeId_);
    registered_ = false;
    chord_ = {};
}

bool GlobalHotkey::isRegistered() const noexcept
{
    return registered_;
}

HotkeyChord GlobalHotkey::chord() const noexcept
{
    return chord_;
}

int GlobalHotkey::nativeId() const noexcept
{
    return nativeId_;
}

bool GlobalHotkey::nativeEventFilter(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
    Q_UNUSED(eventType)
    if (!registered_ || message == nullptr) {
        return false;
    }

    const auto* nativeMessage = static_cast<const MSG*>(message);
    if (nativeMessage->message != WM_HOTKEY
        || static_cast<int>(nativeMessage->wParam) != nativeId_) {
        return false;
    }

    if (result != nullptr) {
        *result = 0;
    }
    emit activated();
    return true;
}

bool GlobalHotkey::ensureEventFilter(QString* error)
{
    if (eventFilterInstalled_) {
        return true;
    }
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A QCoreApplication is required before registering a global shortcut.");
        }
        return false;
    }

    application->installNativeEventFilter(this);
    eventFilterInstalled_ = true;
    return true;
}

}  // namespace snapask::platform::windows
