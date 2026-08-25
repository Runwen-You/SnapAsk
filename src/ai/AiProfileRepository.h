#pragma once

#include "ai/AiTypes.h"

#include <QList>
#include <QReadWriteLock>
#include <optional>

namespace snapask::ai {

class AiProfileRepository final {
public:
    explicit AiProfileRepository(QString filePath);

    bool load(QString* error = nullptr);
    bool save(QString* error = nullptr) const;

    [[nodiscard]] QList<ProviderProfile> profiles() const;
    [[nodiscard]] std::optional<ProviderProfile> profile(const QUuid& id) const;
    [[nodiscard]] std::optional<ProviderProfile> defaultProfile() const;
    [[nodiscard]] QUuid defaultProfileId() const;

    bool upsert(ProviderProfile profile, QString* error = nullptr);
    bool remove(const QUuid& id, QString* error = nullptr);
    std::optional<ProviderProfile> duplicate(const QUuid& id, QString* error = nullptr);
    bool setDefault(const QUuid& id, QString* error = nullptr);

    [[nodiscard]] QJsonObject exportConfiguration() const;
    static bool customHeadersAreSafe(const QJsonObject& headers, QString* error = nullptr);

private:
    static QJsonObject toJson(const ProviderProfile& profile);
    static std::optional<ProviderProfile> fromJson(const QJsonObject& object, QString* error);

    QString filePath_;
    mutable QReadWriteLock lock_;
    QList<ProviderProfile> profiles_;
    QUuid defaultProfileId_;
};

} // namespace snapask::ai

