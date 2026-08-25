#include "ai/AiProfileRepository.h"
#include "ai/ChatCompletionsProvider.h"
#include "ai/EndpointPolicy.h"
#include "ai/OpenAIResponsesProvider.h"
#include "ai/SseDecoder.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace snapask::ai;

class M3AiTests final : public QObject {
    Q_OBJECT

private slots:
    void sseHandlesFragmentationUtf8AndMultipleEvents();
    void sseHandlesCrLfAndMultiLineData();
    void endpointPolicyProtectsPublicTrafficAndOrigins();
    void responsesPayloadAndEventsFollowContract();
    void chatPayloadAndEventsFollowContract();
    void profilesPersistWithoutSecrets();
    void customHeadersCannotPersistCredentialMaterial();
    void unversionedConfigurationMigratesWithoutCredentials();
    void explicitLegacyConfigurationMigratesWithoutCredentials();
    void plaintextCredentialsAndUnsupportedVersionsAreRejected();
};

void M3AiTests::sseHandlesFragmentationUtf8AndMultipleEvents()
{
    SseDecoder decoder;
    const QByteArray first = QStringLiteral(
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"中\"}\n\n"
        "data: [DONE]\n\n").toUtf8();
    const auto characterStart = first.indexOf(QStringLiteral("中").toUtf8());
    QVERIFY(characterStart > 0);

    QVERIFY(decoder.push(first.left(characterStart + 1)).isEmpty());
    const auto events = decoder.push(first.mid(characterStart + 1));
    QVERIFY2(!decoder.hasError(), qPrintable(decoder.errorString()));
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.at(0).event, QStringLiteral("response.output_text.delta"));
    QVERIFY(events.at(0).data.contains(QStringLiteral("中")));
    QCOMPARE(events.at(1).data, QStringLiteral("[DONE]"));
}

void M3AiTests::sseHandlesCrLfAndMultiLineData()
{
    SseDecoder decoder;
    const auto events = decoder.push(
        "id: 7\r\nevent: note\r\ndata: first\r\ndata: second\r\n\r\n");
    QVERIFY2(!decoder.hasError(), qPrintable(decoder.errorString()));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().id, QStringLiteral("7"));
    QCOMPARE(events.first().event, QStringLiteral("note"));
    QCOMPARE(events.first().data, QStringLiteral("first\nsecond"));
}

void M3AiTests::endpointPolicyProtectsPublicTrafficAndOrigins()
{
    QVERIFY(EndpointPolicy::validateBaseUrl(QUrl(QStringLiteral("https://api.example/v1"))).accepted);
    QVERIFY(EndpointPolicy::validateBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:8080/v1"))).accepted);
    QVERIFY(!EndpointPolicy::validateBaseUrl(QUrl(QStringLiteral("http://api.example/v1"))).accepted);
    QVERIFY(!EndpointPolicy::validateBaseUrl(QUrl(QStringLiteral("https://key@api.example/v1"))).accepted);
    QVERIFY(EndpointPolicy::isSameOrigin(
        QUrl(QStringLiteral("https://api.example/v1")),
        QUrl(QStringLiteral("https://API.example:443/other"))));
    QVERIFY(!EndpointPolicy::isSameOrigin(
        QUrl(QStringLiteral("https://api.example")),
        QUrl(QStringLiteral("https://api.example:444"))));
}

void M3AiTests::responsesPayloadAndEventsFollowContract()
{
    ProviderProfile profile;
    profile.baseUrl = QUrl(QStringLiteral("https://api.openai.com/v1"));
    profile.modelId = QStringLiteral("gpt-test");
    AiRequest request;
    request.snapshotPng = QByteArrayLiteral("png-sentinel");
    request.question = QStringLiteral("问题 sentinel");
    request.requestId = QUuid::createUuid();

    OpenAIResponsesProvider provider;
    QString error;
    const auto bytes = provider.buildStreamingPayload(profile, request, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));
    const auto root = QJsonDocument::fromJson(bytes).object();
    QCOMPARE(root.value(QStringLiteral("store")).toBool(), false);
    QCOMPARE(root.value(QStringLiteral("stream")).toBool(), true);
    QCOMPARE(root.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-test"));
    QVERIFY(QString::fromUtf8(bytes).contains(QStringLiteral("问题 sentinel")));
    QVERIFY(QString::fromUtf8(bytes).contains(QStringLiteral("cG5nLXNlbnRpbmVs")));

    const auto delta = provider.mapEvent(
        {QStringLiteral("response.output_text.delta"),
         QStringLiteral("{\"type\":\"response.output_text.delta\",\"delta\":\"答案\"}"), {}},
        request.requestId);
    QCOMPARE(delta.size(), 1);
    QCOMPARE(delta.first().type, EventType::TextDelta);
    QCOMPARE(delta.first().text, QStringLiteral("答案"));

    const auto completed = provider.mapEvent(
        {{}, QStringLiteral("[DONE]"), {}}, request.requestId);
    QCOMPARE(completed.first().type, EventType::Completed);
    QCOMPARE(provider.responseEndpoint(profile), QUrl(QStringLiteral("https://api.openai.com/v1/responses")));
}

void M3AiTests::chatPayloadAndEventsFollowContract()
{
    ProviderProfile profile;
    profile.baseUrl = QUrl(QStringLiteral("https://compatible.example/v1/"));
    profile.modelId = QStringLiteral("vision-model");
    AiRequest request;
    request.snapshotPng = QByteArrayLiteral("image");
    request.question = QStringLiteral("explain");
    request.requestId = QUuid::createUuid();

    ChatCompletionsProvider provider;
    QString error;
    const auto bytes = provider.buildStreamingPayload(profile, request, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));
    const auto root = QJsonDocument::fromJson(bytes).object();
    QVERIFY(root.value(QStringLiteral("messages")).isArray());
    QCOMPARE(root.value(QStringLiteral("stream")).toBool(), true);
    QVERIFY(QString::fromUtf8(bytes).contains(QStringLiteral("data:image/png;base64,")));

    const auto events = provider.mapEvent(
        {{}, QStringLiteral("{\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":null}]}"), {}},
        request.requestId);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().text, QStringLiteral("ok"));
    QCOMPARE(provider.responseEndpoint(profile),
             QUrl(QStringLiteral("https://compatible.example/v1/chat/completions")));
}

