#pragma once

#include "domain/annotation/AnnotationDocument.h"

#include <QUndoCommand>

namespace snapask {

class AddAnnotationCommand : public QUndoCommand {
public:
    AddAnnotationCommand(AnnotationDocument* document,
                         Annotation annotation,
                         qsizetype insertionIndex = -1,
                         QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

protected:
    AnnotationDocument* document_{nullptr};
    Annotation annotation_;
    qsizetype insertionIndex_{-1};
    bool applied_{false};
};

class RemoveAnnotationCommand final : public QUndoCommand {
public:
    RemoveAnnotationCommand(AnnotationDocument* document,
                            const QUuid& annotationId,
                            QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    AnnotationDocument* document_{nullptr};
    Annotation annotation_;
    qsizetype insertionIndex_{-1};
    bool valid_{false};
    bool applied_{false};
};

class TransformAnnotationCommand final : public QUndoCommand {
public:
    TransformAnnotationCommand(AnnotationDocument* document,
                               const QUuid& annotationId,
                               AnnotationGeometry after,
                               quint64 mergeGroup = 0,
                               QUndoCommand* parent = nullptr);
    TransformAnnotationCommand(AnnotationDocument* document,
                               const QUuid& annotationId,
                               AnnotationGeometry before,
                               AnnotationGeometry after,
                               quint64 mergeGroup,
                               QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;
    [[nodiscard]] int id() const override;
    bool mergeWith(const QUndoCommand* other) override;

private:
    AnnotationDocument* document_{nullptr};
    QUuid annotationId_;
    AnnotationGeometry before_;
    AnnotationGeometry after_;
    quint64 mergeGroup_{0};
    bool valid_{false};
};

class ChangeStyleCommand final : public QUndoCommand {
public:
    ChangeStyleCommand(AnnotationDocument* document,
                       const QUuid& annotationId,
                       AnnotationStyle after,
                       quint64 mergeGroup = 0,
                       QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;
    [[nodiscard]] int id() const override;
    bool mergeWith(const QUndoCommand* other) override;

private:
    AnnotationDocument* document_{nullptr};
    QUuid annotationId_;
    AnnotationStyle before_;
    AnnotationStyle after_;
    quint64 mergeGroup_{0};
    bool valid_{false};
};

class EditTextCommand final : public QUndoCommand {
public:
    EditTextCommand(AnnotationDocument* document,
                    const QUuid& annotationId,
                    QString after,
                    QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    AnnotationDocument* document_{nullptr};
    QUuid annotationId_;
    QString before_;
    QString after_;
    bool valid_{false};
};

class AddMosaicStrokeCommand final : public AddAnnotationCommand {
public:
    AddMosaicStrokeCommand(AnnotationDocument* document,
                           QVector<QPointF> points,
                           qreal brushWidth,
                           AnnotationStyle style = {},
                           int zOrder = 0,
                           const QUuid& annotationId = QUuid::createUuid(),
                           QUndoCommand* parent = nullptr);
};

class ClearAnnotationsCommand final : public QUndoCommand {
public:
    explicit ClearAnnotationsCommand(AnnotationDocument* document,
                                     QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    AnnotationDocument* document_{nullptr};
    AnnotationDocument::Container annotations_;
    QSet<QUuid> selection_;
    bool valid_{false};
};

}  // namespace snapask
