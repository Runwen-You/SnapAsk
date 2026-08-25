#pragma once

#include <QImage>
#include <QMainWindow>
#include <QRect>
#include <QString>

#include <memory>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QFont;
class QFontComboBox;
class QLabel;
class QToolBar;
class QShowEvent;

namespace snapask {
class RenderedSnapshot;
class ScreenshotSession;
}

namespace snapask::ui::canvas {
class CanvasWidget;
enum class CanvasTool;
}

namespace snapask::ui::pin {
class PinWindow;
}

namespace snapask::ui::editor {

struct SnapshotRenderState;

class EditorWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit EditorWindow(QImage sourceImage, QWidget* parent = nullptr);
    explicit EditorWindow(
        std::unique_ptr<snapask::ScreenshotSession> session,
        QWidget* parent = nullptr);
    ~EditorWindow() override;

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    [[nodiscard]] snapask::ScreenshotSession& session() noexcept;
    [[nodiscard]] const snapask::ScreenshotSession& session() const noexcept;
    [[nodiscard]] snapask::ui::canvas::CanvasWidget* canvasWidget() const noexcept;
    void setGenerationActive(bool active);
    [[nodiscard]] bool isGenerationActive() const noexcept;
    void setCaptureDesktopRectPx(const QRect& desktopRectPx);
    void activateTool(snapask::ui::canvas::CanvasTool tool);

    // The cache is revision-bound. Save, clipboard, pin and explicit AI send
    // all consume this one immutable value instead of independently rendering.
    [[nodiscard]] const snapask::RenderedSnapshot& currentRenderedSnapshot();
    [[nodiscard]] bool copyCurrentSnapshot(QString* error = nullptr);
    [[nodiscard]] bool saveCurrentSnapshot(
        const QString& filePath,
        QString* error = nullptr);
    [[nodiscard]] snapask::ui::pin::PinWindow* pinCurrentSnapshot(
        QString* error = nullptr);

signals:
    void askRequested();
    // Emitted only after the screenshot content (crop or annotation model)
    // changes. Selection-only editor chrome is intentionally excluded.
    void snapshotContentChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void chooseColor();
    void chooseSaveLocation();
    void copyFromAction();
    void pinFromAction();
    void clearAnnotations();
    void restoreOriginal();
    void markContentChanged();

private:
    void buildToolbar();
    void setCanvasTool(snapask::ui::canvas::CanvasTool tool);
    void applyLineWidth(qreal width);
    void applyTextFont(const QFont& font);
    void updateColorActionIcon();
    void showOperationError(const QString& title, const QString& error);
    void setStatusText(const QString& text);
    void applyCaptureDesktopGeometry();

    std::unique_ptr<snapask::ScreenshotSession> session_;
    std::unique_ptr<snapask::RenderedSnapshot> renderedSnapshotCache_;
    std::unique_ptr<SnapshotRenderState> snapshotRenderState_;
    snapask::ui::canvas::CanvasWidget* canvas_{nullptr};
    QToolBar* toolbar_{nullptr};
    QActionGroup* toolActionGroup_{nullptr};
    QAction* colorAction_{nullptr};
    QAction* clearAction_{nullptr};
    QAction* restoreAction_{nullptr};
    QComboBox* lineWidthCombo_{nullptr};
    QFontComboBox* fontCombo_{nullptr};
    QLabel* statusLabel_{nullptr};
    QRect captureDesktopRectPx_;
    bool generationActive_{false};
};

}  // namespace snapask::ui::editor
