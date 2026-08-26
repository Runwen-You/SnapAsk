#include "ui/capture/CaptureOverlay.h"

#include "platform/windows/MonitorCoordinateMapper.h"
#include "platform/windows/WindowBackdrop.h"
#include "ui/common/GlyphIcon.h"
#include "ui/glass/GlassToolbar.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QShowEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace snapask::ui::capture {

namespace {

constexpr int hoverRefreshIntervalMs = 34;

[[nodiscard]] Qt::CursorShape cursorForHandle(snapask::capture::SelectionHandle handle) noexcept
{
    using snapask::capture::SelectionHandle;
    switch (handle) {
    case SelectionHandle::NorthWest:
    case SelectionHandle::SouthEast:
        return Qt::SizeFDiagCursor;
    case SelectionHandle::NorthEast:
    case SelectionHandle::SouthWest:
        return Qt::SizeBDiagCursor;
    case SelectionHandle::North:
    case SelectionHandle::South:
        return Qt::SizeVerCursor;
    case SelectionHandle::East:
    case SelectionHandle::West:
        return Qt::SizeHorCursor;
    case SelectionHandle::Move:
        return Qt::SizeAllCursor;
    case SelectionHandle::None:
        return Qt::CrossCursor;
    }
    return Qt::CrossCursor;
}

[[nodiscard]] QRect centeredSquare(const QPoint& center, int radius) noexcept
{
    return QRect(center - QPoint(radius, radius), QSize((radius * 2) + 1, (radius * 2) + 1));
}

}  // namespace

CaptureOverlay::CaptureOverlay(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(tr("SnapAsk 截图选择"));
    setAccessibleName(tr("SnapAsk 截图选择"));
    setWindowFlags(
        Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint
        | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    buildActionBar();
}

bool CaptureOverlay::beginCapture(snapask::capture::CaptureFrame frame, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!frame.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("CaptureOverlay requires a valid physical-pixel desktop frame.");
        }
        return false;
    }

    frame_ = std::move(frame);
    selection_.setBounds(frame_.desktopRectPx());
    selection_.setMinimumSize({2, 2});
    selection_.clearSelection();
    hoveredWindow_.reset();
    clickCandidatePx_.reset();
    hoverRefreshTimer_.invalidate();
    pointerDragged_ = false;
    handoffAction_ = CaptureHandoffAction::Edit;
    captureActive_ = true;
    actionBar_->clearBackdropImage();
    actionBar_->hide();

    show();
    applyPhysicalGeometry();
    raise();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);

    const QPoint cursor = cursorDesktopPx();
    updateHoverWindow(cursor, true);
    updateCursor(cursor);
    update();
    return true;
}

void CaptureOverlay::cancelCapture()
{
    if (!captureActive_) {
        return;
    }
    captureActive_ = false;
    selection_.clearSelection();
    actionBar_->clearBackdropImage();
    frame_ = {};
    hoveredWindow_.reset();
    clickCandidatePx_.reset();
    actionBar_->hide();
    hide();
    emit captureCancelled();
}

void CaptureOverlay::confirmSelection()
{
    confirmSelectionWithAction(CaptureHandoffAction::Edit);
}

void CaptureOverlay::confirmSelectionWithAction(const CaptureHandoffAction action)
{
    if (!captureActive_ || !selection_.hasSelection()) {
        return;
    }

    QString error;
    const auto crop = frame_.cropPixels(selection_.selectionPx(), &error);
    if (!crop.has_value()) {
        emit captureFailed(error);
        return;
    }

    captureActive_ = false;
    handoffAction_ = action;
    selection_.clearSelection();
    actionBar_->clearBackdropImage();
    frame_ = {};
    hoveredWindow_.reset();
    clickCandidatePx_.reset();
    actionBar_->hide();
    hide();
    emit captureConfirmed(*crop);
}

CaptureHandoffAction CaptureOverlay::takeHandoffAction() noexcept
{
    const CaptureHandoffAction result = handoffAction_;
    handoffAction_ = CaptureHandoffAction::Edit;
    return result;
}

bool CaptureOverlay::isCaptureActive() const noexcept
{
    return captureActive_;
}

QRect CaptureOverlay::selectionPx() const noexcept
{
    return selection_.selectionPx();
}

void CaptureOverlay::setSelectionPx(const QRect& desktopRectPx)
{
    if (!captureActive_) {
        return;
    }
    const QRect previous = selection_.selectionPx();
    selection_.setSelection(desktopRectPx);
    hoveredWindow_.reset();
    emitSelectionIfChanged(previous);
    updateCursor(cursorDesktopPx());
    updateActionBarGeometry();
    update();
}

void CaptureOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    if (!frame_.isValid()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }

    const QRect desktopRect = frame_.desktopRectPx();
    const QTransform viewTransform = desktopToWidgetTransform();
    painter.setTransform(viewTransform);
    painter.drawImage(desktopRect, frame_.image(), frame_.image().rect());
    painter.fillRect(desktopRect, QColor(0, 0, 0, 105));

    const bool hasSelection = selection_.hasSelection();
    QRect highlightedRect;
    if (hasSelection) {
        highlightedRect = selection_.selectionPx();
    } else if (hoveredWindow_.has_value()) {
        highlightedRect = hoveredWindow_->framePx.intersected(desktopRect);
    }

    if (highlightedRect.isValid() && !highlightedRect.isEmpty()) {
        const QRect sourceRect = highlightedRect.translated(-desktopRect.topLeft());
        painter.drawImage(highlightedRect, frame_.image(), sourceRect);

        QPen border(hasSelection ? QColor(42, 134, 255) : QColor(255, 255, 255));
        border.setWidth(2);
        border.setCosmetic(true);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(highlightedRect.adjusted(0, 0, -1, -1));
    }

    if (hasSelection && !selection_.creatingSelection()) {
        const int radius = handleRadiusPx(selection_.selectionPx().center());
        QPen handleBorder(QColor(22, 90, 190));
        handleBorder.setWidth(1);
        handleBorder.setCosmetic(true);
        painter.setPen(handleBorder);
        painter.setBrush(QColor(245, 249, 255));
        for (const QPoint& center : snapask::capture::SelectionModel::handleCenters(
                 selection_.selectionPx())) {
            painter.drawRect(centeredSquare(center, radius));
        }

        painter.resetTransform();
        const QString dimensions = QStringLiteral("%1 × %2 px")
                                       .arg(selection_.selectionPx().width())
                                       .arg(selection_.selectionPx().height());
        const QFontMetrics metrics(font());
        const QSize textSize = metrics.size(Qt::TextSingleLine, dimensions);
        QPointF anchor = viewTransform.map(selection_.selectionPx().bottomLeft())
            + QPointF(0.0, 10.0);
        QRect labelRect(
            qRound(anchor.x()),
            qRound(anchor.y()),
            textSize.width() + 16,
            textSize.height() + 8);
        if (labelRect.bottom() > height()) {
            const QPointF above = viewTransform.map(selection_.selectionPx().topLeft())
                - QPointF(0.0, labelRect.height() + 8.0);
            labelRect.moveTopLeft(QPoint(qRound(above.x()), qRound(above.y())));
        }
        labelRect.moveLeft(std::clamp(labelRect.left(), 0, std::max(0, width() - labelRect.width())));
        labelRect.moveTop(std::clamp(labelRect.top(), 0, std::max(0, height() - labelRect.height())));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 23, 29, 225));
        painter.drawRoundedRect(labelRect, 6, 6);
        painter.setPen(Qt::white);
        painter.drawText(labelRect, Qt::AlignCenter, dimensions);
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent* event)
{
    if (!captureActive_ || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint point = cursorDesktopPx();
    pointerPressPx_ = point;
    pointerDragged_ = false;
    clickCandidatePx_.reset();

    const int radius = handleRadiusPx(point);
    const auto handle = snapask::capture::SelectionModel::hitTest(
        selection_.selectionPx(),
        point,
        radius);
    bool started = false;
    if (handle != snapask::capture::SelectionHandle::None) {
        started = selection_.beginTransform(handle, point);
    } else {
        QSet<quintptr> exclusions;
        exclusions.insert(static_cast<quintptr>(winId()));
        if (const auto candidate = windowPicker_.windowAt(point, exclusions, true);
            candidate.has_value()) {
            const QRect clipped = candidate->framePx.intersected(frame_.desktopRectPx());
            if (clipped.isValid() && !clipped.isEmpty()) {
                clickCandidatePx_ = clipped;
            }
        }
        started = selection_.beginCreate(point);
    }

    if (started) {
        actionBar_->hide();
        hoveredWindow_.reset();
        emit selectionChanged(selection_.selectionPx());
        updateCursor(point);
        update();
        event->accept();
    }
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!captureActive_) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint point = cursorDesktopPx();
    if (selection_.interactionActive()) {
        if ((point - pointerPressPx_).manhattanLength() >= dragThresholdPx(pointerPressPx_)) {
            pointerDragged_ = true;
        }
        const QRect previous = selection_.selectionPx();
        static_cast<void>(selection_.updateInteraction(point));
        emitSelectionIfChanged(previous);
        updateCursor(point);
        update();
        event->accept();
        return;
    }

    if (!selection_.hasSelection()) {
        updateHoverWindow(point);
    }
    updateCursor(point);
    QWidget::mouseMoveEvent(event);
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (!captureActive_ || event->button() != Qt::LeftButton
        || !selection_.interactionActive()) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    const bool wasCreating = selection_.creatingSelection();
    const QRect previous = selection_.selectionPx();
    static_cast<void>(selection_.updateInteraction(cursorDesktopPx()));
    selection_.commitInteraction();
    if (wasCreating && !pointerDragged_ && clickCandidatePx_.has_value()) {
        selection_.setSelection(*clickCandidatePx_);
    }
    clickCandidatePx_.reset();
    emitSelectionIfChanged(previous);
    updateCursor(cursorDesktopPx());
    updateActionBarGeometry();
    update();
    event->accept();
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (captureActive_ && event->button() == Qt::LeftButton && selection_.hasSelection()
        && selection_.selectionPx().contains(cursorDesktopPx())) {
        confirmSelection();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CaptureOverlay::keyPressEvent(QKeyEvent* event)
{
    if (!captureActive_) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        escapeOneLevel();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        confirmSelection();
        event->accept();
        return;
    }

    QPoint delta;
    const int distance = event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
    switch (event->key()) {
    case Qt::Key_Left:
        delta.setX(-distance);
        break;
    case Qt::Key_Right:
        delta.setX(distance);
        break;
    case Qt::Key_Up:
        delta.setY(-distance);
        break;
    case Qt::Key_Down:
        delta.setY(distance);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (selection_.hasSelection()) {
        const QRect previous = selection_.selectionPx();
        const QRect moved = frame_.monitorLayout().clampToDesktop(previous.translated(delta));
        selection_.setSelection(moved);
        emitSelectionIfChanged(previous);
        update();
    }
    event->accept();
}

void CaptureOverlay::closeEvent(QCloseEvent* event)
{
    const bool wasActive = captureActive_;
    captureActive_ = false;
    selection_.clearSelection();
    actionBar_->clearBackdropImage();
    frame_ = {};
    hoveredWindow_.reset();
    clickCandidatePx_.reset();
    actionBar_->hide();
    event->accept();
    if (wasActive) {
        emit captureCancelled();
    }
}

void CaptureOverlay::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (captureActive_) {
        applyPhysicalGeometry();
        updateActionBarGeometry();
    }
}

bool CaptureOverlay::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)
    if (captureActive_ && message != nullptr) {
        const auto* nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage->message == WM_DPICHANGED || nativeMessage->message == WM_DISPLAYCHANGE) {
            QTimer::singleShot(0, this, &CaptureOverlay::applyPhysicalGeometry);
        }
    }
    return false;
}

