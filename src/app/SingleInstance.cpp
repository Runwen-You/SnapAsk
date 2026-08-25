#include "app/SingleInstance.h"

#include <QIODevice>
#include <QLocalSocket>
#include <QMetaObject>
#include <QVariant>

#include <Windows.h>

#include <utility>

namespace snapask::app {
namespace {

constexpr auto kActivationMessage = "activate\n";
constexpr auto kAcknowledgementMessage = "activated\n";
constexpr int kConnectionTimeoutMs = 750;

}  // namespace

SingleInstance::SingleInstance(QString serverName, QObject* parent)
    : QObject(parent), serverName_(std::move(serverName)) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, &SingleInstance::acceptPendingConnections);
}

SingleInstance::~SingleInstance() {
    if (primary_) {
        server_.close();
        QLocalServer::removeServer(serverName_);
    }

    if (mutexHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mutexHandle_));
        mutexHandle_ = nullptr;
    }
}

SingleInstance::StartResult SingleInstance::start() {
    if (started_) {
        return primary_ ? StartResult::Primary : StartResult::Error;
    }
    started_ = true;

    const QString mutexName = QStringLiteral("Local\\") + serverName_;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
    // GetLastError is only defined for this decision immediately after
    // CreateMutexW. Qt/CRT calls made later are allowed to overwrite it.
    const DWORD createMutexResult = GetLastError();
    if (mutex == nullptr) {
        errorString_ = QStringLiteral("CreateMutexW failed with Windows error %1.").arg(createMutexResult);
        return StartResult::Error;
    }
    mutexHandle_ = mutex;

    if (createMutexResult == ERROR_ALREADY_EXISTS) {
        if (notifyExistingInstance()) {
            return StartResult::SecondaryNotified;
        }

        errorString_ = QStringLiteral("The primary instance exists but could not be activated.");
        return StartResult::Error;
    }

    QLocalServer::removeServer(serverName_);
    if (server_.listen(serverName_)) {
        primary_ = true;
        return StartResult::Primary;
    }

    errorString_ = server_.errorString();
    return StartResult::Error;
}

QString SingleInstance::errorString() const {
    return errorString_;
}

bool SingleInstance::isPrimary() const noexcept {
    return primary_;
}

bool SingleInstance::notifyExistingInstance() {
    QLocalSocket socket;
    socket.connectToServer(serverName_, QIODevice::ReadWrite);
    if (!socket.waitForConnected(kConnectionTimeoutMs)) {
        return false;
    }

    if (socket.write(kActivationMessage) < 0) {
        return false;
    }
    // A local socket can synchronously flush this tiny message. In that case
    // waitForBytesWritten() correctly returns false because nothing remains
    // queued, which is still a successful notification.
    if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(kConnectionTimeoutMs)) {
        return false;
    }

    if (!socket.waitForReadyRead(kConnectionTimeoutMs)
        || !socket.readAll().contains(kAcknowledgementMessage)) {
        return false;
    }

    socket.disconnectFromServer();
    return true;
}

void SingleInstance::acceptPendingConnections() {
    while (server_.hasPendingConnections()) {
        attachConnection(server_.nextPendingConnection());
    }
}

void SingleInstance::attachConnection(QLocalSocket* socket) {
    if (socket == nullptr) {
        return;
    }

    const auto readActivation = [this, socket]() {
        const QByteArray payload = socket->readAll();
        if (socket->property("activationHandled").toBool()) {
            return;
        }

        if (payload.contains(kActivationMessage)) {
            socket->setProperty("activationHandled", true);
            socket->write(kAcknowledgementMessage);
            socket->flush();
            // A full settings window can take longer than the secondary
            // instance's acknowledgement timeout to construct. Acknowledge
            // first and activate on the next event-loop turn so a successful
            // double launch never reports a false startup failure.
            QMetaObject::invokeMethod(
                this,
                [this] { emit activationRequested(); },
                Qt::QueuedConnection);
        }
    };

    connect(socket, &QLocalSocket::readyRead, this, readActivation);
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);

    if (socket->bytesAvailable() > 0) {
        readActivation();
    }
}

}  // namespace snapask::app
