#include "ai/ILlmProvider.h"
#include "ai/ProviderProbeClient.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <type_traits>
#include <utility>

using snapask::ai::ErrorKind;
using snapask::ai::ILlmProvider;
using snapask::ai::Protocol;
using snapask::ai::ProviderProbeClient;
using snapask::ai::ProviderProbeOperation;
using snapask::ai::ProviderProbeResult;
using snapask::ai::ProviderProfile;

namespace {

[[nodiscard]] ProviderProfile profile(const Protocol protocol)
{
    ProviderProfile result;
    result.id = QUuid::createUuid();
    result.protocol = protocol;
    result.baseUrl = QUrl(QStringLiteral("https://api.example.test/v1"));
    result.modelId = QStringLiteral("probe-model");
    return result;
}

[[nodiscard]] QByteArray responsesResult(const QString& text)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("output"), QJsonArray{QJsonObject{
             {QStringLiteral("type"), QStringLiteral("message")},
             {QStringLiteral("content"), QJsonArray{QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("output_text")},
                  {QStringLiteral("text"), text},
              }}},
         }}},
    }).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray chatResult(const QString& text)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{QJsonObject{
             {QStringLiteral("message"), QJsonObject{
                  {QStringLiteral("role"), QStringLiteral("assistant")},
                  {QStringLiteral("content"), text},
              }},
         }}},
    }).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QJsonObject payloadObject(
    const ILlmProvider& provider,
    const ProviderProfile& providerProfile,
    const ProviderProbeOperation operation)
{
    QString error;
    const QByteArray payload =
        provider.buildProbePayload(providerProfile, operation, &error);
    if (payload.isEmpty()) {
        qFatal("Probe payload unexpectedly empty: %s", qPrintable(error));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qFatal("Probe payload was not a JSON object");
    }
    return document.object();
}

[[nodiscard]] QString imageDataUrl(
    const Protocol protocol,
    const QJsonObject& payload)
{
    if (protocol == Protocol::OpenAIResponses) {
        const QJsonArray content = payload.value(QStringLiteral("input"))
                                       .toArray()
                                       .first()
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue& value : content) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("type")).toString()
                == QStringLiteral("input_image")) {
                return item.value(QStringLiteral("image_url")).toString();
            }
        }
        return {};
    }

    const QJsonArray content = payload.value(QStringLiteral("messages"))
                                   .toArray()
                                   .first()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue& value : content) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toString()
            == QStringLiteral("image_url")) {
            return item.value(QStringLiteral("image_url"))
                .toObject()
                .value(QStringLiteral("url"))
                .toString();
        }
    }
    return {};
}

}  // namespace

class M4ProviderProbeTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void fixedPayloadsContainNoUserData_data();
    void fixedPayloadsContainNoUserData();
    void parsesModelsWithStableDeduplicationAndLimit_data();
    void parsesModelsWithStableDeduplicationAndLimit();
    void rejectsMalformedModelResponses_data();
    void rejectsMalformedModelResponses();
    void validatesProtocolSpecificProbeResponses_data();
    void validatesProtocolSpecificProbeResponses();
    void publicClientRejectsAndWipesEmptyOrInvalidCredentials();
};

void M4ProviderProbeTests::initTestCase()
{
    qRegisterMetaType<ProviderProbeResult>();

    using ImageProbeSignature = QUuid (ProviderProbeClient::*)(
        const ProviderProfile&,
        QString&&);
    static_assert(std::is_same_v<
                  decltype(&ProviderProbeClient::testImageUnderstanding),
                  ImageProbeSignature>);
}

void M4ProviderProbeTests::fixedPayloadsContainNoUserData_data()
{
    QTest::addColumn<int>("protocol");
    QTest::newRow("responses") << static_cast<int>(Protocol::OpenAIResponses);
    QTest::newRow("chat") << static_cast<int>(Protocol::ChatCompletions);
}

