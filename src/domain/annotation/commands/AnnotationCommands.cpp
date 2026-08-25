#include "domain/annotation/commands/AnnotationCommands.h"

#include <utility>

namespace snapask {
namespace {

constexpr int kTransformCommandId = 0x534101;
constexpr int kStyleCommandId = 0x534102;

const Annotation* findAnnotation(const AnnotationDocument* document,
                                 const QUuid& annotationId) {
    return document != nullptr ? document->annotation(annotationId) : nullptr;
}

}  // namespace

AddAnnotationCommand::AddAnnotationCommand(AnnotationDocument* document,
                                           Annotation annotation,
                                           qsizetype insertionIndex,
                                           QUndoCommand* parent)
    : QUndoCommand(parent),
      document_(document),
      annotation_(std::move(annotation)),
      insertionIndex_(insertionIndex) {
    setText(QStringLiteral("Add annotation"));
    if (document_ == nullptr || !annotation_.isValid()) {
        setObsolete(true);
    }
}

void AddAnnotationCommand::undo() {
    if (document_ != nullptr && applied_) {
        document_->removeAnnotation(annotation_.id);
        applied_ = false;
    }
}

void AddAnnotationCommand::redo() {
    if (document_ == nullptr || applied_) {
        return;
    }
    if (insertionIndex_ < 0) {
        insertionIndex_ = document_->size();
    }
    applied_ = document_->insertAnnotation(insertionIndex_, annotation_);
}

RemoveAnnotationCommand::RemoveAnnotationCommand(AnnotationDocument* document,
                                                 const QUuid& annotationId,
                                                 QUndoCommand* parent)
    : QUndoCommand(parent), document_(document) {
    setText(QStringLiteral("Remove annotation"));
    if (const Annotation* existing = findAnnotation(document_, annotationId)) {
        annotation_ = *existing;
        insertionIndex_ = document_->indexOf(annotationId);
        valid_ = true;
    } else {
        setObsolete(true);
    }
}

void RemoveAnnotationCommand::undo() {
    if (valid_ && document_ != nullptr && applied_) {
        applied_ = !document_->insertAnnotation(insertionIndex_, annotation_);
    }
}

void RemoveAnnotationCommand::redo() {
    if (valid_ && document_ != nullptr && !applied_) {
        applied_ = document_->removeAnnotation(annotation_.id);
    }
}

TransformAnnotationCommand::TransformAnnotationCommand(
    AnnotationDocument* document,
    const QUuid& annotationId,
    AnnotationGeometry after,
    quint64 mergeGroup,
    QUndoCommand* parent)
    : QUndoCommand(parent),
      document_(document),
      annotationId_(annotationId),
      after_(std::move(after)),
      mergeGroup_(mergeGroup) {
    setText(QStringLiteral("Transform annotation"));
    if (const Annotation* existing = findAnnotation(document_, annotationId_)) {
        before_ = existing->geometry;
        valid_ = geometryMatchesType(existing->type, after_);
    }
    if (!valid_ || before_ == after_) {
        setObsolete(true);
    }
}

TransformAnnotationCommand::TransformAnnotationCommand(
    AnnotationDocument* document,
    const QUuid& annotationId,
    AnnotationGeometry before,
    AnnotationGeometry after,
    quint64 mergeGroup,
    QUndoCommand* parent)
    : QUndoCommand(parent),
      document_(document),
      annotationId_(annotationId),
      before_(std::move(before)),
      after_(std::move(after)),
      mergeGroup_(mergeGroup) {
    setText(QStringLiteral("Transform annotation"));
    if (const Annotation* existing = findAnnotation(document_, annotationId_)) {
        valid_ = geometryMatchesType(existing->type, before_) &&
                 geometryMatchesType(existing->type, after_);
    }
    if (!valid_ || before_ == after_) {
        setObsolete(true);
    }
}

void TransformAnnotationCommand::undo() {
    if (valid_ && document_ != nullptr) {
        document_->setGeometry(annotationId_, before_);
    }
}

void TransformAnnotationCommand::redo() {
    if (valid_ && document_ != nullptr) {
        document_->setGeometry(annotationId_, after_);
    }
}

int TransformAnnotationCommand::id() const {
    return mergeGroup_ == 0 ? -1 : kTransformCommandId;
}

bool TransformAnnotationCommand::mergeWith(const QUndoCommand* other) {
    const auto* command = dynamic_cast<const TransformAnnotationCommand*>(other);
    if (command == nullptr || mergeGroup_ == 0 ||
        command->mergeGroup_ != mergeGroup_ ||
        command->document_ != document_ ||
        command->annotationId_ != annotationId_ || !command->valid_) {
        return false;
    }
    after_ = command->after_;
    return true;
}

ChangeStyleCommand::ChangeStyleCommand(AnnotationDocument* document,
                                       const QUuid& annotationId,
                                       AnnotationStyle after,
                                       quint64 mergeGroup,
                                       QUndoCommand* parent)
    : QUndoCommand(parent),
      document_(document),
      annotationId_(annotationId),
      after_(std::move(after)),
      mergeGroup_(mergeGroup) {
    setText(QStringLiteral("Change annotation style"));
    if (const Annotation* existing = findAnnotation(document_, annotationId_)) {
        before_ = existing->style;
        Annotation candidate = *existing;
        candidate.style = after_;
        valid_ = candidate.isValid();
    }
    if (!valid_ || before_ == after_) {
        setObsolete(true);
    }
}

void ChangeStyleCommand::undo() {
    if (valid_ && document_ != nullptr) {
        document_->setStyle(annotationId_, before_);
    }
}

void ChangeStyleCommand::redo() {
    if (valid_ && document_ != nullptr) {
        document_->setStyle(annotationId_, after_);
    }
}

int ChangeStyleCommand::id() const {
    return mergeGroup_ == 0 ? -1 : kStyleCommandId;
}

bool ChangeStyleCommand::mergeWith(const QUndoCommand* other) {
    const auto* command = dynamic_cast<const ChangeStyleCommand*>(other);
    if (command == nullptr || mergeGroup_ == 0 ||
        command->mergeGroup_ != mergeGroup_ ||
        command->document_ != document_ ||
        command->annotationId_ != annotationId_ || !command->valid_) {
        return false;
    }
    after_ = command->after_;
    return true;
}

EditTextCommand::EditTextCommand(AnnotationDocument* document,
                                 const QUuid& annotationId,
                                 QString after,
                                 QUndoCommand* parent)
    : QUndoCommand(parent),
      document_(document),
      annotationId_(annotationId),
      after_(std::move(after)) {
    setText(QStringLiteral("Edit annotation text"));
    if (const Annotation* existing = findAnnotation(document_, annotationId_)) {
        if (const auto* text = std::get_if<TextGeometry>(&existing->geometry)) {
            before_ = text->text;
            valid_ = true;
        }
    }
    if (!valid_ || before_ == after_) {
        setObsolete(true);
    }
}

void EditTextCommand::undo() {
    if (valid_ && document_ != nullptr) {
        document_->setText(annotationId_, before_);
    }
}

void EditTextCommand::redo() {
    if (valid_ && document_ != nullptr) {
        document_->setText(annotationId_, after_);
    }
}

AddMosaicStrokeCommand::AddMosaicStrokeCommand(
    AnnotationDocument* document,
    QVector<QPointF> points,
    qreal brushWidth,
    AnnotationStyle style,
    int zOrder,
    const QUuid& annotationId,
    QUndoCommand* parent)
    : AddAnnotationCommand(
          document,
          Annotation::makeMosaic(std::move(points), brushWidth, style, zOrder,
                                 annotationId),
          -1,
          parent) {
    setText(QStringLiteral("Add mosaic stroke"));
}

ClearAnnotationsCommand::ClearAnnotationsCommand(AnnotationDocument* document,
                                                 QUndoCommand* parent)
    : QUndoCommand(parent), document_(document) {
    setText(QStringLiteral("Clear annotations"));
    if (document_ == nullptr || document_->isEmpty()) {
        setObsolete(true);
        return;
    }
    annotations_ = document_->annotations();
    selection_ = document_->selectedAnnotationIds();
    valid_ = true;
}

void ClearAnnotationsCommand::undo() {
    if (valid_ && document_ != nullptr) {
        document_->replaceAll(annotations_);
        document_->setSelectedAnnotationIds(selection_);
    }
}

void ClearAnnotationsCommand::redo() {
    if (valid_ && document_ != nullptr) {
        (void)document_->clearAnnotations();
    }
}

}  // namespace snapask