void M3AiTests::profilesPersistWithoutSecrets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("providers.json"));
    AiProfileRepository repository(path);

    QList<QUuid> ids;
    for (int index = 0; index < 3; ++index) {
        ProviderProfile profile;
        profile.id = QUuid::createUuid();
        profile.displayName = QStringLiteral("Provider %1").arg(index);
        profile.protocol = index == 2 ? Protocol::ChatCompletions : Protocol::OpenAIResponses;
        profile.baseUrl = QUrl(QStringLiteral("https://provider%1.example/v1").arg(index));
        profile.modelId = QStringLiteral("model-%1").arg(index);
        profile.credentialRef = QStringLiteral("SnapAsk/provider/")
            + profile.id.toString(QUuid::WithoutBraces);
        QVERIFY(repository.upsert(profile));
        ids.append(profile.id);
    }
    QVERIFY(repository.setDefault(ids.at(1)));
    QVERIFY(repository.save());

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto stored = file.readAll();
    QVERIFY(!stored.contains("apiKey"));
    QVERIFY(!stored.contains("Authorization"));
    QCOMPARE(QJsonDocument::fromJson(stored).object()
                 .value(QStringLiteral("profiles")).toArray().size(), 3);

    AiProfileRepository reloaded(path);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.profiles().size(), 3);
    QCOMPARE(reloaded.defaultProfileId(), ids.at(1));
    const auto duplicate = reloaded.duplicate(ids.first());
    QVERIFY(duplicate.has_value());
    QVERIFY(duplicate->id != ids.first());
    QVERIFY(duplicate->credentialRef.endsWith(duplicate->id.toString(QUuid::WithoutBraces)));
}

