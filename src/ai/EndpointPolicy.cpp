#include "ai/EndpointPolicy.h"

#include <QHostAddress>

namespace snapask::ai {
namespace {

int effectivePort(const QUrl& url)
{
    if (url.port() >= 0) {
        return url.port();
    }
    return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
}

} // namespace

EndpointPolicy::Result EndpointPolicy::validateBaseUrl(const QUrl& url)
{
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()) {
        return {false, QStringLiteral("服务地址无效")};
    }
    if (!url.userInfo().isEmpty()) {
        return {false, QStringLiteral("服务地址不能包含用户名或密码")};
    }
    if (!url.query().isEmpty() || url.hasFragment()) {
        return {false, QStringLiteral("服务基础地址不能包含查询参数或片段")};
    }
    const auto scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("https")
        && !(scheme == QStringLiteral("http") && isLoopbackHost(url.host()))) {
        return {false, QStringLiteral("公网服务必须使用 HTTPS")};
    }
    return {true, {}};
}

bool EndpointPolicy::isLoopbackHost(const QString& host)
{
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    QHostAddress address;
    return address.setAddress(host) && address.isLoopback();
}

bool EndpointPolicy::isSameOrigin(const QUrl& first, const QUrl& second)
{
    return first.scheme().compare(second.scheme(), Qt::CaseInsensitive) == 0
        && first.host().compare(second.host(), Qt::CaseInsensitive) == 0
        && effectivePort(first) == effectivePort(second);
}

QUrl EndpointPolicy::endpoint(const QUrl& baseUrl, const QString& suffix)
{
    QUrl result = baseUrl;
    QString path = result.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    QString normalizedSuffix = suffix;
    if (!normalizedSuffix.startsWith('/')) {
        normalizedSuffix.prepend('/');
    }
    result.setPath(path + normalizedSuffix);
    return result;
}

} // namespace snapask::ai
