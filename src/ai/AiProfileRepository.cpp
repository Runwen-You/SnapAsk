#include "ai/AiProfileRepository.h"

#include "ai/EndpointPolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <cmath>
#include <utility>

namespace snapask::ai {
namespace {

constexpr int kLegacyConfigVersion = 0;

QString protocolToJson(const Protocol protocol)
{
    return protocol == Protocol::OpenAIResponses
        ? QStringLiteral("responses") : QStringLiteral("chat-completions");
}

std::optional<Protocol> protocolFromJson(const QString& value)
{
    if (value == QStringLiteral("responses")) return Protocol::OpenAIResponses;
    if (value == QStringLiteral("chat-completions")) return Protocol::ChatCompletions;
    return std::nullopt;
}

QJsonArray stringListToJson(const QStringList& values)
{
    QJsonArray result;
    for (const auto& value : values) result.append(value);
    return result;
}

QStringList stringListFromJson(const QJsonArray& values)
{
    QStringList result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (value.isString()) result.append(value.toString());
    }
    return result;
}

bool credentialReferenceMatches(const QString& reference, const QUuid& profileId)
{
    const QString expected = QStringLiteral("SnapAsk/provider/")
        + profileId.toString(QUuid::WithoutBraces);
    return reference == expected;
}

bool proxyUrlIsSafe(const QUrl& proxy)
{
    if (proxy.isEmpty()) return true;
    const QString scheme = proxy.scheme().toLower();
    return proxy.isValid() && !proxy.isRelative() && !proxy.host().isEmpty()
        && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
        && proxy.userInfo().isEmpty() && proxy.query().isEmpty()
        && !proxy.hasFragment()
        && (proxy.path().isEmpty() || proxy.path() == QStringLiteral("/"));
}

bool containsPlaintextCredentialField(const QJsonValue& value)
{
    static const QSet<QString> names{
        QStringLiteral("key"), QStringLiteral("token"),
        QStringLiteral("secret"),
        QStringLiteral("secrets"), QStringLiteral("password"),
        QStringLiteral("passwd"),
        QStringLiteral("credential"), QStringLiteral("credentials"),
    };
    static const QStringList credentialNameFragments{
        QStringLiteral("apikey"), QStringLiteral("authorization"),
        QStringLiteral("bearertoken"), QStringLiteral("accesstoken"),
        QStringLiteral("refreshtoken"), QStringLiteral("authtoken"),
        QStringLiteral("clientsecret"), QStringLiteral("privatekey"),
        QStringLiteral("subscriptionkey"), QStringLiteral("credential"),
    };

    if (value.isArray()) {
        for (const auto& item : value.toArray()) {
            if (containsPlaintextCredentialField(item)) return true;
        }
        return false;
    }
    if (!value.isObject()) return false;

    const auto object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        QString normalizedName = it.key().toLower();
        normalizedName.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
        bool credentialName = names.contains(normalizedName);
        for (const auto& fragment : credentialNameFragments) {
            if (normalizedName.contains(fragment)) {
                credentialName = true;
                break;
            }
        }
        // credentialRef is the one permitted reference-only field. Its value
        // is validated against the profile UUID before entering memory.
        if (normalizedName == QStringLiteral("credentialref")) {
            credentialName = false;
        }
        if (credentialName || containsPlaintextCredentialField(it.value())) {
            return true;
        }
    }
    return false;
}

std::optional<int> readConfigVersion(const QJsonObject& root, QString* error)
{
    const auto value = root.value(QStringLiteral("version"));
    if (value.isUndefined()) return kLegacyConfigVersion;
    if (!value.isDouble()) {
        if (error) *error = QStringLiteral("服务配置版本格式无效");
        return std::nullopt;
    }

    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < kLegacyConfigVersion
        || number > kProviderConfigurationSchemaVersion) {
        if (error) *error = QStringLiteral("服务配置版本不受支持");
        return std::nullopt;
    }
    return static_cast<int>(number);
}

QJsonObject migrateLegacyProfile(QJsonObject profile)
{
    if (profile.contains(QStringLiteral("credentialRef"))) return profile;

    const QUuid profileId(profile.value(QStringLiteral("id")).toString());
    if (!profileId.isNull()) {
        profile.insert(
            QStringLiteral("credentialRef"),
            QStringLiteral("SnapAsk/provider/")
                + profileId.toString(QUuid::WithoutBraces));
    }
    return profile;
}

