#include "ui/glass/GlassToolbar.h"

#include "ui/common/ThemeTokens.h"
#include "ui/glass/GlassToolButton.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QSizePolicy>

#include <algorithm>

namespace snapask::ui::glass {
namespace {

[[nodiscard]] snapask::ui::ThemeMode modeForPalette(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() >= 128
        ? snapask::ui::ThemeMode::Dark
        : snapask::ui::ThemeMode::Light;
}

class ToolbarSeparator final : public QWidget {
public:
    explicit ToolbarSeparator(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setFixedWidth(9);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        const auto tokens = snapask::ui::ThemeTokens::resolve(
            modeForPalette(palette()));
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal x = (width() - 1.0) * 0.5;
        QLinearGradient gradient(0.0, 5.0, 0.0, height() - 5.0);
        QColor transparent = tokens.glassEdgeBright;
        transparent.setAlpha(0);
        gradient.setColorAt(0.0, transparent);
        gradient.setColorAt(0.28, tokens.glassEdgeBright);
        gradient.setColorAt(0.72, tokens.glassEdgeDim);
        gradient.setColorAt(1.0, transparent);
        QPen pen(gradient, 1.0);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawLine(QPointF(x, 5.0), QPointF(x, height() - 5.0));
    }
};

}  // namespace

GlassToolbar::GlassToolbar(QWidget* parent)
    : GlassSurface(parent)
{
    setObjectName(QStringLiteral("GlassToolbar"));
    setMaterialRole(GlassMaterialRole::Elevated);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setMinimumHeight(48);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 6, 8, 8);
    layout_->setSpacing(2);
}

GlassToolButton* GlassToolbar::addAction(QAction* action)
{
    if (action == nullptr) {
        return nullptr;
    }
    auto* button = new GlassToolButton(this);
    button->setDefaultAction(action);
    button->setFixedSize(buttonExtent_, buttonExtent_);
    button->setAccessibleName(action->text());
    if (!action->objectName().isEmpty()) {
        button->setObjectName(action->objectName() + QStringLiteral("Button"));
    }
    button->installEventFilter(this);
    layout_->addWidget(button);
    return button;
}

void GlassToolbar::addWidget(QWidget* widget, const int stretch)
{
    if (widget == nullptr) {
        return;
    }
    widget->setParent(this);
    widget->installEventFilter(this);
    layout_->addWidget(widget, stretch);
}

void GlassToolbar::addSeparator()
{
    auto* separator = new ToolbarSeparator(this);
    separator->installEventFilter(this);
    layout_->addWidget(separator);
}

void GlassToolbar::addStretch(const int stretch)
{
    layout_->addStretch(stretch);
}

void GlassToolbar::setButtonExtent(const int extent)
{
    const int bounded = std::clamp(extent, 32, 44);
    if (buttonExtent_ == bounded) {
        return;
    }
    buttonExtent_ = bounded;
    const auto buttons = findChildren<GlassToolButton*>(
        {},
        Qt::FindDirectChildrenOnly);
    for (GlassToolButton* button : buttons) {
        button->setFixedSize(buttonExtent_, buttonExtent_);
    }
    updateGeometry();
}

int GlassToolbar::buttonExtent() const noexcept
{
    return buttonExtent_;
}

QSize GlassToolbar::sizeHint() const
{
    return layout_->sizeHint().expandedTo(QSize(48, 48));
}

QSize GlassToolbar::minimumSizeHint() const
{
    return layout_->minimumSize().expandedTo(QSize(48, 48));
}

bool GlassToolbar::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* widget = qobject_cast<QWidget*>(watched);
        widget != nullptr && event != nullptr) {
        observePointerFrom(widget, event);
    }
    return GlassSurface::eventFilter(watched, event);
}

QRectF GlassToolbar::glassRect() const
{
    return QRectF(rect()).adjusted(4.0, 3.0, -4.0, -5.0);
}

void GlassToolbar::observePointerFrom(QWidget* widget, QEvent* event)
{
    if (event->type() == QEvent::MouseMove) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        setPointerPosition(
            widget->mapTo(this, mouseEvent->position().toPoint()));
    } else if (event->type() == QEvent::Enter) {
        setHoverProgress(1.0);
    }
}

}  // namespace snapask::ui::glass
