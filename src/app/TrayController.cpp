#include "app/TrayController.h"

#include <QAction>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace snapask::app {

TrayController::TrayController(QObject* parent) : QObject(parent) {}

TrayController::~TrayController() {
    if (trayIcon_ != nullptr) {
        trayIcon_->hide();
    }
}

bool TrayController::start() {
    if (trayIcon_ != nullptr) {
        return true;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return false;
    }

    menu_ = new QMenu();
    menu_->setObjectName(QStringLiteral("TrayMenu"));

    QAction* captureAction = menu_->addAction(tr("截图提问(&C)"));
    captureAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Space")));
    QAction* clipboardAction = menu_->addAction(tr("打开剪贴板图片(&P)"));
    menu_->addSeparator();
    QAction* settingsAction = menu_->addAction(tr("设置(&S)…"));
    menu_->addSeparator();
    QAction* exitAction = menu_->addAction(tr("退出(&X)"));

    trayIcon_ = new QSystemTrayIcon(createApplicationIcon(), this);
    trayIcon_->setToolTip(tr("SnapAsk"));
    trayIcon_->setContextMenu(menu_);

    connect(captureAction, &QAction::triggered, this, &TrayController::captureRequested);
    connect(clipboardAction, &QAction::triggered, this, &TrayController::clipboardImageRequested);
    connect(settingsAction, &QAction::triggered, this, &TrayController::settingsRequested);
    connect(exitAction, &QAction::triggered, this, &TrayController::exitRequested);
    connect(trayIcon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
            emit captureRequested();
        }
    });

    trayIcon_->show();
    return true;
}

void TrayController::showMessage(const QString& title, const QString& message) {
    if (trayIcon_ != nullptr) {
        trayIcon_->showMessage(title, message, QSystemTrayIcon::Information, 4000);
    }
}

QIcon TrayController::createApplicationIcon() const {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 122, 255));
    painter.drawRoundedRect(QRectF(4.0, 4.0, 56.0, 56.0), 15.0, 15.0);

    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(39);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("S"));

    return QIcon(pixmap);
}

}  // namespace snapask::app