void M4ProviderProbeTests::fixedPayloadsContainNoUserData()
{
    QFETCH(int, protocol);
    const Protocol protocolValue = static_cast<Protocol>(protocol);
    const ProviderProfile providerProfile = profile(protocolValue);
    const std::unique_ptr<ILlmProvider> provider =
        snapask::ai::createProvider(protocolValue);
    QVERIFY(provider != nullptr);

    const QJsonObject textPayload = payloadObject(
        *provider,
        providerProfile,
        ProviderProbeOperation::TextConnection);
    const QByteArray textBytes =
        QJsonDocument(textPayload).toJson(QJsonDocument::Compact);
    QVERIFY(textBytes.contains("SNAPASK_TEXT_OK"));
    QVERIFY(!textBytes.contains("data:image"));
    QVERIFY(!textBytes.contains("USER_QUESTION_SENTINEL"));
    QCOMPARE(textPayload.value(QStringLiteral("stream")).toBool(), false);

    const QJsonObject imagePayload = payloadObject(
        *provider,
        providerProfile,
        ProviderProbeOperation::ImageUnderstanding);
    const QByteArray imageBytes =
        QJsonDocument(imagePayload).toJson(QJsonDocument::Compact);
    QVERIFY(!imageBytes.contains("USER_SCREENSHOT_SENTINEL"));
    const QString dataUrl = imageDataUrl(protocolValue, imagePayload);
    const QString prefix = QStringLiteral("data:image/png;base64,");
    QVERIFY(dataUrl.startsWith(prefix));
    const QByteArray png = QByteArray::fromBase64(
        dataUrl.sliced(prefix.size()).toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    const QImage image = QImage::fromData(png, "PNG");
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(256, 256));
    QCOMPARE(image.pixelColor(128, 128), QColor(220, 32, 32));

    if (protocolValue == Protocol::OpenAIResponses) {
        QCOMPARE(textPayload.value(QStringLiteral("store")).toBool(), false);
        QCOMPARE(imagePayload.value(QStringLiteral("store")).toBool(), false);
    } else {
        QVERIFY(!textPayload.contains(QStringLiteral("store")));
        QVERIFY(!imagePayload.contains(QStringLiteral("store")));
        QVERIFY(!textPayload.contains(QStringLiteral("stream_options")));
    }
}

void M4ProviderProbeTests::parsesModelsWithStableDeduplicationAndLimit_data()
{
    fixedPayloadsContainNoUserData_data();
}

void M4ProviderProbeTests::parsesModelsWithStableDeduplicationAndLimit()
{
    QFETCH(int, protocol);
    const std::unique_ptr<ILlmProvider> provider =
        snapask::ai::createProvider(static_cast<Protocol>(protocol));
    QVERIFY(provider != nullptr);

    QJsonArray data;
    data.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("model-a")}});
    data.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("model-a")}});
    data.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("model-b")}});
    for (int index = 0; index < 1'005; ++index) {
        data.append(QJsonObject{{
            QStringLiteral("id"),
            QStringLiteral("bulk-%1").arg(index, 4, 10, QLatin1Char('0')),
        }});
    }
    const QByteArray response = QJsonDocument(
        QJsonObject{{QStringLiteral("data"), data}})
                                    .toJson(QJsonDocument::Compact);
    QStringList models;
    QVERIFY(provider->parseProbeResponse(
        ProviderProbeOperation::ModelList,
        response,
        &models));
    QCOMPARE(models.size(), 1'000);
    QCOMPARE(models.at(0), QStringLiteral("model-a"));
    QCOMPARE(models.at(1), QStringLiteral("model-b"));
    QCOMPARE(models.count(QStringLiteral("model-a")), 1);
}

void M4ProviderProbeTests::rejectsMalformedModelResponses_data()
{
    QTest::addColumn<QByteArray>("response");
    QTest::newRow("not-json") << QByteArrayLiteral("not-json");
    QTest::newRow("missing-data") << QByteArrayLiteral("{}");
    QTest::newRow("data-not-array") << QByteArrayLiteral(R"({"data":{}})");
    QTest::newRow("entry-not-object") << QByteArrayLiteral(R"({"data":[7]})");
    QTest::newRow("id-not-string")
        << QByteArrayLiteral(R"({"data":[{"id":7}]})");
    QTest::newRow("empty-id")
        << QByteArrayLiteral(R"({"data":[{"id":"  "}]})");
    QTest::newRow("control-id")
        << QByteArrayLiteral("{\"data\":[{\"id\":\"bad\\u0001id\"}]}");
}