void M3AiTests::customHeadersCannotPersistCredentialMaterial()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AiProfileRepository repository(
        directory.filePath(QStringLiteral("providers.json")));

    ProviderProfile profile;
    profile.id = QUuid::createUuid();
    profile.displayName = QStringLiteral("Header policy test");
    profile.baseUrl = QUrl(QStringLiteral("https://provider.example/v1"));
    profile.modelId = QStringLiteral("model");
    profile.credentialRef = QStringLiteral("SnapAsk/provider/")
        + profile.id.toString(QUuid::WithoutBraces);

    QString error;
    QVERIFY2(AiProfileRepository::customHeadersAreSafe(
                 QJsonObject{
                     {QStringLiteral("X-Client"), QStringLiteral("SnapAsk")},
                     {QStringLiteral("X-Region"), QStringLiteral("test")},
                 },
                 &error),
             qPrintable(error));

    profile.customHeaders = {
        {QStringLiteral("X-Client-Name"), QStringLiteral("SnapAsk")},
        {QStringLiteral("X-Region"), QStringLiteral("southeast-asia")},
    };
    error.clear();
    QVERIFY2(repository.upsert(profile, &error), qPrintable(error));

    const QList<QPair<QString, QString>> rejectedHeaders{
        {QStringLiteral("Ocp-Apim-Subscription-Key"),
         QStringLiteral("subscription-secret-value")},
        {QStringLiteral("X-Credential"), QStringLiteral("credential-value")},
        {QStringLiteral("X-Signature"), QStringLiteral("signature-value")},
        {QStringLiteral("X-Foo"), QStringLiteral("sk-live-secret-sentinel-1234")},
        {QStringLiteral("X-Client-Name"),
         QStringLiteral("hf_abcdefghijklmnopqrstuvwxyz012345")},
        {QStringLiteral("X-Foo"), QStringLiteral("Bearer secret-sentinel")},
        {QStringLiteral("X-Foo"),
         QStringLiteral("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJzbmFwYXNrIn0.signaturepart")},
        {QStringLiteral("X-Foo"), QStringLiteral("api_key=secret-sentinel")},
        {QStringLiteral("X-Region"),
         QStringLiteral("0123456789abcdef0123456789abcdef")},
        {QStringLiteral("X-Client-Name"),
         QStringLiteral("opaque-value-without-a-known-secret-prefix")},
        {QStringLiteral("X-Feature"), QStringLiteral("enabled")},
    };
    for (const auto& [name, value] : rejectedHeaders) {
        ProviderProfile unsafe = profile;
        unsafe.customHeaders = {{name, value}};
        error.clear();
        QVERIFY2(!repository.upsert(unsafe, &error),
                 qPrintable(QStringLiteral("unsafe header was accepted: %1")
                                .arg(name)));
        QVERIFY(!error.isEmpty());
    }

    QVERIFY(repository.save(&error));
    QFile storedFile(directory.filePath(QStringLiteral("providers.json")));
    QVERIFY(storedFile.open(QIODevice::ReadOnly));
    const QByteArray stored = storedFile.readAll();
    QVERIFY(!stored.contains("subscription-secret-value"));
    QVERIFY(!stored.contains("sk-live-secret-sentinel"));
    QVERIFY(!stored.contains("api_key=secret-sentinel"));
    QVERIFY(!stored.contains("0123456789abcdef0123456789abcdef"));
    QVERIFY(stored.contains("SnapAsk"));
    QVERIFY(stored.contains("southeast-asia"));
    QVERIFY(stored.contains("credentialRef"));

    const QByteArray exported = QJsonDocument(
        repository.exportConfiguration()).toJson(QJsonDocument::Compact);
    QVERIFY(!exported.contains("subscription-secret-value"));
    QVERIFY(!exported.contains("sk-live-secret-sentinel"));
    QVERIFY(!exported.contains("api_key=secret-sentinel"));
    QVERIFY(!exported.contains("0123456789abcdef0123456789abcdef"));
    QVERIFY(exported.contains("SnapAsk"));
    QVERIFY(exported.contains("southeast-asia"));

    QJsonObject injectedRoot = repository.exportConfiguration();
    QJsonArray injectedProfiles = injectedRoot.value(
        QStringLiteral("profiles")).toArray();
    QJsonObject injectedProfile = injectedProfiles.first().toObject();
    injectedProfile.insert(
        QStringLiteral("customHeaders"),
        QJsonObject{{
            QStringLiteral("X-Region"),
            QStringLiteral("0123456789abcdef0123456789abcdef")}});
    injectedProfiles.replace(0, injectedProfile);
    injectedRoot.insert(QStringLiteral("profiles"), injectedProfiles);

    const QString injectedPath = directory.filePath(
        QStringLiteral("providers-injected.json"));
    QFile injectedFile(injectedPath);
    QVERIFY(injectedFile.open(QIODevice::WriteOnly));
    const QByteArray injectedBytes = QJsonDocument(injectedRoot).toJson(
        QJsonDocument::Compact);
    QCOMPARE(injectedFile.write(injectedBytes), injectedBytes.size());
    injectedFile.close();

    AiProfileRepository injectedRepository(injectedPath);
    error.clear();
    QVERIFY(!injectedRepository.load(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(
        QStringLiteral("0123456789abcdef0123456789abcdef")));
    QVERIFY(injectedRepository.profiles().isEmpty());
}

