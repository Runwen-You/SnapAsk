#pragma once

#include "domain/annotation/Annotation.h"

#include <QElapsedTimer>
#include <QImage>
#include <QMetaObject>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <memory>
#include <optional>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace snapask {
class ScreenshotSession;
}

namespace snapask::ui::canvas {

enum class CanvasTool {
    Select,
    Rectangle,
    Arrow,
    Text,
    Mosaic,
};

// CanvasWidget maps QWidget logical input coordinates to source-image physical
// pixels. Selection outlines and resize handles are editor chrome painted only
// by this widget; they are never added to AnnotationDocument.
class CanvasWidget final : public QWidget {
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget* parent = nullptr);
    explicit CanvasWidget(ScreenshotSession* session,
                          QWidget* parent = nullptr);
    ~CanvasWidget() override;

    CanvasWidget(const CanvasWidget&) = delete;
    CanvasWidget& operator=(const CanvasWidget&) = delete;

    void setSession(ScreenshotSession* session);
    [[nodiscard]] ScreenshotSession* session() noexcept;
    [[nodiscard]] const ScreenshotSession* session() const noexcept;

    void setTool(CanvasTool tool);
    [[nodiscard]] CanvasTool tool() const noexcept;

    // Changing the current style also applies it to the current selection as
    // one undoable operation. With no selection it only changes creation style.
    void setCurrentStyle(const AnnotationStyle& style);
    [[nodiscard]] const AnnotationStyle& currentStyle() const noexcept;
    void setStrokeColor(const QColor& color);
    void setStrokeWidth(qreal width);
    void setTextFont(const QFont& font);
    void setMosaicBlockSize(int blockSize);
    void setMosaicBrushWidth(qreal width);
    [[nodiscard]] qreal mosaicBrushWidth() const noexcept;
    [[nodiscard]] quint64 rebuildableCacheByteSize() const noexcept;
    void releaseRebuildableCaches();

    // Mapping helpers are intentionally public so callers and tests can verify
    // that all annotation geometry remains in source physical pixels.
    [[nodiscard]] QRectF imageDisplayRect() const;
    [[nodiscard]] std::optional<QPointF> mapWidgetToImage(
        const QPointF& widgetPoint) const;
    [[nodiscard]] QPointF mapImageToWidget(const QPointF& imagePoint) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

public slots:
    void deleteSelection();
    void clearAnnotations();
    void editSelectedText();
    void refreshFromSession();

signals:
    void contentChanged();
    void selectionChanged();
    void toolChanged(snapask::ui::canvas::CanvasTool tool);
    void currentStyleChanged(const snapask::AnnotationStyle& style);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct InteractionState;

    void initialize();
    void invalidatePresentation();
    void handleUndoIndexChanged();
    void setSelectedAnnotation(const QUuid& annotationId);
    void clearSelection();
    void cancelInteraction();
    void updateHoverCursor(const QPointF& widgetPoint);
    void finishInteraction(const QPointF& imagePoint);
    void createTextAnnotation(const QRectF& requestedRect);
    void applyCurrentStyleToSelection();
    [[nodiscard]] bool nudgeSelection(
        int key,
        Qt::KeyboardModifiers modifiers);
    void resetNudgeMergeGroup();

    [[nodiscard]] const Annotation* hitAnnotation(
        const QPointF& imagePoint) const;
    [[nodiscard]] const Annotation* primarySelection() const;
    [[nodiscard]] QImage presentationImage();

    ScreenshotSession* session_ = nullptr;
    CanvasTool tool_ = CanvasTool::Select;
    AnnotationStyle currentStyle_;
    qreal mosaicBrushWidth_ = 28.0;
    QMetaObject::Connection undoIndexConnection_;

    QImage cachedPresentation_;
    quint64 cachedRevision_ = 0;
    bool presentationValid_ = false;

    QElapsedTimer nudgeMergeTimer_;
    QUuid nudgeAnnotationId_;
    quint64 nudgeMergeGroup_{0};
    quint64 nextNudgeMergeGroup_{1};

    std::unique_ptr<InteractionState> interaction_;
};

}  // namespace snapask::ui::canvas

Q_DECLARE_METATYPE(snapask::ui::canvas::CanvasTool)
