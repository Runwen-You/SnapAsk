#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace snapask::ai {

struct SseEvent {
    QString event;
    QString data;
    QString id;
};

class SseDecoder final {
public:
    [[nodiscard]] QList<SseEvent> push(const QByteArray& bytes);
    [[nodiscard]] QList<SseEvent> finish();
    void reset();
    [[nodiscard]] bool hasError() const noexcept;
    [[nodiscard]] QString errorString() const;

private:
    QList<SseEvent> consumeCompleteLines(bool endOfStream);
    void consumeLine(const QByteArray& line, QList<SseEvent>& events);
    void dispatch(QList<SseEvent>& events);
    void setLimitError();

    QByteArray buffer_;
    QByteArray eventName_;
    QByteArray eventId_;
    QList<QByteArray> dataLines_;
    qsizetype queuedDataBytes_{0};
    bool failed_{false};
    QString errorString_;
};

} // namespace snapask::ai
