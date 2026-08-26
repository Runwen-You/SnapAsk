#pragma once

#include "ui/glass/GlassBackdropCache.h"
#include "ui/glass/GlassMaterial.h"

#include <QPointF>
#include <QWidget>

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPropertyAnimation;

namespace snapask::ui::glass {

class GlassSurface : public QWidget {
    Q_OBJECT
    Q_PROPERTY(
        qreal hoverProgress
        READ hoverProgress
        WRITE setHoverProgress)
    Q_PROPERTY(
        qreal pressProgress
        READ pressProgress
        WRITE setPressProgress)
    Q_PROPERTY(
        QPointF pointerPosition
        READ pointerPosition
        WRITE setPointerPosition)

public:
    explicit GlassSurface(
        QWidget* parent = nullptr,
        Qt::WindowFlags flags = {});
    ~GlassSurface() override;

    [[nodiscard]] GlassMaterial material() const;
    void setMaterial(const GlassMaterial& material);
    void resetMaterialFromTheme();

    [[nodiscard]] GlassMaterialRole materialRole() const noexcept;
    void setMaterialRole(GlassMaterialRole role);

    [[nodiscard]] GlassBackdropMode backdropMode() const noexcept;
    void setBackdropMode(GlassBackdropMode mode);
    void setBackdropImage(
        const QImage& source,
        const QRect& sourceRect = {},
        quint64 sourceRevision = 0);
    void clearBackdropImage();
    [[nodiscard]] bool hasBackdropImage() const noexcept;
    [[nodiscard]] quint64 backdropGenerationCount() const noexcept;
    void setFallbackEnabled(bool enabled);
    [[nodiscard]] bool fallbackEnabled() const noexcept;

    [[nodiscard]] qreal hoverProgress() const noexcept;
    void setHoverProgress(qreal progress);
    [[nodiscard]] qreal pressProgress() const noexcept;
    void setPressProgress(qreal progress);
    [[nodiscard]] QPointF pointerPosition() const noexcept;
    void setPointerPosition(const QPointF& position);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;
    [[nodiscard]] virtual QRectF glassRect() const;
    [[nodiscard]] GlassMaterial effectiveMaterial() const;

private:
    void refreshMaterial();
    void animateHover(qreal target, int durationMs);
    void animatePress(qreal target, int durationMs);
    void animatePointerHome();

    GlassMaterial material_;
    GlassMaterialRole materialRole_{GlassMaterialRole::Standard};
    GlassBackdropMode backdropMode_{GlassBackdropMode::Native};
    GlassBackdropCache backdropCache_;
    QImage backdropSource_;
    QRect backdropSourceRect_;
    quint64 backdropSourceRevision_{0};
    QPropertyAnimation* hoverAnimation_{nullptr};
    QPropertyAnimation* pressAnimation_{nullptr};
    QPropertyAnimation* pointerAnimation_{nullptr};
    qreal hoverProgress_{0.0};
    qreal pressProgress_{0.0};
    QPointF pointerPosition_{-1.0, -1.0};
    bool followsTheme_{true};
    bool fallbackEnabled_{false};
};

}  // namespace snapask::ui::glass
