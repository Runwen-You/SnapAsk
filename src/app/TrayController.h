#pragma once

#include <QObject>

class QMenu;
class QSystemTrayIcon;

namespace snapask::app {

class TrayController final : public QObject {
    Q_OBJECT

public:
    explicit TrayController(QObject* parent = nullptr);
    ~TrayController() override;

    [[nodiscard]] bool start();
    void showMessage(const QString& title, const QString& message);

signals:
    void captureRequested();
    void clipboardImageRequested();
    void settingsRequested();
    void exitRequested();

private:
    class QIcon createApplicationIcon() const;

    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* menu_ = nullptr;
};

}  // namespace snapask::app
