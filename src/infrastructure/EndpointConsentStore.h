#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

class QSettings;

namespace snapask::infrastructure {

// Persists only the normalized network origin that the user has approved.
// It never stores credentials, request content, screenshots, or response data.
class EndpointConsentStore final {
public:
    explicit EndpointConsentStore(QString settingsFilePath = {});
    ~EndpointConsentStore();

    EndpointConsentStore(const EndpointConsentStore&) = delete;
    EndpointConsentStore& operator=(const EndpointConsentStore&) = delete;

    [[nodiscard]] static QString normalizedOrigin(const QUrl& baseUrl);
    [[nodiscard]] static bool requiresConsent(const QUrl& baseUrl);
    [[nodiscard]] bool isApproved(const QUrl& baseUrl) const;
    bool approve(const QUrl& baseUrl, QString* error = nullptr);
    [[nodiscard]] QStringList approvedOrigins() const;

private:
    std::unique_ptr<QSettings> settings_;
};

}  // namespace snapask::infrastructure
