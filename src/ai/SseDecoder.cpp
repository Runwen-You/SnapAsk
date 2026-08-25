#include "ai/SseDecoder.h"

#include <utility>

namespace snapask::ai {

namespace {
constexpr qsizetype kMaximumBufferedLineBytes = 1024 * 1024;
constexpr qsizetype kMaximumEventDataBytes = 8 * 1024 * 1024;
}

QList<SseEvent> SseDecoder::push(const QByteArray& bytes)
{
    if (failed_) return {};
    if (bytes.size() > kMaximumEventDataBytes
        || buffer_.size() > kMaximumEventDataBytes - bytes.size()) {
        setLimitError();
        return {};
    }
    buffer_.append(bytes);
    return consumeCompleteLines(false);
}

QList<SseEvent> SseDecoder::finish()
{
    if (failed_) return {};
    auto events = consumeCompleteLines(true);
    if (!eventName_.isEmpty() || !eventId_.isEmpty() || !dataLines_.isEmpty()) {
        dispatch(events);
    }
    return events;
}

void SseDecoder::reset()
{
    buffer_.clear();
    eventName_.clear();
    eventId_.clear();
    dataLines_.clear();
    queuedDataBytes_ = 0;
    failed_ = false;
    errorString_.clear();
}

bool SseDecoder::hasError() const noexcept
{
    return failed_;
}

QString SseDecoder::errorString() const
{
    return errorString_;
}

QList<SseEvent> SseDecoder::consumeCompleteLines(const bool endOfStream)
{
    QList<SseEvent> events;
    while (true) {
        const auto newline = buffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        if (line.size() > kMaximumBufferedLineBytes) {
            setLimitError();
            break;
        }
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        consumeLine(line, events);
    }

    if (endOfStream && !buffer_.isEmpty()) {
        QByteArray line = buffer_;
        buffer_.clear();
        if (line.size() > kMaximumBufferedLineBytes) {
            setLimitError();
            return events;
        }
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        consumeLine(line, events);
    }
    return events;
}

void SseDecoder::consumeLine(const QByteArray& line, QList<SseEvent>& events)
{
    if (line.isEmpty()) {
        dispatch(events);
        return;
    }
    if (line.startsWith(':')) {
        return;
    }

    const auto colon = line.indexOf(':');
    QByteArray field = colon < 0 ? line : line.left(colon);
    QByteArray value = colon < 0 ? QByteArray{} : line.mid(colon + 1);
    if (value.startsWith(' ')) {
        value.remove(0, 1);
    }

    if (field == "event") {
        eventName_ = value;
    } else if (field == "data") {
        if (value.size() > kMaximumEventDataBytes - queuedDataBytes_) {
            setLimitError();
            return;
        }
        dataLines_.append(value);
        queuedDataBytes_ += value.size() + 1;
    } else if (field == "id" && !value.contains('\0')) {
        eventId_ = value;
    }
}

void SseDecoder::dispatch(QList<SseEvent>& events)
{
    if (dataLines_.isEmpty()) {
        eventName_.clear();
        return;
    }

    SseEvent event;
    event.event = QString::fromUtf8(eventName_);
    event.id = QString::fromUtf8(eventId_);
    event.data = QString::fromUtf8(dataLines_.join('\n'));
    events.append(std::move(event));
    eventName_.clear();
    dataLines_.clear();
    queuedDataBytes_ = 0;
}

void SseDecoder::setLimitError()
{
    failed_ = true;
    buffer_.clear();
    eventName_.clear();
    eventId_.clear();
    dataLines_.clear();
    queuedDataBytes_ = 0;
    errorString_ = QStringLiteral("服务返回的流式内容超出安全大小限制");
}

} // namespace snapask::ai
