#pragma once

#include <QPushButton>

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPropertyAnimation;

namespace snapask::ui::glass {

class GlassButton final : public QPushButton {
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
    explicit GlassButton(QWidget* parent = nullptr);
    explicit GlassButton(const QString& text, QWidget* parent = nullptr);

    void setAccent(bool accent);
    [[nodiscard]] bool isAccent() const noexcept;
    void setDanger(bool danger);
    [[nodiscard]] bool isDanger() const noexcept;
    [[nodiscard]] qreal hoverProgress() const noexcept;
    void setHoverProgress(qreal progress);
    [[nodiscard]] qreal pressProgress() const noexcept;
    void setPressProgress(qreal progress);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void initialize();
    void animateHover(qreal target, int durationMs);
    void animatePress(qreal target, int durationMs);

    QPropertyAnimation* hoverAnimation_{nullptr};
    QPropertyAnimation* pressAnimation_{nullptr};
    qreal hoverProgress_{0.0};
    qreal pressProgress_{0.0};
    bool accent_{false};
    bool danger_{false};
};

}  // namespace snapask::ui::glass
