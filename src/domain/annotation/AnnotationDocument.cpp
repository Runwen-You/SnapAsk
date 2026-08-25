#include "domain/annotation/AnnotationDocument.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace snapask {
namespace {

bool containsDuplicateIds(const AnnotationDocument::Container& annotations) {
    QSet<QUuid> ids;
    for (const Annotation& annotation : annotations) {
        if (!annotation.isValid() || ids.contains(annotation.id)) {
            return true;
        }
        ids.insert(annotation.id);
    }
    return false;
}

}  // namespace

const AnnotationDocument::Container& AnnotationDocument::annotations() const
    noexcept {
    return annotations_;
}

AnnotationDocument::Container AnnotationDocument::annotationsInPaintOrder()
    const {
    Container ordered = annotations_;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Annotation& lhs, const Annotation& rhs) {
                         return lhs.zOrder < rhs.zOrder;
                     });
    return ordered;
}

qsizetype AnnotationDocument::size() const noexcept {
    return annotations_.size();
}

bool AnnotationDocument::isEmpty() const noexcept {
    return annotations_.isEmpty();
}

quint64 AnnotationDocument::revision() const noexcept {
    return revision_;
}

qsizetype AnnotationDocument::indexOf(const QUuid& id) const {
    for (qsizetype index = 0; index < annotations_.size(); ++index) {
        if (annotations_[index].id == id) {
            return index;
        }
    }
    return -1;
}

const Annotation* AnnotationDocument::annotation(const QUuid& id) const {
    const qsizetype index = indexOf(id);
    return index >= 0 ? &annotations_[index] : nullptr;
}

bool AnnotationDocument::addAnnotation(const Annotation& annotation) {
    return insertAnnotation(annotations_.size(), annotation);
}

bool AnnotationDocument::insertAnnotation(qsizetype index,
                                          const Annotation& annotation) {
    if (!annotation.isValid() || indexOf(annotation.id) >= 0) {
        return false;
    }
    index = std::clamp<qsizetype>(index, 0, annotations_.size());
    annotations_.insert(index, annotation);
    recordMutation();
    return true;
}

std::optional<AnnotationDocument::RemovedAnnotation>
AnnotationDocument::takeAnnotation(const QUuid& id) {
    const qsizetype index = indexOf(id);
    if (index < 0) {
        return std::nullopt;
    }

    RemovedAnnotation removed{annotations_.takeAt(index), index};
    selectedAnnotationIds_.remove(id);
    recordMutation();
    return removed;
}

bool AnnotationDocument::removeAnnotation(const QUuid& id) {
    return takeAnnotation(id).has_value();
}

bool AnnotationDocument::setGeometry(const QUuid& id,
                                     const AnnotationGeometry& geometry) {
    const qsizetype index = indexOf(id);
    if (index < 0 || !geometryMatchesType(annotations_[index].type, geometry) ||
        annotations_[index].geometry == geometry) {
        return false;
    }

    Annotation candidate = annotations_[index];
    candidate.geometry = geometry;
    if (!candidate.isValid()) {
        return false;
    }
    annotations_[index].geometry = geometry;
    recordMutation();
    return true;
}

bool AnnotationDocument::setStyle(const QUuid& id,
                                  const AnnotationStyle& style) {
    const qsizetype index = indexOf(id);
    if (index < 0 || annotations_[index].style == style) {
        return false;
    }

    Annotation candidate = annotations_[index];
    candidate.style = style;
    if (!candidate.isValid()) {
        return false;
    }
    annotations_[index].style = style;
    recordMutation();
    return true;
}

bool AnnotationDocument::setText(const QUuid& id, const QString& text) {
    const qsizetype index = indexOf(id);
    if (index < 0 || annotations_[index].type != AnnotationType::Text) {
        return false;
    }

    auto* geometry = std::get_if<TextGeometry>(&annotations_[index].geometry);
    if (geometry == nullptr || geometry->text == text) {
        return false;
    }
    geometry->text = text;
    recordMutation();
    return true;
}

AnnotationDocument::Container AnnotationDocument::clearAnnotations() {
    if (annotations_.isEmpty()) {
        return {};
    }
    Container previous = std::exchange(annotations_, Container{});
    selectedAnnotationIds_.clear();
    recordMutation();
    return previous;
}

bool AnnotationDocument::replaceAll(Container annotations) {
    if (containsDuplicateIds(annotations) || annotations_ == annotations) {
        return false;
    }
    annotations_ = std::move(annotations);
    removeMissingSelections();
    recordMutation();
    return true;
}

const QSet<QUuid>& AnnotationDocument::selectedAnnotationIds() const noexcept {
    return selectedAnnotationIds_;
}

void AnnotationDocument::setSelectedAnnotationIds(QSet<QUuid> ids) {
    for (auto iterator = ids.begin(); iterator != ids.end();) {
        if (indexOf(*iterator) < 0) {
            iterator = ids.erase(iterator);
        } else {
            ++iterator;
        }
    }
    selectedAnnotationIds_ = std::move(ids);
}

void AnnotationDocument::clearSelection() {
    selectedAnnotationIds_.clear();
}

void AnnotationDocument::recordMutation() {
    if (revision_ != std::numeric_limits<quint64>::max()) {
        ++revision_;
    }
}

void AnnotationDocument::removeMissingSelections() {
    for (auto iterator = selectedAnnotationIds_.begin();
         iterator != selectedAnnotationIds_.end();) {
        if (indexOf(*iterator) < 0) {
            iterator = selectedAnnotationIds_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

}  // namespace snapask
