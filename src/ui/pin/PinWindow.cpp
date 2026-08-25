#include "ui/pin/PinWindow.h"

#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace snapask::ui::pin {

namespace {

constexpr qreal minimumScale = 0.10;
constexpr qreal maximumScale = 8.0;
constexpr qreal wheelScaleStep = 1.10;

}  // namespace

PinWindow::PinWindow(const snapask::RenderedSnapshot& snapshot)
    : snapshot_(snapshot)
{
    setWindowFlags(
        Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint
        | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
    setWindowTitle(tr("置顶截图"));

    if (!snapshot_.isValid()) {
        resize(1, 1);
        return;
    }

    QSize availableSize = snapshot_.pixelSize();
    if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos()); screen != nullptr) {
        availableSize = screen->availableGeometry().size() * 0.75;
    } else if (QScreen* primary = QGuiApplication::primaryScreen(); primary != nullptr) {
        availableSize = primary->availableGeometry().size() * 0.75;
    }

    const QSize pixels = snapshot_.pixelSize();
    displayScale_ = std::min<qreal>(
        1.0,
        std::min(
            static_cast<qreal>(availableSize.width()) / pixels.width(),
            static_cast<qreal>(availableSize.height()) / pixels.height()));
    displayScale_ = std::clamp(displayScale_, minimumScale, maximumScale);
    resize(scaledSize(displayScale_));
}

const snapask::RenderedSnapshot& PinWindow::snapshot() const noexcept
{
    return snapshot_;
}

bool PinWindow::isAlwaysOnTop() const noexcept
{
    return alwaysOnTop_;
}

qreal PinWindow::displayScale() const noexcept
{
    return displayScale_;
}

void PinWindow::setAlwaysOnTop(bool enabled)
{
    if (alwaysOnTop_ == enabled) {
        return;
    }

    const QPoint previousPosition = pos();
    const bool wasVisible = isVisible();
    alwaysOnTop_ = enabled;
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    if (wasVisible) {
        show();
        move(previousPosition);
        raise();
    }
    emit alwaysOnTopChanged(enabled);
}

void PinWindow::toggleAlwaysOnTop()
{
    setAlwaysOnTop(!alwaysOnTop_);
}

void PinWindow::resetToActualPixels()
{
    if (!snapshot_.isValid()) {
        return;
    }
    resizeAroundGlobalPoint(1.0, frameGeometry().center());
}

void PinWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(24, 24, 24));
    if (snapshot_.isValid()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, displayScale_ < 1.0);
        painter.drawImage(rect(), snapshot_.image(), snapshot_.image().rect());
    }
}

void PinWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_ && event->buttons().testFlag(Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PinWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        resetToActualPixels();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PinWindow::wheelEvent(QWheelEvent* event)
{
    if (!snapshot_.isValid() || event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    const qreal steps = static_cast<qreal>(event->angleDelta().y()) / 120.0;
    const qreal requested = displayScale_ * std::pow(wheelScaleStep, steps);
    resizeAroundGlobalPoint(requested, event->globalPosition().toPoint());
    event->accept();
}

void PinWindow::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    QAction* topAction = menu.addAction(tr("始终置顶"));
    topAction->setCheckable(true);
    topAction->setChecked(alwaysOnTop_);
    connect(topAction, &QAction::toggled, this, &PinWindow::setAlwaysOnTop);

    QAction* actualPixelsAction = menu.addAction(tr("实际像素大小"));
    connect(actualPixelsAction, &QAction::triggered, this, &PinWindow::resetToActualPixels);
    menu.addSeparator();
    QAction* closeAction = menu.addAction(tr("关闭"));
    connect(closeAction, &QAction::triggered, this, &QWidget::close);
    menu.exec(event->globalPos());
    event->accept();
}

void PinWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_T) {
        toggleAlwaysOnTop();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PinWindow::closeEvent(QCloseEvent* event)
{
    dragging_ = false;
    event->accept();
}

QSize PinWindow::scaledSize(qreal scale) const noexcept
{
    if (!snapshot_.isValid()) {
        return {1, 1};
    }
    return {
        std::max(1, qRound(snapshot_.pixelSize().width() * scale)),
        std::max(1, qRound(snapshot_.pixelSize().height() * scale)),
    };
}

void PinWindow::resizeAroundGlobalPoint(qreal scale, const QPoint& globalAnchor)
{
    if (!snapshot_.isValid()) {
        return;
    }

    scale = std::clamp(scale, minimumScale, maximumScale);
    if (qFuzzyCompare(scale, displayScale_)) {
        return;
    }

    const QSize previousSize = size();
    const QPoint localAnchor = mapFromGlobal(globalAnchor);
    const qreal xRatio = previousSize.width() > 0
        ? static_cast<qreal>(localAnchor.x()) / previousSize.width()
        : 0.5;
    const qreal yRatio = previousSize.height() > 0
        ? static_cast<qreal>(localAnchor.y()) / previousSize.height()
        : 0.5;

    displayScale_ = scale;
    const QSize nextSize = scaledSize(displayScale_);
    const QPoint nextLocalAnchor(
        qRound(xRatio * nextSize.width()),
        qRound(yRatio * nextSize.height()));
    resize(nextSize);
    move(globalAnchor - nextLocalAnchor);
    update();
}

}  // namespace snapask::ui::pin