void M4ProviderProbeTests::rejectsMalformedModelResponses()
{
    QFETCH(QByteArray, response);
    for (const Protocol protocol : {
             Protocol::OpenAIResponses,
             Protocol::ChatCompletions}) {
        const std::unique_ptr<ILlmProvider> provider =
            snapask::ai::createProvider(protocol);
        QVERIFY(provider != nullptr);
        QStringList models{QStringLiteral("must-be-cleared")};
        QVERIFY(!provider->parseProbeResponse(
            ProviderProbeOperation::ModelList,
            response,
            &models));
        QVERIFY(models.isEmpty());
    }
}

void M4ProviderProbeTests::validatesProtocolSpecificProbeResponses_data()
{
    fixedPayloadsContainNoUserData_data();
}

void M4ProviderProbeTests::validatesProtocolSpecificProbeResponses()
{
    QFETCH(int, protocol);
    const Protocol protocolValue = static_cast<Protocol>(protocol);
    const std::unique_ptr<ILlmProvider> provider =
        snapask::ai::createProvider(protocolValue);
    QVERIFY(provider != nullptr);
    const auto response = [protocolValue](const QString& text) {
        return protocolValue == Protocol::OpenAIResponses
            ? responsesResult(text)
            : chatResult(text);
    };

    QStringList models;
    QVERIFY(provider->parseProbeResponse(
        ProviderProbeOperation::TextConnection,
        response(QStringLiteral("SNAPASK_TEXT_OK")),
        &models));
    QVERIFY(provider->parseProbeResponse(
        ProviderProbeOperation::ImageUnderstanding,
        response(QStringLiteral("SNAPASK_IMAGE_RED")),
        &models));
    QVERIFY(!provider->parseProbeResponse(
        ProviderProbeOperation::TextConnection,
        response(QStringLiteral("unexpected provider prose")),
        &models));
    QVERIFY(!provider->parseProbeResponse(
        ProviderProbeOperation::ImageUnderstanding,
        QByteArrayLiteral("{malformed"),
        &models));
}

void M4ProviderProbeTests::publicClientRejectsAndWipesEmptyOrInvalidCredentials()
{
    ProviderProbeClient client;
    QSignalSpy resultSpy(&client, &ProviderProbeClient::resultReady);
    ProviderProfile providerProfile = profile(Protocol::OpenAIResponses);

    QString emptyKey;
    const QUuid emptyOperation =
        client.fetchModels(providerProfile, std::move(emptyKey));
    QCOMPARE(resultSpy.size(), 1);
    ProviderProbeResult result =
        qvariant_cast<ProviderProbeResult>(resultSpy.takeFirst().first());
    QCOMPARE(result.operationId, emptyOperation);
    QCOMPARE(result.providerProfileId, providerProfile.id);
    QCOMPARE(result.errorKind, ErrorKind::Authentication);
    QVERIFY(!result.success);

    providerProfile.baseUrl = QUrl(QStringLiteral("http://public.example.test/v1"));
    QString key = QStringLiteral("SECRET_PROBE_KEY_SENTINEL");
    const QUuid invalidOperation =
        client.testTextConnection(providerProfile, std::move(key));
    QVERIFY(key.isEmpty());
    QCOMPARE(resultSpy.size(), 1);
    result = qvariant_cast<ProviderProbeResult>(resultSpy.takeFirst().first());
    QCOMPARE(result.operationId, invalidOperation);
    QCOMPARE(result.errorKind, ErrorKind::InvalidConfiguration);
    QVERIFY(!result.message.contains(QStringLiteral("SECRET")));
    QVERIFY(!client.isActive(invalidOperation));
}

QTEST_GUILESS_MAIN(M4ProviderProbeTests)
#include "M4ProviderProbeTests.moc"
