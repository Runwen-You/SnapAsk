#pragma once

#include <QString>
#include <QUrl>

namespace snapask::ai {

class EndpointPolicy final {
public:
    struct Result {
        bool accepted = false;
        QString error;
    };

    static Result validateBaseUrl(const QUrl& url);
    static bool isLoopbackHost(const QString& host);
    static bool isSameOrigin(const QUrl& first, const QUrl& second);
    static QUrl endpoint(const QUrl& baseUrl, const QString& suffix);
};

} // namespace snapask::ai

