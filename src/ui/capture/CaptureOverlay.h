#pragma once

#include "domain/capture/CaptureFrame.h"
#include "domain/capture/SelectionModel.h"
#include "platform/windows/WindowPicker.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QRect>
#include <QTransform>
#include <QWidget>

#include <optional>

class QCloseEvent;
class QFrame;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace snapask::ui::capture {

enum class CaptureHandoffAction {
    Edit,
    Rectangle,
    Arrow,
    Text,
    Mosaic,
    Copy,
    Save,
    Pin,
    Ask,
};

class CaptureOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit CaptureOverlay(QWidget* parent = nullptr);
    ~CaptureOverlay() override = default;

    CaptureOverlay(const CaptureOverlay&) = delete;
    CaptureOverlay& operator=(const CaptureOverlay&) = delete;

    [[nodiscard]] bool beginCapture(
        snapask::capture::CaptureFrame frame,
        QString* error = nullptr);
    void cancelCapture();
    void confirmSelection();
    [[nodiscard]] CaptureHandoffAction takeHandoffAction() noexcept;

    [[nodiscard]] bool isCaptureActive() const noexcept;
    [[nodiscard]] QRect selectionPx() const noexcept;
    void setSelectionPx(const QRect& desktopRectPx);

signals:
    void captureConfirmed(const snapask::capture::CaptureSelection& capture);
    void captureCancelled();
    void captureFailed(const QString& error);
    void selectionChanged(const QRect& desktopRectPx);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    [[nodiscard]] QPoint cursorDesktopPx() const noexcept;
    [[nodiscard]] int handleRadiusPx(const QPoint& desktopPointPx) const noexcept;
    [[nodiscard]] int dragThresholdPx(const QPoint& desktopPointPx) const noexcept;
    [[nodiscard]] QTransform desktopToWidgetTransform() const noexcept;

    void applyPhysicalGeometry();
    void updateHoverWindow(const QPoint& desktopPointPx, bool force = false);
    void updateCursor(const QPoint& desktopPointPx);
    void emitSelectionIfChanged(const QRect& previousSelection);
    void escapeOneLevel();
    void buildActionBar();
    void updateActionBarGeometry();
    void confirmSelectionWithAction(CaptureHandoffAction action);

    snapask::capture::CaptureFrame frame_;
    snapask::capture::SelectionModel selection_;
    snapask::platform::windows::WindowPicker windowPicker_;
    std::optional<snapask::platform::windows::TopLevelWindow> hoveredWindow_;
    std::optional<QRect> clickCandidatePx_;
    QElapsedTimer hoverRefreshTimer_;
    QFrame* actionBar_{nullptr};
    CaptureHandoffAction handoffAction_{CaptureHandoffAction::Edit};

    bool captureActive_{false};
    bool pointerDragged_{false};
    QPoint pointerPressPx_;
};

}  // namespace snapask::ui::capture
