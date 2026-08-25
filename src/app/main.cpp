#include "SnapAskVersion.h"
#include "app/AppController.h"
#include "app/SingleInstance.h"
#include "infrastructure/RedactingLogger.h"

#include <QApplication>
#include <QMessageBox>

namespace {

constexpr auto kSingleInstanceName = "SnapAsk.Application.13f54c32-060f-49a3-98ec-b3eae4fa906d";

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setOrganizationName(QStringLiteral("SnapAsk"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("snapask.local"));
    QCoreApplication::setApplicationName(QStringLiteral("SnapAsk"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SNAPASK_VERSION_STRING));

    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    snapask::infrastructure::RedactingLogger::install();

    snapask::app::SingleInstance singleInstance(QString::fromLatin1(kSingleInstanceName));
    const auto instanceResult = singleInstance.start();
    if (instanceResult == snapask::app::SingleInstance::StartResult::SecondaryNotified) {
        snapask::infrastructure::RedactingLogger::uninstall();
        return 0;
    }
    if (instanceResult == snapask::app::SingleInstance::StartResult::Error) {
        qCritical().noquote() << "Single-instance startup failed:" << singleInstance.errorString();
        QMessageBox::critical(
            nullptr,
            QObject::tr("SnapAsk 无法启动"),
            QObject::tr("无法建立单实例通信。请关闭其他 SnapAsk 进程后重试。"));
        snapask::infrastructure::RedactingLogger::uninstall();
        return 1;
    }

    snapask::app::AppController controller;
    QObject::connect(
        &singleInstance,
        &snapask::app::SingleInstance::activationRequested,
        &controller,
        &snapask::app::AppController::activate);

    if (!controller.start()) {
        snapask::infrastructure::RedactingLogger::uninstall();
        return 1;
    }

    const int exitCode = application.exec();
    snapask::infrastructure::RedactingLogger::uninstall();
    return exitCode;
}

