#pragma once

#include "ui/glass/GlassSurface.h"

class QAction;
class QEvent;
class QHBoxLayout;

namespace snapask::ui::glass {

class GlassToolButton;

class GlassToolbar final : public GlassSurface {
    Q_OBJECT

public:
    explicit GlassToolbar(QWidget* parent = nullptr);

    [[nodiscard]] GlassToolButton* addAction(QAction* action);
    void addWidget(QWidget* widget, int stretch = 0);
    void addSeparator();
    void addStretch(int stretch = 1);
    void setButtonExtent(int extent);
    [[nodiscard]] int buttonExtent() const noexcept;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    [[nodiscard]] QRectF glassRect() const override;

private:
    void observePointerFrom(QWidget* widget, QEvent* event);

    QHBoxLayout* layout_{nullptr};
    int buttonExtent_{36};
};

}  // namespace snapask::ui::glass
