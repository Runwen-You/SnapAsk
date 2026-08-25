#pragma once

#include "services/SnapshotRenderer.h"

#include <QPoint>
#include <QWidget>

class QCloseEvent;
class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

namespace snapask::ui::pin {

class PinWindow final : public QWidget {
    Q_OBJECT

public:
    // A pin is created only from one already-rendered immutable snapshot. It has
    // no access to ScreenshotSession, annotations, or SnapshotRenderer.
    explicit PinWindow(const snapask::RenderedSnapshot& snapshot);
    ~PinWindow() override = default;

    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    [[nodiscard]] const snapask::RenderedSnapshot& snapshot() const noexcept;
    [[nodiscard]] bool isAlwaysOnTop() const noexcept;
    [[nodiscard]] qreal displayScale() const noexcept;

public slots:
    void setAlwaysOnTop(bool enabled);
    void toggleAlwaysOnTop();
    void resetToActualPixels();

signals:
    void alwaysOnTopChanged(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    [[nodiscard]] QSize scaledSize(qreal scale) const noexcept;
    void resizeAroundGlobalPoint(qreal scale, const QPoint& globalAnchor);

    snapask::RenderedSnapshot snapshot_;
    qreal displayScale_{1.0};
    bool alwaysOnTop_{true};
    bool dragging_{false};
    QPoint dragOffset_;
};

}  // namespace snapask::ui::pin