QPoint CaptureOverlay::cursorDesktopPx() const noexcept
{
    bool ok = false;
    QPoint point = snapask::platform::windows::MonitorCoordinateMapper::cursorPositionPx(&ok);
    if (!ok || !frame_.desktopRectPx().isValid()) {
        return frame_.desktopRectPx().topLeft();
    }
    point.setX(std::clamp(point.x(), frame_.desktopRectPx().left(), frame_.desktopRectPx().right()));
    point.setY(std::clamp(point.y(), frame_.desktopRectPx().top(), frame_.desktopRectPx().bottom()));
    return point;
}

int CaptureOverlay::handleRadiusPx(const QPoint& desktopPointPx) const noexcept
{
    const auto* monitor = frame_.monitorLayout().monitorAt(desktopPointPx);
    const qreal scale = monitor != nullptr ? std::max(monitor->scaleX(), monitor->scaleY()) : 1.0;
    return std::max(4, qRound(5.0 * scale));
}

int CaptureOverlay::dragThresholdPx(const QPoint& desktopPointPx) const noexcept
{
    const auto* monitor = frame_.monitorLayout().monitorAt(desktopPointPx);
    const qreal scale = monitor != nullptr ? std::max(monitor->scaleX(), monitor->scaleY()) : 1.0;
    return std::max(4, qRound(4.0 * scale));
}

QTransform CaptureOverlay::desktopToWidgetTransform() const noexcept
{
    const QRect desktop = frame_.desktopRectPx();
    if (desktop.isEmpty() || width() <= 0 || height() <= 0) {
        return {};
    }
    const qreal scaleX = static_cast<qreal>(width()) / static_cast<qreal>(desktop.width());
    const qreal scaleY = static_cast<qreal>(height()) / static_cast<qreal>(desktop.height());
    return {
        scaleX,
        0.0,
        0.0,
        scaleY,
        -static_cast<qreal>(desktop.x()) * scaleX,
        -static_cast<qreal>(desktop.y()) * scaleY,
    };
}