QJsonObject migrateConfiguration(QJsonObject root, const int sourceVersion)
{
    if (sourceVersion != kLegacyConfigVersion) return root;

    QJsonArray migratedProfiles;
    const auto sourceProfiles = root.value(QStringLiteral("profiles")).toArray();
    for (const auto& value : sourceProfiles) {
        migratedProfiles.append(value.isObject()
            ? QJsonValue(migrateLegacyProfile(value.toObject())) : value);
    }
    root.insert(QStringLiteral("profiles"), migratedProfiles);
    root.insert(
        QStringLiteral("version"), kProviderConfigurationSchemaVersion);
    return root;
}

} // namespace

AiProfileRepository::AiProfileRepository(QString filePath)
    : filePath_(std::move(filePath))
{
}

bool AiProfileRepository::load(QString* error)
{
    QFile file(filePath_);
    if (!file.exists()) {
        QWriteLocker locker(&lock_);
        profiles_.clear();
        defaultProfileId_ = {};
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取服务配置");
        return false;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("服务配置格式无效");
        return false;
    }
    const auto sourceRoot = document.object();
    if (containsPlaintextCredentialField(sourceRoot)) {
        if (error) *error = QStringLiteral("服务配置含有明文凭据，已拒绝读取");
        return false;
    }
    const auto version = readConfigVersion(sourceRoot, error);
    if (!version.has_value()) return false;
    const auto root = migrateConfiguration(sourceRoot, *version);

    QList<ProviderProfile> loaded;
    QSet<QUuid> ids;
    for (const auto& value : root.value(QStringLiteral("profiles")).toArray()) {
        if (!value.isObject()) continue;
        QString itemError;
        auto parsed = fromJson(value.toObject(), &itemError);
        if (!parsed) {
            if (error) *error = itemError;
            return false;
        }
        if (ids.contains(parsed->id)) {
            if (error) *error = QStringLiteral("服务配置包含重复 ID");
            return false;
        }
        ids.insert(parsed->id);
        loaded.append(*parsed);
    }
    const QUuid loadedDefault(root.value(QStringLiteral("defaultProfileId")).toString());

    QWriteLocker locker(&lock_);
    profiles_ = std::move(loaded);
    defaultProfileId_ = ids.contains(loadedDefault)
        ? loadedDefault : (profiles_.isEmpty() ? QUuid{} : profiles_.first().id);
    return true;
}

bool AiProfileRepository::save(QString* error) const
{
    const auto info = QFileInfo(filePath_);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = QStringLiteral("无法创建配置目录");
        return false;
    }

    QSaveFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入服务配置");
        return false;
    }
    const auto bytes = QJsonDocument(exportConfiguration()).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法原子保存服务配置");
        return false;
    }
    return true;
}

QList<ProviderProfile> AiProfileRepository::profiles() const
{
    QReadLocker locker(&lock_);
    return profiles_;
}

std::optional<ProviderProfile> AiProfileRepository::profile(const QUuid& id) const
{
    QReadLocker locker(&lock_);
    for (const auto& item : profiles_) if (item.id == id) return item;
    return std::nullopt;
}

std::optional<ProviderProfile> AiProfileRepository::defaultProfile() const
{
    return profile(defaultProfileId());
}

QUuid AiProfileRepository::defaultProfileId() const
{
    QReadLocker locker(&lock_);
    return defaultProfileId_;
}

