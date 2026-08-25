#include "infrastructure/EndpointConsentStore.h"

#include <QSettings>

#include <algorithm>
#include <utility>

namespace snapask::infrastructure {
namespace {

constexpr auto kApprovedOriginsKey = "privacy/approvedCustomOrigins";

int effectivePort(const QUrl& url)
{
    if (url.port() >= 0) return url.port();
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
        return 443;
    }
    if (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0) {
        return 80;
    }
    return -1;
}

}  // namespace

EndpointConsentStore::EndpointConsentStore(QString settingsFilePath)
{
    if (settingsFilePath.isEmpty()) {
        settings_ = std::make_unique<QSettings>();
    } else {
        settings_ = std::make_unique<QSettings>(
            std::move(settingsFilePath), QSettings::IniFormat);
    }
}

EndpointConsentStore::~EndpointConsentStore() = default;

QString EndpointConsentStore::normalizedOrigin(const QUrl& baseUrl)
{
    if (!baseUrl.isValid() || baseUrl.isRelative() || baseUrl.host().isEmpty()
        || !baseUrl.userInfo().isEmpty()) {
        return {};
    }
    const QString scheme = baseUrl.scheme().toLower();
    if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")) {
        return {};
    }
    const int port = effectivePort(baseUrl);
    if (port <= 0 || port > 65'535) return {};

    QString host = QString::fromLatin1(QUrl::toAce(baseUrl.host().toLower()));
    if (host.isEmpty()) return {};
    if (host.contains(QLatin1Char(':'))) {
        host = QLatin1Char('[') + host + QLatin1Char(']');
    }
    return QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);
}

bool EndpointConsentStore::requiresConsent(const QUrl& baseUrl)
{
    const QString origin = normalizedOrigin(baseUrl);
    return !origin.isEmpty()
        && origin != QStringLiteral("https://api.openai.com:443");
}

bool EndpointConsentStore::isApproved(const QUrl& baseUrl) const
{
    const QString origin = normalizedOrigin(baseUrl);
    if (origin.isEmpty()) return false;
    if (!requiresConsent(baseUrl)) return true;
    return approvedOrigins().contains(origin);
}

bool EndpointConsentStore::approve(const QUrl& baseUrl, QString* error)
{
    const QString origin = normalizedOrigin(baseUrl);
    if (origin.isEmpty()) {
        if (error) *error = QStringLiteral("服务目标域名无效");
        return false;
    }
    if (!requiresConsent(baseUrl)) return true;

    QStringList origins = approvedOrigins();
    if (!origins.contains(origin)) {
        origins.append(origin);
        std::sort(origins.begin(), origins.end());
        settings_->setValue(QString::fromLatin1(kApprovedOriginsKey), origins);
        settings_->sync();
    }
    if (settings_->status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("无法保存自定义服务授权状态");
        return false;
    }
    return true;
}

QStringList EndpointConsentStore::approvedOrigins() const
{
    QStringList origins = settings_->value(
        QString::fromLatin1(kApprovedOriginsKey)).toStringList();
    origins.removeDuplicates();
    origins.removeIf([](const QString& origin) { return origin.isEmpty(); });
    return origins;
}

}  // namespace snapask::infrastructure
