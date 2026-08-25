#include "ai/ILlmProvider.h"

#include "ai/ChatCompletionsProvider.h"
#include "ai/OpenAIResponsesProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace snapask::ai {

std::unique_ptr<ILlmProvider> createProvider(const Protocol protocol)
{
    switch (protocol) {
    case Protocol::OpenAIResponses:
        return std::make_unique<OpenAIResponsesProvider>();
    case Protocol::ChatCompletions:
        return std::make_unique<ChatCompletionsProvider>();
    }
    return {};
}

QString defaultSystemPrompt()
{
    return QStringLiteral(
        "你是一个截图知识问答助手。使用用户的语言回答。\n"
        "先给结论，再给 3～5 个必要要点，避免冗长背景。\n"
        "解释代码时说明用途、主要流程、关键变量和明显风险。\n"
        "图片更新时，重点关注用户新增的矩形、箭头、文字和马赛克区域。\n"
        "如果截图内容不完整或不可辨认，请明确指出，不要猜测。");
}

QString ILlmProvider::textProbePrompt()
{
    return QStringLiteral(
        "Reply with exactly SNAPASK_TEXT_OK. Do not add anything else.");
}

QString ILlmProvider::imageProbePrompt()
{
    return QStringLiteral(
        "Inspect the attached non-sensitive test image. If its dominant "
        "color is red, reply exactly SNAPASK_IMAGE_RED; otherwise reply "
        "SNAPASK_IMAGE_OTHER.");
}

QString ILlmProvider::fixedProbeImageDataUrl()
{
    // A 256 x 256 opaque red PNG generated specifically for SnapAsk's provider
    // capability probe. It contains no user or machine data.
    return QStringLiteral(
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAB/klEQVR42u3TQREAAAQAQSTRP5Mw3jLYjXAzl9Md8FVJgAHAAGAA"
        "MAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAA"
        "MAAYAAwABgADgAHAAGAADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgA"
        "DAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAATAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYA"
        "A4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMgAHAAGAAMAAYAAwABgADgAHA"
        "AGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHA"
        "AGAAMAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAw"
        "ABgADAAGAAOAAcAAYAAwABgADADXAnq7Axw49NyKAAAAAElFTkSuQmCC");
}

QString ILlmProvider::expectedProbeMarker(
    const ProviderProbeOperation operation)
{
    switch (operation) {
    case ProviderProbeOperation::TextConnection:
        return QStringLiteral("SNAPASK_TEXT_OK");
    case ProviderProbeOperation::ImageUnderstanding:
        return QStringLiteral("SNAPASK_IMAGE_RED");
    case ProviderProbeOperation::ModelList:
        return {};
    }
    return {};
}

bool ILlmProvider::parseOpenAiModelList(
    const QByteArray& response,
    QStringList* modelIds)
{
    if (modelIds == nullptr) {
        return false;
    }
    modelIds->clear();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonValue dataValue = document.object().value(QStringLiteral("data"));
    if (!dataValue.isArray()) {
        return false;
    }

    constexpr qsizetype maximumModels = 1'000;
    QSet<QString> seen;
    const QJsonArray data = dataValue.toArray();
    for (const QJsonValue& value : data) {
        if (!value.isObject()) {
            return false;
        }
        const QJsonValue idValue = value.toObject().value(QStringLiteral("id"));
        if (!idValue.isString()) {
            return false;
        }
        const QString id = idValue.toString().trimmed();
        if (id.isEmpty() || id.size() > 256) {
            return false;
        }
        for (const QChar character : id) {
            if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
                return false;
            }
        }
        if (!seen.contains(id)) {
            seen.insert(id);
            if (modelIds->size() < maximumModels) {
                modelIds->append(id);
            }
        }
    }
    return true;
}

} // namespace snapask::ai