bool AiProfileRepository::upsert(ProviderProfile profile, QString* error)
{
    if (profile.id.isNull()) profile.id = QUuid::createUuid();
    if (profile.displayName.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("服务名称不能为空");
        return false;
    }
    const auto endpointResult = EndpointPolicy::validateBaseUrl(profile.baseUrl);
    if (!endpointResult.accepted) {
        if (error) *error = endpointResult.error;
        return false;
    }
    if (!customHeadersAreSafe(profile.customHeaders, error)) return false;
    if (profile.credentialRef.isEmpty()) {
        profile.credentialRef = QStringLiteral("SnapAsk/provider/")
            + profile.id.toString(QUuid::WithoutBraces);
    } else if (!credentialReferenceMatches(profile.credentialRef, profile.id)) {
        if (error) *error = QStringLiteral("凭据引用与服务档案不匹配");
        return false;
    }
    if (!proxyUrlIsSafe(profile.proxyUrl)) {
        if (error) *error = QStringLiteral("代理地址无效，不得包含凭据");
        return false;
    }
    if (profile.displayName.size() > 128 || profile.modelId.size() > 256
        || profile.availableModels.size() > 1'000) {
        if (error) *error = QStringLiteral("服务档案内容超出大小限制");
        return false;
    }
    profile.connectTimeoutMs = qBound(1'000, profile.connectTimeoutMs, 120'000);
    profile.requestTimeoutMs = qBound(profile.connectTimeoutMs, profile.requestTimeoutMs, 600'000);

    QWriteLocker locker(&lock_);
    for (auto& item : profiles_) {
        if (item.id == profile.id) {
            item = std::move(profile);
            return true;
        }
    }
    profiles_.append(std::move(profile));
    if (defaultProfileId_.isNull()) defaultProfileId_ = profiles_.last().id;
    return true;
}

bool AiProfileRepository::remove(const QUuid& id, QString* error)
{
    QWriteLocker locker(&lock_);
    for (qsizetype index = 0; index < profiles_.size(); ++index) {
        if (profiles_.at(index).id != id) continue;
        profiles_.removeAt(index);
        if (defaultProfileId_ == id) {
            defaultProfileId_ = profiles_.isEmpty() ? QUuid{} : profiles_.first().id;
        }
        return true;
    }
    if (error) *error = QStringLiteral("未找到服务档案");
    return false;
}

std::optional<ProviderProfile> AiProfileRepository::duplicate(const QUuid& id, QString* error)
{
    auto source = profile(id);
    if (!source) {
        if (error) *error = QStringLiteral("未找到服务档案");
        return std::nullopt;
    }
    source->id = QUuid::createUuid();
    source->displayName += QStringLiteral(" 副本");
    source->credentialRef = QStringLiteral("SnapAsk/provider/")
        + source->id.toString(QUuid::WithoutBraces);
    source->lastTestedAt = {};
    source->lastTestStatus.clear();
    if (!upsert(*source, error)) return std::nullopt;
    return source;
}

bool AiProfileRepository::setDefault(const QUuid& id, QString* error)
{
    QWriteLocker locker(&lock_);
    for (const auto& item : profiles_) {
        if (item.id == id) {
            defaultProfileId_ = id;
            return true;
        }
    }
    if (error) *error = QStringLiteral("未找到服务档案");
    return false;
}

QJsonObject AiProfileRepository::exportConfiguration() const
{
    QReadLocker locker(&lock_);
    QJsonArray profiles;
    for (const auto& profile : profiles_) profiles.append(toJson(profile));
    return {
        {QStringLiteral("version"), kProviderConfigurationSchemaVersion},
        {QStringLiteral("defaultProfileId"), defaultProfileId_.toString(QUuid::WithoutBraces)},
        {QStringLiteral("profiles"), profiles},
    };
}

bool AiProfileRepository::customHeadersAreSafe(const QJsonObject& headers, QString* error)
{
    // Provider configuration is ordinary JSON, so arbitrary header values are
    // fundamentally incompatible with the requirement that credentials can be
    // persisted only through credentialRef. Secret-pattern heuristics always
    // have opaque-token bypasses; accept only a small, finite set of public
    // metadata values instead.
    static const QSet<QString> clientHeaderNames{
        QStringLiteral("x-client"),
        QStringLiteral("x-client-name"),
    };
    static const QSet<QString> publicRegionValues{
        QStringLiteral("global"),
        QStringLiteral("test"),
        QStringLiteral("southeast-asia"),
    };

    if (headers.size() > 3) {
        if (error) *error = QStringLiteral("非敏感自定义请求头数量不能超过 3 个");
        return false;
    }

    QSet<QString> seenNames;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        const QString normalizedName = it.key().toLower();
        if (seenNames.contains(normalizedName)) {
            if (error) *error = QStringLiteral("自定义请求头名称不能重复");
            return false;
        }
        seenNames.insert(normalizedName);

        const bool clientMetadata = clientHeaderNames.contains(normalizedName);
        const bool regionMetadata = normalizedName == QStringLiteral("x-region");
        if (!clientMetadata && !regionMetadata) {
            if (error) {
                *error = QStringLiteral(
                    "仅允许 X-Client、X-Client-Name 和 X-Region 非敏感元数据；"
                    "凭据必须使用系统凭据存储");
            }
            return false;
        }
        if (!it.value().isString()) {
            if (error) *error = QStringLiteral("自定义请求头值必须是文本");
            return false;
        }
        const QString value = it.value().toString();
        const bool valueAllowed = clientMetadata
            ? value == QStringLiteral("SnapAsk")
            : publicRegionValues.contains(value);
        if (!valueAllowed) {
            if (error) {
                *error = QStringLiteral(
                    "自定义请求头只接受公开元数据值：X-Client/X-Client-Name "
                    "仅允许 SnapAsk，X-Region 仅允许 global、test 或 "
                    "southeast-asia；凭据必须使用系统凭据存储");
            }
            return false;
        }
    }
    return true;
}

