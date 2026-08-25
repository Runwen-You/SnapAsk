#include "ai/ChatCompletionsProvider.h"

#include "ai/EndpointPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace snapask::ai {

Protocol ChatCompletionsProvider::protocol() const
{
    return Protocol::ChatCompletions;
}

QUrl ChatCompletionsProvider::responseEndpoint(const ProviderProfile& profile) const
{
    return EndpointPolicy::endpoint(profile.baseUrl, QStringLiteral("chat/completions"));
}

QUrl ChatCompletionsProvider::modelsEndpoint(const ProviderProfile& profile) const
{
    return EndpointPolicy::endpoint(profile.baseUrl, QStringLiteral("models"));
}

QByteArray ChatCompletionsProvider::buildStreamingPayload(
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

    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), defaultSystemPrompt()},
    });
    for (const auto& message : request.recentContext) {
        messages.append(QJsonObject{
            {QStringLiteral("role"), message.role == ConversationMessage::Role::User
                 ? QStringLiteral("user") : QStringLiteral("assistant")},
            {QStringLiteral("content"), message.text},
        });
    }

    QJsonArray content;
    content.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), request.question},
    });
    content.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("image_url")},
        {QStringLiteral("image_url"), QJsonObject{
            {QStringLiteral("url"), QStringLiteral("data:image/png;base64,")
                 + QString::fromLatin1(request.snapshotPng.toBase64())},
        }},
    });
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), content},
    });

    return QJsonDocument(QJsonObject{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("stream"), true},
        {QStringLiteral("stream_options"), QJsonObject{{QStringLiteral("include_usage"), true}}},
    }).toJson(QJsonDocument::Compact);
}

QList<AiStreamEvent> ChatCompletionsProvider::mapEvent(
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
        AiStreamEvent failed;
        failed.type = EventType::Failed;
        failed.requestId = requestId;
        failed.errorKind = ErrorKind::InvalidResponse;
        failed.errorMessage = QStringLiteral("兼容服务返回了无法解析的流式事件");
        return {failed};
    }
    const auto root = document.object();
    if (root.contains(QStringLiteral("error"))) {
        AiStreamEvent failed;
        failed.type = EventType::Failed;
        failed.requestId = requestId;
        failed.errorKind = ErrorKind::Server;
        failed.errorMessage = QStringLiteral("兼容服务报告生成失败");
        return {failed};
    }

    QList<AiStreamEvent> results;
    const auto choices = root.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const auto choice = choices.first().toObject();
        const auto text = choice.value(QStringLiteral("delta")).toObject()
                              .value(QStringLiteral("content")).toString();
        if (!text.isEmpty()) {
            AiStreamEvent delta;
            delta.type = EventType::TextDelta;
            delta.requestId = requestId;
            delta.text = text;
            results.append(delta);
        }
        const QString finishReason =
            choice.value(QStringLiteral("finish_reason")).toString();
        if (!finishReason.isEmpty()) {
            AiStreamEvent terminal;
            terminal.requestId = requestId;
            if (finishReason == QStringLiteral("stop")) {
                terminal.type = EventType::Completed;
            } else {
                terminal.type = EventType::Failed;
                terminal.errorKind = ErrorKind::InvalidResponse;
                terminal.errorMessage = finishReason == QStringLiteral("length")
                    ? QStringLiteral("回答因长度限制未完整生成")
                    : QStringLiteral("回答未完整生成");
            }
            results.append(terminal);
        }
    }

    const auto usage = root.value(QStringLiteral("usage")).toObject();
    if (!usage.isEmpty()) {
        AiStreamEvent usageEvent;
        usageEvent.type = EventType::UsageUpdated;
        usageEvent.requestId = requestId;
        usageEvent.inputTokens = usage.value(QStringLiteral("prompt_tokens")).toInteger(-1);
        usageEvent.outputTokens = usage.value(QStringLiteral("completion_tokens")).toInteger(-1);
        results.append(usageEvent);
    }
    return results;
}

QByteArray ChatCompletionsProvider::buildProbePayload(
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

    QJsonValue content = operation == ProviderProbeOperation::TextConnection
        ? QJsonValue(textProbePrompt())
        : QJsonValue(QJsonArray{
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("text")},
                  {QStringLiteral("text"), imageProbePrompt()},
              },
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("image_url")},
                  {QStringLiteral("image_url"), QJsonObject{
                       {QStringLiteral("url"), fixedProbeImageDataUrl()},
                   }},
              },
          });
    const QJsonArray messages{QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), content},
    }};
    return QJsonDocument(QJsonObject{
        {QStringLiteral("model"), profile.modelId},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("stream"), false},
    }).toJson(QJsonDocument::Compact);
}

bool ChatCompletionsProvider::parseProbeResponse(
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
    const QJsonValue choicesValue = root.value(QStringLiteral("choices"));
    if (!choicesValue.isArray() || choicesValue.toArray().isEmpty()) {
        return false;
    }
    const QJsonValue firstChoice = choicesValue.toArray().first();
    if (!firstChoice.isObject()) {
        return false;
    }
    const QJsonValue contentValue = firstChoice.toObject()
                                        .value(QStringLiteral("message"))
                                        .toObject()
                                        .value(QStringLiteral("content"));
    QString outputText;
    if (contentValue.isString()) {
        outputText = contentValue.toString();
    } else if (contentValue.isArray()) {
        QStringList pieces;
        for (const QJsonValue& item : contentValue.toArray()) {
            if (!item.isObject()) {
                return false;
            }
            const QJsonObject object = item.toObject();
            if (object.value(QStringLiteral("type")).toString()
                == QStringLiteral("text")) {
                pieces.append(object.value(QStringLiteral("text")).toString());
            }
        }
        outputText = pieces.join(QLatin1Char('\n'));
    } else {
        return false;
    }

    const QString marker = expectedProbeMarker(operation);
    return !marker.isEmpty() && outputText.contains(marker);
}

} // namespace snapask::ai
