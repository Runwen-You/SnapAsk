#pragma once

#include "domain/annotation/Annotation.h"

#include <QSet>
#include <QVector>

#include <optional>

namespace snapask {

class AnnotationDocument final {
public:
    struct RemovedAnnotation {
        Annotation annotation;
        qsizetype index{-1};
    };

    using Container = QVector<Annotation>;

    [[nodiscard]] const Container& annotations() const noexcept;
    [[nodiscard]] Container annotationsInPaintOrder() const;
    [[nodiscard]] qsizetype size() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] quint64 revision() const noexcept;

    [[nodiscard]] qsizetype indexOf(const QUuid& id) const;
    [[nodiscard]] const Annotation* annotation(const QUuid& id) const;

    bool addAnnotation(const Annotation& annotation);
    bool insertAnnotation(qsizetype index, const Annotation& annotation);
    [[nodiscard]] std::optional<RemovedAnnotation> takeAnnotation(
        const QUuid& id);
    bool removeAnnotation(const QUuid& id);

    bool setGeometry(const QUuid& id, const AnnotationGeometry& geometry);
    bool setStyle(const QUuid& id, const AnnotationStyle& style);
    bool setText(const QUuid& id, const QString& text);

    [[nodiscard]] Container clearAnnotations();
    bool replaceAll(Container annotations);

    [[nodiscard]] const QSet<QUuid>& selectedAnnotationIds() const noexcept;
    // Selection is editor chrome and intentionally does not advance content revision.
    void setSelectedAnnotationIds(QSet<QUuid> ids);
    void clearSelection();

private:
    void recordMutation();
    void removeMissingSelections();

    Container annotations_;
    QSet<QUuid> selectedAnnotationIds_;
    quint64 revision_{0};
};

}  // namespace snapask