QJsonObject AiProfileRepository::toJson(const ProviderProfile& profile)
{
    return {
        {QStringLiteral("id"), profile.id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("displayName"), profile.displayName},
        {QStringLiteral("protocol"), protocolToJson(profile.protocol)},
        {QStringLiteral("baseUrl"), profile.baseUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("credentialRef"), profile.credentialRef},
        {QStringLiteral("modelId"), profile.modelId},
        {QStringLiteral("availableModels"), stringListToJson(profile.availableModels)},
        {QStringLiteral("connectTimeoutMs"), profile.connectTimeoutMs},
        {QStringLiteral("requestTimeoutMs"), profile.requestTimeoutMs},
        {QStringLiteral("capabilities"), static_cast<qint64>(profile.capabilities.toInt())},
        {QStringLiteral("proxyUrl"), profile.proxyUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("customHeaders"), profile.customHeaders},
        {QStringLiteral("lastTestedAt"), profile.lastTestedAt.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("lastTestStatus"), profile.lastTestStatus},
    };
}

std::optional<ProviderProfile> AiProfileRepository::fromJson(
    const QJsonObject& object,
    QString* error)
{
    // Plaintext credential fields are rejected recursively before migration.
    ProviderProfile profile;
    profile.id = QUuid(object.value(QStringLiteral("id")).toString());
    if (profile.id.isNull()) {
        if (error) *error = QStringLiteral("服务档案 ID 无效");
        return std::nullopt;
    }
    profile.displayName = object.value(QStringLiteral("displayName")).toString();
    const auto protocol = protocolFromJson(object.value(QStringLiteral("protocol")).toString());
    if (!protocol) {
        if (error) *error = QStringLiteral("服务协议不受支持");
        return std::nullopt;
    }
    profile.protocol = *protocol;
    profile.baseUrl = QUrl(object.value(QStringLiteral("baseUrl")).toString());
    const auto endpointResult = EndpointPolicy::validateBaseUrl(profile.baseUrl);
    if (!endpointResult.accepted) {
        if (error) *error = endpointResult.error;
        return std::nullopt;
    }
    profile.credentialRef = object.value(QStringLiteral("credentialRef")).toString();
    if (!credentialReferenceMatches(profile.credentialRef, profile.id)) {
        if (error) *error = QStringLiteral("凭据引用与服务档案不匹配");
        return std::nullopt;
    }
    profile.modelId = object.value(QStringLiteral("modelId")).toString();
    profile.availableModels = stringListFromJson(
        object.value(QStringLiteral("availableModels")).toArray());
    profile.connectTimeoutMs = object.value(QStringLiteral("connectTimeoutMs")).toInt(15'000);
    profile.requestTimeoutMs = object.value(QStringLiteral("requestTimeoutMs")).toInt(120'000);
    profile.connectTimeoutMs = qBound(1'000, profile.connectTimeoutMs, 120'000);
    profile.requestTimeoutMs = qBound(
        profile.connectTimeoutMs, profile.requestTimeoutMs, 600'000);
    profile.capabilities = Capabilities::fromInt(
        object.value(QStringLiteral("capabilities")).toInt(
            static_cast<int>(ImageInput | Streaming | ModelList)));
    profile.proxyUrl = QUrl(object.value(QStringLiteral("proxyUrl")).toString());
    if (!proxyUrlIsSafe(profile.proxyUrl)) {
        if (error) *error = QStringLiteral("代理地址无效，不得包含凭据");
        return std::nullopt;
    }
    profile.customHeaders = object.value(QStringLiteral("customHeaders")).toObject();
    if (!customHeadersAreSafe(profile.customHeaders, error)) return std::nullopt;
    profile.lastTestedAt = QDateTime::fromString(
        object.value(QStringLiteral("lastTestedAt")).toString(), Qt::ISODateWithMs);
    profile.lastTestStatus = object.value(QStringLiteral("lastTestStatus")).toString();
    return profile;
}

} // namespace snapask::ai
