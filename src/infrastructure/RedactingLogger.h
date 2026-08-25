#pragma once

#include <QString>
#include <QStringView>
#include <QtMessageHandler>

namespace snapask::infrastructure {

class RedactingLogger final {
public:
    RedactingLogger() = delete;

    static void install();
    static void uninstall();
    [[nodiscard]] static QString redact(QStringView message);

private:
    static void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message);
};

}  // namespace snapask::infrastructure

