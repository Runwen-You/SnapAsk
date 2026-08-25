#include "ai/OpenAIResponsesProvider.h"

#include "ai/EndpointPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace snapask::ai {
namespace {

AiStreamEvent failedEvent(
    const QUuid& requestId,
    const QString& message,
    const ErrorKind kind = ErrorKind::InvalidResponse)
{
    AiStreamEvent result;
    result.type = EventType::Failed;
    result.requestId = requestId;
    result.errorKind = kind;
    result.errorMessage = message;
    return result;
}

QJsonObject textMessage(const QString& role, const QString& text)
{
    return {
        {QStringLiteral("role"), role},
        {QStringLiteral("content"), text},
    };
}

} // namespace

Protocol OpenAIResponsesProvider::protocol() const
{
    return Protocol::OpenAIResponses;
}

QUrl OpenAIResponsesProvider::responseEndpoint(const ProviderProfile& profile) const
{
    return EndpointPolicy::endpoint(profile.baseUrl, QStringLiteral("responses"));
}

QUrl OpenAIResponsesProvider::modelsEndpoint(const ProviderProfile& profile) const
{
    return EndpointPolicy::endpoint(profile.baseUrl, QStringLiteral("models"));
}

QByteArray OpenAIResponsesProvider::buildStreamingPayload(
    const ProviderProfile& profile,
    const AiRequest& request,
    QString* error) const
{
    if (request.snapshotPng.isEmpty()) {
        if (error) *error = QStringLiteral("当前截图为空");
        return {};
    }
    const auto model = request.modelId.isEmpty() ? profile.modelId : request.modelId;
    if (model.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未选择模型");
        return {};
    }

    QJsonArray input;
    input.append(textMessage(QStringLiteral("system"), defaultSystemPrompt()));
    for (const auto& message : request.recentContext) {
        input.append(textMessage(
            message.role == ConversationMessage::Role::User
                ? QStringLiteral("user") : QStringLiteral("assistant"),
            message.text));
    }

    QJsonArray currentContent;
    currentContent.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("input_text")},
        {QStringLiteral("text"), request.question},
    });
    currentContent.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("input_image")},
        {QStringLiteral("image_url"), QStringLiteral("data:image/png;base64,")
            + QString::fromLatin1(request.snapshotPng.toBase64())},
    });
    input.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), currentContent},
    });

    const QJsonObject root{
        {QStringLiteral("model"), model},
        {QStringLiteral("store"), false},
        {QStringLiteral("stream"), true},
        {QStringLiteral("input"), input},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QList<AiStreamEvent> OpenAIResponsesProvider::mapEvent(
    const SseEvent& event,
    const QUuid& requestId) const
{
    if (event.data.trimmed() == QStringLiteral("[DONE]")) {
        AiStreamEvent completed;
        completed.type = EventType::Completed;
        completed.requestId = requestId;
        return {completed};
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(event.data.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {failedEvent(requestId, QStringLiteral("服务返回了无法解析的流式事件"))};
    }
    const auto root = document.object();
    const auto type = !event.event.isEmpty()
        ? event.event : root.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("response.output_text.delta")) {
        AiStreamEvent delta;
        delta.type = EventType::TextDelta;
        delta.requestId = requestId;
        delta.text = root.value(QStringLiteral("delta")).toString();
        return delta.text.isEmpty() ? QList<AiStreamEvent>{} : QList<AiStreamEvent>{delta};
    }
    if (type == QStringLiteral("response.completed")) {
        QList<AiStreamEvent> results;
        const auto usage = root.value(QStringLiteral("response")).toObject()
                               .value(QStringLiteral("usage")).toObject();
        if (!usage.isEmpty()) {
            AiStreamEvent usageEvent;
            usageEvent.type = EventType::UsageUpdated;
            usageEvent.requestId = requestId;
            usageEvent.inputTokens = usage.value(QStringLiteral("input_tokens")).toInteger(-1);
            usageEvent.outputTokens = usage.value(QStringLiteral("output_tokens")).toInteger(-1);
            results.append(usageEvent);
        }
        AiStreamEvent completed;
        completed.type = EventType::Completed;
        completed.requestId = requestId;
        results.append(completed);
        return results;
    }
    if (type == QStringLiteral("response.incomplete")) {
        return {failedEvent(
            requestId,
            QStringLiteral("回答未完整生成，可重试原请求"),
            ErrorKind::InvalidResponse)};
    }
    if (type == QStringLiteral("response.failed") || type == QStringLiteral("error")) {
        return {failedEvent(
            requestId,
            QStringLiteral("服务报告生成失败"),
            ErrorKind::Server)};
    }
    return {};
}

QByteArray OpenAIResponsesProvider::buildProbePayload(
    const ProviderProfile& profile,
    const ProviderProbeOperation operation,
    QString* error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (operation == ProviderProbeOperation::ModelList) {
        return {};
    }
    if (profile.modelId.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("未选择测试模型");
        }
        return {};
    }

    QJsonArray content;
    content.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("input_text")},
        {QStringLiteral("text"), operation == ProviderProbeOperation::TextConnection
             ? textProbePrompt()
             : imageProbePrompt()},
    });
    if (operation == ProviderProbeOperation::ImageUnderstanding) {
        content.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("input_image")},
            {QStringLiteral("image_url"), fixedProbeImageDataUrl()},
        });
    }

    const QJsonArray input{QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), content},
    }};
    return QJsonDocument(QJsonObject{
        {QStringLiteral("model"), profile.modelId},
        {QStringLiteral("store"), false},
        {QStringLiteral("stream"), false},
        {QStringLiteral("input"), input},
    }).toJson(QJsonDocument::Compact);
}

bool OpenAIResponsesProvider::parseProbeResponse(
    const ProviderProbeOperation operation,
    const QByteArray& response,
    QStringList* modelIds) const
{
    if (operation == ProviderProbeOperation::ModelList) {
        return parseOpenAiModelList(response, modelIds);
    }
    if (modelIds != nullptr) {
        modelIds->clear();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject root = document.object();
    if (root.contains(QStringLiteral("error"))) {
        return false;
    }

    QStringList outputTexts;
    const QJsonValue directText = root.value(QStringLiteral("output_text"));
    if (directText.isString()) {
        outputTexts.append(directText.toString());
    }
    const QJsonValue outputValue = root.value(QStringLiteral("output"));
    if (!outputValue.isArray()) {
        return false;
    }
    for (const QJsonValue& outputItem : outputValue.toArray()) {
        if (!outputItem.isObject()) {
            return false;
        }
        const QJsonValue contentValue =
            outputItem.toObject().value(QStringLiteral("content"));
        if (!contentValue.isArray()) {
            continue;
        }
        for (const QJsonValue& contentItem : contentValue.toArray()) {
            if (!contentItem.isObject()) {
                return false;
            }
            const QJsonObject contentObject = contentItem.toObject();
            if (contentObject.value(QStringLiteral("type")).toString()
                == QStringLiteral("output_text")) {
                outputTexts.append(
                    contentObject.value(QStringLiteral("text")).toString());
            }
        }
    }

    const QString marker = expectedProbeMarker(operation);
    return !marker.isEmpty() && outputTexts.join(QLatin1Char('\n')).contains(marker);
}

} // namespace snapask::ai
