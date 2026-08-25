#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

namespace snapask::app {

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    enum class StartResult {
        Primary,
        SecondaryNotified,
        Error,
    };

    explicit SingleInstance(QString serverName, QObject* parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    StartResult start();
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] bool isPrimary() const noexcept;

signals:
    void activationRequested();

private slots:
    void acceptPendingConnections();

private:
    bool notifyExistingInstance();
    void attachConnection(class QLocalSocket* socket);

    QString serverName_;
    QString errorString_;
    QLocalServer server_;
    void* mutexHandle_ = nullptr;
    bool started_ = false;
    bool primary_ = false;
};

}  // namespace snapask::app
