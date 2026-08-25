#include "infrastructure/RedactingLogger.h"

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QVector>

#include <cstdio>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace snapask::infrastructure {
namespace {

struct CaptureRange {
    qsizetype start = -1;
    qsizetype length = 0;
};

QMutex gHandlerMutex;
QtMessageHandler gPreviousHandler = nullptr;
bool gInstalled = false;

void redactCapturedValues(QString& text, const QRegularExpression& expression) {
    QVector<CaptureRange> ranges;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const qsizetype start = match.capturedStart(QStringLiteral("value"));
        const qsizetype length = match.capturedLength(QStringLiteral("value"));
        if (start >= 0 && length > 0) {
            ranges.push_back({start, length});
        }
    }

    for (auto iterator = ranges.crbegin(); iterator != ranges.crend(); ++iterator) {
        text.replace(iterator->start, iterator->length, QStringLiteral("<redacted>"));
    }
}

void writeDefaultMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QString formatted = qFormatLogMessage(type, context, message);

#ifdef Q_OS_WIN
    const QString debuggerLine = formatted + QStringLiteral("\r\n");
    OutputDebugStringW(reinterpret_cast<LPCWSTR>(debuggerLine.utf16()));
#endif

    const QByteArray bytes = formatted.toLocal8Bit();
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stderr);
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

}  // namespace

void RedactingLogger::install() {
    QMutexLocker lock(&gHandlerMutex);
    if (gInstalled) {
        return;
    }

    qSetMessagePattern(QStringLiteral("[%{time yyyy-MM-dd hh:mm:ss.zzz}] [%{type}] %{category}: %{message}"));
    gPreviousHandler = qInstallMessageHandler(&RedactingLogger::handleMessage);
    gInstalled = true;
}

void RedactingLogger::uninstall() {
    QMutexLocker lock(&gHandlerMutex);
    if (!gInstalled) {
        return;
    }

    qInstallMessageHandler(gPreviousHandler);
    gPreviousHandler = nullptr;
    gInstalled = false;
}

QString RedactingLogger::redact(QStringView message) {
    QString result = message.toString();

    static const QRegularExpression dataImagePattern(
        QStringLiteral(R"((?i)data:image/[a-z0-9.+-]+;base64,(?<value>[a-z0-9+/_=-]+))"));
    static const QRegularExpression authorizationPattern(
        QStringLiteral(R"((?i)(?:["']?authorization["']?\s*[:=]\s*["']?)(?<value>(?:bearer\s+)?[^"'\r\n,;}\]]+))"));
    static const QRegularExpression secretFieldPattern(
        QStringLiteral(R"((?i)(?:["']?(?:api[_-]?key|access[_-]?token|secret|password)["']?\s*[:=]\s*["']?)(?<value>[^"'\s,;}\]]+))"));
    static const QRegularExpression querySecretPattern(
        QStringLiteral(R"((?i)(?:[?&](?:api[_-]?key|access[_-]?token|token)=)(?<value>[^&\s]+))"));
    static const QRegularExpression structuredContentPattern(
        QStringLiteral(R"regex((?i)"(?:question|prompt|answer|request[_-]?body|response[_-]?body)"\s*:\s*"(?<value>(?:\\.|[^"\\])*)")regex"));
    static const QRegularExpression labelledContentPattern(
        QStringLiteral(R"((?i)\b(?:question|prompt|answer|request\s+body|response\s+body)\s*[:=]\s*(?<value>[^\r\n]+))"));
    static const QRegularExpression knownTokenPattern(
        QStringLiteral(R"((?i)(?<value>\b(?:sk|sess)-[a-z0-9_-]{8,}\b))"));
    static const QRegularExpression longEncodedValuePattern(
        QStringLiteral(R"((?<value>(?<![A-Za-z0-9+/_=-])[A-Za-z0-9+/_-]{128,}={0,2}(?![A-Za-z0-9+/_=-])))"));

    redactCapturedValues(result, dataImagePattern);
    redactCapturedValues(result, authorizationPattern);
    redactCapturedValues(result, secretFieldPattern);
    redactCapturedValues(result, querySecretPattern);
    redactCapturedValues(result, structuredContentPattern);
    redactCapturedValues(result, labelledContentPattern);
    redactCapturedValues(result, knownTokenPattern);
    redactCapturedValues(result, longEncodedValuePattern);

    return result;
}

void RedactingLogger::handleMessage(
    QtMsgType type,
    const QMessageLogContext& context,
    const QString& message) {
    const QString safeMessage = redact(message);

    QtMessageHandler previousHandler = nullptr;
    {
        QMutexLocker lock(&gHandlerMutex);
        previousHandler = gPreviousHandler;
    }

    if (previousHandler != nullptr) {
        previousHandler(type, context, safeMessage);
        return;
    }

    writeDefaultMessage(type, context, safeMessage);
}

}  // namespace snapask::infrastructure
