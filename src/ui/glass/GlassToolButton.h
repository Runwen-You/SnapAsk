#pragma once

#include <QToolButton>

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPropertyAnimation;

namespace snapask::ui::glass {

class GlassToolButton final : public QToolButton {
    Q_OBJECT
    Q_PROPERTY(
        qreal hoverProgress
        READ hoverProgress
        WRITE setHoverProgress)
    Q_PROPERTY(
        qreal pressProgress
        READ pressProgress
        WRITE setPressProgress)

public:
    explicit GlassToolButton(QWidget* parent = nullptr);

    [[nodiscard]] qreal hoverProgress() const noexcept;
    void setHoverProgress(qreal progress);
    [[nodiscard]] qreal pressProgress() const noexcept;
    void setPressProgress(qreal progress);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void animateHover(qreal target, int durationMs);
    void animatePress(qreal target, int durationMs);

    QPropertyAnimation* hoverAnimation_{nullptr};
    QPropertyAnimation* pressAnimation_{nullptr};
    qreal hoverProgress_{0.0};
    qreal pressProgress_{0.0};
};

}  // namespace snapask::ui::glass
