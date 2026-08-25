#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

namespace snapask::platform::windows {

enum HotkeyModifier : quint32 {
    HotkeyAlt = 0x0001,
    HotkeyControl = 0x0002,
    HotkeyShift = 0x0004,
    HotkeyWindows = 0x0008,
};

struct HotkeyChord final {
    quint32 virtualKey{0};
    quint32 modifiers{0};

    [[nodiscard]] bool isValid() const noexcept { return virtualKey != 0; }
};

class GlobalHotkey final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject* parent = nullptr);
    ~GlobalHotkey() override;

    GlobalHotkey(const GlobalHotkey&) = delete;
    GlobalHotkey& operator=(const GlobalHotkey&) = delete;

    [[nodiscard]] bool registerHotkey(const HotkeyChord& chord, QString* error = nullptr);
    void unregisterHotkey() noexcept;

    [[nodiscard]] bool isRegistered() const noexcept;
    [[nodiscard]] HotkeyChord chord() const noexcept;
    [[nodiscard]] int nativeId() const noexcept;

    bool nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        qintptr* result) override;

signals:
    void activated();

private:
    [[nodiscard]] bool ensureEventFilter(QString* error);

    int nativeId_{0};
    HotkeyChord chord_;
    bool registered_{false};
    bool eventFilterInstalled_{false};
};

}  // namespace snapask::platform::windows