void CaptureOverlay::applyPhysicalGeometry()
{
    if (!captureActive_ || !frame_.desktopRectPx().isValid()) {
        return;
    }
    const QRect desktop = frame_.desktopRectPx();
    const HWND window = reinterpret_cast<HWND>(winId());
    SetWindowPos(
        window,
        HWND_TOPMOST,
        desktop.x(),
        desktop.y(),
        desktop.width(),
        desktop.height(),
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    updateActionBarGeometry();
}

void CaptureOverlay::updateHoverWindow(const QPoint& desktopPointPx, bool force)
{
    if (!captureActive_ || selection_.hasSelection() || selection_.interactionActive()) {
        return;
    }
    if (!force && hoverRefreshTimer_.isValid()
        && hoverRefreshTimer_.elapsed() < hoverRefreshIntervalMs) {
        return;
    }
    hoverRefreshTimer_.restart();

    QSet<quintptr> exclusions;
    exclusions.insert(static_cast<quintptr>(winId()));
    const auto previous = hoveredWindow_;
    hoveredWindow_ = windowPicker_.windowAt(desktopPointPx, exclusions, true);
    if (hoveredWindow_.has_value()) {
        hoveredWindow_->framePx = hoveredWindow_->framePx.intersected(frame_.desktopRectPx());
        if (hoveredWindow_->framePx.isEmpty()) {
            hoveredWindow_.reset();
        }
    }

    const QRect oldRect = previous.has_value() ? previous->framePx : QRect{};
    const QRect newRect = hoveredWindow_.has_value() ? hoveredWindow_->framePx : QRect{};
    if (oldRect != newRect) {
        update();
    }
}

void CaptureOverlay::updateCursor(const QPoint& desktopPointPx)
{
    if (!captureActive_) {
        unsetCursor();
        return;
    }
    if (selection_.interactionActive()) {
        const auto active = selection_.creatingSelection()
            ? snapask::capture::SelectionHandle::None
            : selection_.activeHandle();
        setCursor(cursorForHandle(active));
        return;
    }
    setCursor(cursorForHandle(snapask::capture::SelectionModel::hitTest(
        selection_.selectionPx(),
        desktopPointPx,
        handleRadiusPx(desktopPointPx))));
}

void CaptureOverlay::emitSelectionIfChanged(const QRect& previousSelection)
{
    if (previousSelection != selection_.selectionPx()) {
        emit selectionChanged(selection_.selectionPx());
    }
}

void CaptureOverlay::escapeOneLevel()
{
    if (selection_.interactionActive()) {
        const QRect previous = selection_.selectionPx();
        selection_.cancelInteraction();
        clickCandidatePx_.reset();
        emitSelectionIfChanged(previous);
        updateCursor(cursorDesktopPx());
        updateActionBarGeometry();
        update();
        return;
    }
    if (selection_.hasSelection()) {
        selection_.clearSelection();
        actionBar_->hide();
        emit selectionChanged({});
        updateHoverWindow(cursorDesktopPx(), true);
        updateCursor(cursorDesktopPx());
        update();
        return;
    }
    cancelCapture();
}

void CaptureOverlay::buildActionBar()
{
    actionBar_ = new snapask::ui::glass::GlassToolbar(this);
    actionBar_->setObjectName(QStringLiteral("CaptureActionBar"));
    actionBar_->setBackdropMode(
        snapask::ui::glass::GlassBackdropMode::Image);
    actionBar_->setButtonExtent(36);

    const QColor foreground = palette().color(QPalette::WindowText);
    auto* toolGroup = new QActionGroup(actionBar_);
    toolGroup->setExclusive(true);
    const auto addButton = [this, foreground, toolGroup](
                               const snapask::ui::Glyph glyph,
                               const QString& tooltip,
                               const QString& objectName,
                               const CaptureHandoffAction action,
                               const bool selectable = false,
                               const bool selected = false) {
        auto* visualAction = new QAction(
            snapask::ui::glyphIcon(glyph, foreground),
            tooltip,
            actionBar_);
        visualAction->setObjectName(objectName);
        visualAction->setToolTip(tooltip);
        if (selectable) {
            visualAction->setCheckable(true);
            visualAction->setChecked(selected);
            toolGroup->addAction(visualAction);
        }
        connect(visualAction, &QAction::triggered, this, [this, action] {
            confirmSelectionWithAction(action);
        });
        (void)actionBar_->addAction(visualAction);
    };
    addButton(
        Glyph::Select,
        tr("进入编辑"),
        QStringLiteral("captureEditAction"),
        CaptureHandoffAction::Edit,
        true,
        true);
    addButton(
        Glyph::Rectangle,
        tr("矩形"),
        QStringLiteral("captureRectangleAction"),
        CaptureHandoffAction::Rectangle,
        true);
    addButton(
        Glyph::Arrow,
        tr("箭头"),
        QStringLiteral("captureArrowAction"),
        CaptureHandoffAction::Arrow,
        true);
    addButton(
        Glyph::Text,
        tr("文字"),
        QStringLiteral("captureTextAction"),
        CaptureHandoffAction::Text,
        true);
    addButton(
        Glyph::Mosaic,
        tr("马赛克"),
        QStringLiteral("captureMosaicAction"),
        CaptureHandoffAction::Mosaic,
        true);

    actionBar_->addSeparator();

    addButton(
        Glyph::Copy,
        tr("复制截图"),
        QStringLiteral("captureCopyAction"),
        CaptureHandoffAction::Copy);
    addButton(
        Glyph::Save,
        tr("保存截图"),
        QStringLiteral("captureSaveAction"),
        CaptureHandoffAction::Save);
    addButton(
        Glyph::Pin,
        tr("贴图"),
        QStringLiteral("capturePinAction"),
        CaptureHandoffAction::Pin);
    addButton(
        Glyph::Ask,
        tr("向 AI 提问"),
        QStringLiteral("captureAskAction"),
        CaptureHandoffAction::Ask);

    auto* closeAction = new QAction(
        glyphIcon(Glyph::Close, foreground),
        tr("取消截图"),
        actionBar_);
    closeAction->setObjectName(QStringLiteral("captureCloseAction"));
    closeAction->setToolTip(tr("取消截图"));
    connect(closeAction, &QAction::triggered, this, &CaptureOverlay::cancelCapture);
    (void)actionBar_->addAction(closeAction);
    actionBar_->adjustSize();
    actionBar_->hide();
}

void CaptureOverlay::updateActionBarGeometry()
{
    if (actionBar_ == nullptr || !captureActive_
        || !selection_.hasSelection() || selection_.interactionActive()) {
        if (actionBar_ != nullptr) {
            actionBar_->hide();
        }
        return;
    }

    actionBar_->adjustSize();
    const QRect selectionRect = desktopToWidgetTransform().mapRect(
        QRectF(selection_.selectionPx())).toAlignedRect();
    const QSize barSize = actionBar_->sizeHint();
    int x = selectionRect.right() - barSize.width() + 1;
    int y = selectionRect.bottom() + 9;
    if (y + barSize.height() > height()) {
        y = selectionRect.top() - barSize.height() - 9;
    }
    x = std::clamp(x, 6, std::max(6, width() - barSize.width() - 6));
    y = std::clamp(y, 6, std::max(6, height() - barSize.height() - 6));
    actionBar_->setGeometry(QRect(QPoint(x, y), barSize));
    updateActionBarBackdrop();
    actionBar_->show();
    actionBar_->raise();
}

void CaptureOverlay::updateActionBarBackdrop()
{
    if (actionBar_ == nullptr || !frame_.isValid()) {
        if (actionBar_ != nullptr) {
            actionBar_->clearBackdropImage();
        }
        return;
    }
    const bool fallback =
        snapask::platform::windows::WindowBackdrop::isHighContrastEnabled()
        || snapask::platform::windows::WindowBackdrop::isRemoteSession()
        || !snapask::platform::windows::WindowBackdrop::
                isDesktopCompositionEnabled();
    actionBar_->setFallbackEnabled(fallback);

    bool invertible = false;
    const QTransform widgetToDesktop =
        desktopToWidgetTransform().inverted(&invertible);
    if (!invertible) {
        actionBar_->clearBackdropImage();
        return;
    }
    const QRect desktopRect = widgetToDesktop
                                  .mapRect(QRectF(actionBar_->geometry()))
                                  .toAlignedRect();
    const QRect sourceRect = desktopRect
                                 .translated(-frame_.desktopRectPx().topLeft())
                                 .intersected(frame_.image().rect());
    if (sourceRect.isEmpty()) {
        actionBar_->clearBackdropImage();
        return;
    }
    actionBar_->setBackdropImage(
        frame_.image(),
        sourceRect,
        static_cast<quint64>(frame_.image().cacheKey()));
}

}  // namespace snapask::ui::capture