namespace {

QJsonObject legacyProfile(const QUuid& id)
{
    return {
        {QStringLiteral("id"), id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("displayName"), QStringLiteral("Legacy Provider")},
        {QStringLiteral("protocol"), QStringLiteral("responses")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://legacy.example/v1")},
        {QStringLiteral("modelId"), QStringLiteral("legacy-model")},
        {QStringLiteral("connectTimeoutMs"), 12'000},
        {QStringLiteral("requestTimeoutMs"), 90'000},
    };
}

bool writeConfiguration(const QString& path, const QJsonObject& root)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size();
}

void verifyLegacyMigration(const bool includeExplicitVersion)
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("providers.json"));
    const QUuid id = QUuid::createUuid();

    QJsonObject root{
        {QStringLiteral("defaultProfileId"), id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("profiles"), QJsonArray{legacyProfile(id)}},
    };
    if (includeExplicitVersion) {
        root.insert(QStringLiteral("version"), 0);
    }
    QVERIFY(writeConfiguration(path, root));

    AiProfileRepository repository(path);
    QString error;
    QVERIFY2(repository.load(&error), qPrintable(error));
    QCOMPARE(repository.defaultProfileId(), id);
    const auto migrated = repository.profile(id);
    QVERIFY(migrated.has_value());
    QCOMPARE(
        migrated->credentialRef,
        QStringLiteral("SnapAsk/provider/")
            + id.toString(QUuid::WithoutBraces));
    QCOMPARE(migrated->displayName, QStringLiteral("Legacy Provider"));
    QCOMPARE(migrated->modelId, QStringLiteral("legacy-model"));

    const QJsonObject current = repository.exportConfiguration();
    QCOMPARE(
        current.value(QStringLiteral("version")).toInt(),
        kProviderConfigurationSchemaVersion);
    const QByteArray exported = QJsonDocument(current).toJson(QJsonDocument::Compact);
    QVERIFY(!exported.contains("apiKey"));
    QVERIFY(!exported.contains("Authorization"));

    QVERIFY2(repository.save(&error), qPrintable(error));
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto savedRoot = QJsonDocument::fromJson(saved.readAll()).object();
    QCOMPARE(
        savedRoot.value(QStringLiteral("version")).toInt(),
        kProviderConfigurationSchemaVersion);
}

} // namespace

void M3AiTests::unversionedConfigurationMigratesWithoutCredentials()
{
    verifyLegacyMigration(false);
}

void M3AiTests::explicitLegacyConfigurationMigratesWithoutCredentials()
{
    verifyLegacyMigration(true);
}

void M3AiTests::plaintextCredentialsAndUnsupportedVersionsAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("providers.json"));
    const QUuid id = QUuid::createUuid();

    QJsonObject validRoot{
        {QStringLiteral("version"), 0},
        {QStringLiteral("defaultProfileId"), id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("profiles"), QJsonArray{legacyProfile(id)}},
    };

    const QString secret = QStringLiteral("credential-migration-secret-sentinel");
    AiProfileRepository repository(path);
    const QStringList rejectedFieldNames{
        QStringLiteral("api_key"), QStringLiteral("Authorization"),
        QStringLiteral("access-token"), QStringLiteral("clientSecret"),
        QStringLiteral("credentials"),
    };
    QString error;
    for (const auto& fieldName : rejectedFieldNames) {
        QJsonObject profileWithSecret = legacyProfile(id);
        profileWithSecret.insert(fieldName, secret);
        QJsonObject rootWithSecret = validRoot;
        rootWithSecret.insert(
            QStringLiteral("profiles"), QJsonArray{profileWithSecret});
        QVERIFY(writeConfiguration(path, rootWithSecret));

        error.clear();
        QVERIFY2(!repository.load(&error), qPrintable(fieldName));
        QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains(secret));
        QVERIFY(repository.profiles().isEmpty());
    }

    QJsonObject nestedSecretRoot = validRoot;
    nestedSecretRoot.insert(
        QStringLiteral("legacyAuth"),
        QJsonObject{{QStringLiteral("client_secret"), secret}});
    QVERIFY(writeConfiguration(path, nestedSecretRoot));
    error.clear();
    QVERIFY(!repository.load(&error));
    QVERIFY(!error.contains(secret));

    QJsonObject futureRoot = validRoot;
    futureRoot.insert(
        QStringLiteral("version"), kProviderConfigurationSchemaVersion + 1);
    QVERIFY(writeConfiguration(path, futureRoot));
    error.clear();
    QVERIFY(!repository.load(&error));
    QVERIFY(!error.isEmpty());

    QJsonObject malformedVersionRoot = validRoot;
    malformedVersionRoot.insert(QStringLiteral("version"), QStringLiteral("1"));
    QVERIFY(writeConfiguration(path, malformedVersionRoot));
    error.clear();
    QVERIFY(!repository.load(&error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(M3AiTests)
#include "M3AiTests.moc"
