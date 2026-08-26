#include "ui/editor/EditorWindow.h"

#include "domain/annotation/Annotation.h"
#include "domain/capture/ScreenshotSession.h"
#include "services/ClipboardService.h"
#include "services/SaveService.h"
#include "services/SnapshotRenderer.h"
#include "ui/canvas/CanvasWidget.h"
#include "ui/common/GlyphIcon.h"
#include "ui/glass/GlassToolbar.h"
#include "ui/pin/PinWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFuture>
#include <QFutureWatcher>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QEventLoop>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QRegion>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QUndoStack>
#include <QWindow>
#include <QtConcurrentRun>

#include <utility>

#include <QtCore/qt_windows.h>

namespace snapask::ui::editor {

struct SnapshotRenderState final {
    QFuture<snapask::SnapshotRenderResult> future;
    QUuid sessionId;
    quint64 revision{0};
};

namespace {

constexpr auto colorSettingsKey = "annotation/lastColor";
constexpr auto lineWidthSettingsKey = "annotation/lastLineWidth";
constexpr auto fontSettingsKey = "annotation/lastFont";

[[nodiscard]] QString pngPath(QString path)
{
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        path.append(QStringLiteral(".png"));
    }
    return path;
}

}  // namespace

EditorWindow::EditorWindow(QImage sourceImage, QWidget* parent)
    : EditorWindow(
          std::make_unique<snapask::ScreenshotSession>(std::move(sourceImage)),
          parent)
{
}

EditorWindow::EditorWindow(
    std::unique_ptr<snapask::ScreenshotSession> session,
    QWidget* parent)
    : QMainWindow(parent)
    , session_(std::move(session))
{
    if (session_ == nullptr) {
        session_ = std::make_unique<snapask::ScreenshotSession>();
    }

    setAttribute(Qt::WA_DeleteOnClose, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setWindowFlags(
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setWindowTitle(tr("SnapAsk 截图编辑[*]"));
    const QSize imageSize = session_->sourceImage().size();
    const int initialWidth = std::clamp(imageSize.width(), 480, 1120);
    const qreal scale = imageSize.width() > 0
        ? static_cast<qreal>(initialWidth) / imageSize.width() : 1.0;
    const int initialCanvasHeight = std::clamp(
        qRound(imageSize.height() * scale), 220, 720);
    resize(initialWidth, initialCanvasHeight);

    canvas_ = new snapask::ui::canvas::CanvasWidget(session_.get(), this);
    setCentralWidget(canvas_);
    buildToolbar();
    QTimer::singleShot(0, this, &EditorWindow::updateToolbarGeometry);

    statusLabel_ = new QLabel(this);
    statusLabel_->hide();
    statusBar()->hide();
    setStatusText(tr("当前截图尚未保存，尚未发送"));
    setWindowModified(true);

    connect(
        canvas_,
        &snapask::ui::canvas::CanvasWidget::contentChanged,
        this,
        &EditorWindow::markContentChanged);
    connect(
        canvas_,
        &snapask::ui::canvas::CanvasWidget::currentStyleChanged,
        this,
        [this](const snapask::AnnotationStyle&) { updateColorActionIcon(); });
}

void EditorWindow::setCaptureDesktopRectPx(const QRect& desktopRectPx)
{
    captureDesktopRectPx_ = desktopRectPx;
    if (isVisible()) {
        QTimer::singleShot(0, this, &EditorWindow::applyCaptureDesktopGeometry);
    }
}

void EditorWindow::activateTool(const snapask::ui::canvas::CanvasTool tool)
{
    setCanvasTool(tool);
    if (toolActionGroup_ == nullptr) {
        return;
    }
    for (QAction* action : toolActionGroup_->actions()) {
        if (action->data().toInt() == static_cast<int>(tool)) {
            action->setChecked(true);
            break;
        }
    }
}

EditorWindow::~EditorWindow()
{
    // ScreenshotSession is a C++ member while the canvas and undo/redo actions
    // are QObject children deleted later by QMainWindow. Disconnect and remove
    // the canvas first so QUndoStack::clear() cannot notify objects that still
    // point back into a session already being destroyed.
    if (session_ != nullptr) {
        session_->undoStack().disconnect();
    }
    QWidget* central = takeCentralWidget();
    canvas_ = nullptr;
    delete central;
}

snapask::ScreenshotSession& EditorWindow::session() noexcept
{
    return *session_;
}

const snapask::ScreenshotSession& EditorWindow::session() const noexcept
{
    return *session_;
}

snapask::ui::canvas::CanvasWidget* EditorWindow::canvasWidget() const noexcept
{
    return canvas_;
}

void EditorWindow::setGenerationActive(const bool active)
{
    generationActive_ = active;
    if (active) {
        setStatusText(tr("正在生成回答；截图与标注仍可继续编辑"));
    }
}

bool EditorWindow::isGenerationActive() const noexcept
{
    return generationActive_;
}

const snapask::RenderedSnapshot& EditorWindow::currentRenderedSnapshot()
{
    for (;;) {
        const quint64 expectedRevision = session_->currentRevision();
        const QUuid expectedSessionId = session_->sessionId();
        if (renderedSnapshotCache_ != nullptr
            && renderedSnapshotCache_->revision() == expectedRevision) {
            return *renderedSnapshotCache_;
        }

        if (snapshotRenderState_ == nullptr
            || snapshotRenderState_->sessionId != expectedSessionId
            || snapshotRenderState_->revision != expectedRevision) {
            snapask::SnapshotRenderInput input =
                snapask::SnapshotRenderer::freezeCurrent(*session_);
            SnapshotRenderState state;
            state.sessionId = input.sessionId;
            state.revision = input.revision;
            state.future = QtConcurrent::run(
                [input = std::move(input)]() mutable {
                    return snapask::SnapshotRenderer::renderFrozen(
                        std::move(input));
                });
            snapshotRenderState_ =
                std::make_unique<SnapshotRenderState>(std::move(state));
        }

        // Paints, timers and network events keep flowing while the lossless
        // worker runs. User edits are queued until this atomic export/send
        // boundary completes, so the frozen revision cannot race the canvas.
        const QFuture<snapask::SnapshotRenderResult> future =
            snapshotRenderState_->future;
        QFutureWatcher<snapask::SnapshotRenderResult> watcher;
        QEventLoop eventLoop;
        connect(
            &watcher,
            &QFutureWatcher<snapask::SnapshotRenderResult>::finished,
            &eventLoop,
            &QEventLoop::quit);
        watcher.setFuture(future);
        if (!watcher.isFinished()) {
            eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
        }

        snapask::SnapshotRenderResult result = future.result();
        if (snapshotRenderState_ != nullptr
            && snapshotRenderState_->sessionId == result.sessionId
            && snapshotRenderState_->revision == result.revision) {
            snapshotRenderState_.reset();
        }

        if (result.sessionId == session_->sessionId()
            && result.revision == session_->currentRevision()) {
            renderedSnapshotCache_ =
                std::make_unique<snapask::RenderedSnapshot>(
                    std::move(result.snapshot));
            return *renderedSnapshotCache_;
        }
    }
}

bool EditorWindow::copyCurrentSnapshot(QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    const snapask::RenderedSnapshot& snapshot = currentRenderedSnapshot();
    if (!snapshot.isValid()) {
        if (error != nullptr) {
            *error = tr("当前截图无法渲染。截图内容将保留在编辑器中。");
        }
        return false;
    }
    return snapask::ClipboardService::copy(snapshot, error);
}

bool EditorWindow::saveCurrentSnapshot(const QString& filePath, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    const snapask::RenderedSnapshot& snapshot = currentRenderedSnapshot();
    if (!snapshot.isValid()) {
        if (error != nullptr) {
            *error = tr("当前截图无法渲染。截图内容将保留在编辑器中。");
        }
        return false;
    }
    if (!snapask::SaveService::savePng(snapshot, filePath, error)) {
        return false;
    }
    session_->markSavedHash(snapshot.sha256());
    setWindowModified(false);
    setStatusText(tr("已保存；图片尚未发送"));
    return true;
}

snapask::ui::pin::PinWindow* EditorWindow::pinCurrentSnapshot(QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    const snapask::RenderedSnapshot& snapshot = currentRenderedSnapshot();
    if (!snapshot.isValid()) {
        if (error != nullptr) {
            *error = tr("当前截图无法渲染。截图内容将保留在编辑器中。");
        }
        return nullptr;
    }

    auto* pinWindow = new snapask::ui::pin::PinWindow(snapshot);
    pinWindow->show();
    pinWindow->raise();
    pinWindow->activateWindow();
    setStatusText(tr("已使用当前快照创建贴图；图片尚未发送"));
    return pinWindow;
}

void EditorWindow::closeEvent(QCloseEvent* event)
{
    if (!isWindowModified() && !generationActive_) {
        event->accept();
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        tr("关闭编辑器"),
        generationActive_
            ? tr("回答仍在生成，关闭将停止请求。当前截图和已收到的回答不会上传到其它地方。仍要关闭吗？")
            : tr("当前截图有尚未保存的修改。仍要关闭吗？"),
        QMessageBox::Close | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (answer == QMessageBox::Close) {
        event->accept();
    } else {
        event->ignore();
    }
}

void EditorWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    updateToolbarGeometry();
    if (captureDesktopRectPx_.isValid()) {
        QTimer::singleShot(0, this, &EditorWindow::applyCaptureDesktopGeometry);
    }
}

void EditorWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateToolbarGeometry();
}

bool EditorWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == toolbar_ && event != nullptr
        && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton
            && toolbar_->childAt(mouseEvent->position().toPoint()) == nullptr
            && windowHandle() != nullptr) {
            windowHandle()->startSystemMove();
            mouseEvent->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void EditorWindow::chooseColor()
{
    const QColor current = canvas_->currentStyle().strokeColor;
    const QColor selected = QColorDialog::getColor(
        current,
        this,
        tr("选择标注颜色"),
        QColorDialog::ShowAlphaChannel);
    if (!selected.isValid()) {
        return;
    }

    snapask::AnnotationStyle style = canvas_->currentStyle();
    style.strokeColor = selected;
    canvas_->setCurrentStyle(style);
    QSettings().setValue(QString::fromLatin1(colorSettingsKey), selected);
    updateColorActionIcon();
}

void EditorWindow::chooseSaveLocation()
{
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested = QStringLiteral("SnapAsk-%1.png")
                                  .arg(QDateTime::currentDateTime().toString(
                                      QStringLiteral("yyyyMMdd-HHmmss")));
    const QString selected = QFileDialog::getSaveFileName(
        this,
        tr("保存当前截图"),
        pictures + QLatin1Char('/') + suggested,
        tr("PNG 图片 (*.png)"));
    if (selected.isEmpty()) {
        return;
    }

    QString error;
    if (!saveCurrentSnapshot(pngPath(selected), &error)) {
        showOperationError(tr("保存失败"), error);
    }
}

void EditorWindow::copyFromAction()
{
    QString error;
    if (!copyCurrentSnapshot(&error)) {
        showOperationError(tr("复制失败"), error);
        return;
    }
    setStatusText(tr("已复制当前快照；图片尚未发送"));
}

void EditorWindow::pinFromAction()
{
    QString error;
    if (pinCurrentSnapshot(&error) == nullptr) {
        showOperationError(tr("贴图失败"), error);
    }
}

void EditorWindow::clearAnnotations()
{
    canvas_->clearAnnotations();
}

void EditorWindow::restoreOriginal()
{
    const QRect previousCrop = session_->cropRect();
    session_->restoreOriginalCrop();
    if (session_->cropRect() != previousCrop) {
        canvas_->refreshFromSession();
        markContentChanged();
    }
}

void EditorWindow::markContentChanged()
{
    renderedSnapshotCache_.reset();
    snapshotRenderState_.reset();
    setWindowModified(true);
    setStatusText(tr("图片已修改，尚未保存，尚未发送"));
    if (clearAction_ != nullptr) {
        clearAction_->setEnabled(!session_->annotations().isEmpty());
    }
    if (restoreAction_ != nullptr) {
        restoreAction_->setEnabled(
            session_->hasSourceImage() && session_->cropRect() != session_->sourceImage().rect());
    }
    scheduleToolbarBackdropUpdate();
    emit snapshotContentChanged();
}

void EditorWindow::buildToolbar()
{
    toolbar_ = new snapask::ui::glass::GlassToolbar(canvas_);
    toolbar_->setObjectName(QStringLiteral("editorToolbar"));
    toolbar_->setAccessibleName(tr("截图编辑工具"));
    toolbar_->setBackdropMode(
        snapask::ui::glass::GlassBackdropMode::Image);
    toolbar_->setButtonExtent(36);
    toolbar_->installEventFilter(this);

    const QColor iconColor = palette().color(QPalette::WindowText);

    toolActionGroup_ = new QActionGroup(this);
    toolActionGroup_->setExclusive(true);

    const auto addTool = [this, iconColor](
                             const QString& label,
                             const QString& objectName,
                             snapask::ui::canvas::CanvasTool tool) {
        snapask::ui::Glyph glyph = snapask::ui::Glyph::Select;
        if (tool == snapask::ui::canvas::CanvasTool::Rectangle) {
            glyph = snapask::ui::Glyph::Rectangle;
        } else if (tool == snapask::ui::canvas::CanvasTool::Arrow) {
            glyph = snapask::ui::Glyph::Arrow;
        } else if (tool == snapask::ui::canvas::CanvasTool::Text) {
            glyph = snapask::ui::Glyph::Text;
        } else if (tool == snapask::ui::canvas::CanvasTool::Mosaic) {
            glyph = snapask::ui::Glyph::Mosaic;
        }
        auto* action = new QAction(
            glyphIcon(glyph, iconColor),
            label,
            this);
        action->setObjectName(objectName);
        action->setCheckable(true);
        action->setData(static_cast<int>(tool));
        toolActionGroup_->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool] { setCanvasTool(tool); });
        (void)toolbar_->addAction(action);
        return action;
    };

    QAction* selectAction = addTool(
        tr("选择"),
        QStringLiteral("toolSelectAction"),
        snapask::ui::canvas::CanvasTool::Select);
    addTool(
        tr("矩形"),
        QStringLiteral("toolRectangleAction"),
        snapask::ui::canvas::CanvasTool::Rectangle);
    addTool(
        tr("箭头"),
        QStringLiteral("toolArrowAction"),
        snapask::ui::canvas::CanvasTool::Arrow);
    addTool(
        tr("文字"),
        QStringLiteral("toolTextAction"),
        snapask::ui::canvas::CanvasTool::Text);
    addTool(
        tr("马赛克"),
        QStringLiteral("toolMosaicAction"),
        snapask::ui::canvas::CanvasTool::Mosaic);
    selectAction->setChecked(true);

    toolbar_->addSeparator();
    colorAction_ = new QAction(
        glyphIcon(Glyph::Color, iconColor),
        tr("颜色"),
        this);
    colorAction_->setObjectName(QStringLiteral("annotationColorAction"));
    connect(colorAction_, &QAction::triggered, this, &EditorWindow::chooseColor);
    (void)toolbar_->addAction(colorAction_);

    lineWidthCombo_ = new QComboBox(toolbar_);
    lineWidthCombo_->setObjectName(QStringLiteral("annotationLineWidthCombo"));
    lineWidthCombo_->setAccessibleName(tr("标注线宽"));
    for (const qreal width : {1.0, 2.0, 3.0, 5.0, 8.0}) {
        lineWidthCombo_->addItem(tr("%1 px").arg(width), width);
    }
    lineWidthCombo_->hide();

    fontCombo_ = new QFontComboBox(toolbar_);
    fontCombo_->setObjectName(QStringLiteral("annotationFontCombo"));
    fontCombo_->setAccessibleName(tr("文字字体"));
    fontCombo_->setMaximumWidth(180);
    fontCombo_->hide();

    QSettings settings;
    snapask::AnnotationStyle initialStyle = canvas_->currentStyle();
    const QColor storedColor = settings.value(
        QString::fromLatin1(colorSettingsKey), initialStyle.strokeColor).value<QColor>();
    if (storedColor.isValid()) {
        initialStyle.strokeColor = storedColor;
    }
    const qreal storedWidth = settings.value(
        QString::fromLatin1(lineWidthSettingsKey), initialStyle.strokeWidth).toDouble();
    initialStyle.strokeWidth = storedWidth > 0.0 ? storedWidth : 3.0;
    const QVariant storedFontValue = settings.value(
        QString::fromLatin1(fontSettingsKey));
    if (storedFontValue.canConvert<QFont>()) {
        const QFont storedFont = storedFontValue.value<QFont>();
        if (!storedFont.family().trimmed().isEmpty()) {
            initialStyle.font = storedFont;
        }
    }
    canvas_->setCurrentStyle(initialStyle);
    const int storedWidthIndex = lineWidthCombo_->findData(initialStyle.strokeWidth);
    lineWidthCombo_->setCurrentIndex(storedWidthIndex >= 0 ? storedWidthIndex : 2);
    connect(
        lineWidthCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [this](int index) { applyLineWidth(lineWidthCombo_->itemData(index).toDouble()); });
    fontCombo_->setCurrentFont(initialStyle.font);
    connect(
        fontCombo_,
        &QFontComboBox::currentFontChanged,
        this,
        &EditorWindow::applyTextFont);
    updateColorActionIcon();

    toolbar_->addSeparator();
    QAction* undoAction = session_->undoStack().createUndoAction(this, tr("撤销"));
    undoAction->setIcon(glyphIcon(Glyph::Undo, iconColor));
    undoAction->setObjectName(QStringLiteral("undoAction"));
    undoAction->setShortcut(QKeySequence::Undo);
    (void)toolbar_->addAction(undoAction);
    QAction* redoAction = session_->undoStack().createRedoAction(this, tr("重做"));
    redoAction->setIcon(glyphIcon(Glyph::Redo, iconColor));
    redoAction->setObjectName(QStringLiteral("redoAction"));
    redoAction->setShortcuts(
        {QKeySequence::Redo, QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    (void)toolbar_->addAction(redoAction);

    clearAction_ = new QAction(
        glyphIcon(Glyph::Clear, iconColor),
        tr("清除标注"),
        this);
    clearAction_->setObjectName(QStringLiteral("clearAnnotationsAction"));
    clearAction_->setEnabled(!session_->annotations().isEmpty());
    connect(clearAction_, &QAction::triggered, this, &EditorWindow::clearAnnotations);
    (void)toolbar_->addAction(clearAction_);

    restoreAction_ = new QAction(
        glyphIcon(Glyph::Restore, iconColor),
        tr("恢复原图"),
        this);
    restoreAction_->setObjectName(QStringLiteral("restoreOriginalAction"));
    restoreAction_->setEnabled(
        session_->hasSourceImage() && session_->cropRect() != session_->sourceImage().rect());
    connect(restoreAction_, &QAction::triggered, this, &EditorWindow::restoreOriginal);
    (void)toolbar_->addAction(restoreAction_);

    toolbar_->addSeparator();
    auto* copyAction = new QAction(
        glyphIcon(Glyph::Copy, iconColor),
        tr("复制"),
        this);
    copyAction->setObjectName(QStringLiteral("copySnapshotAction"));
    copyAction->setShortcut(QKeySequence::Copy);
    connect(copyAction, &QAction::triggered, this, &EditorWindow::copyFromAction);
    (void)toolbar_->addAction(copyAction);

    auto* saveAction = new QAction(
        glyphIcon(Glyph::Save, iconColor),
        tr("保存"),
        this);
    saveAction->setObjectName(QStringLiteral("saveSnapshotAction"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &EditorWindow::chooseSaveLocation);
    (void)toolbar_->addAction(saveAction);

    auto* pinAction = new QAction(
        glyphIcon(Glyph::Pin, iconColor),
        tr("贴图"),
        this);
    pinAction->setObjectName(QStringLiteral("pinSnapshotAction"));
    connect(pinAction, &QAction::triggered, this, &EditorWindow::pinFromAction);
    (void)toolbar_->addAction(pinAction);

    toolbar_->addSeparator();
    auto* askAction = new QAction(
        glyphIcon(Glyph::Ask, iconColor),
        tr("提问"),
        this);
    askAction->setObjectName(QStringLiteral("askSnapshotAction"));
    askAction->setToolTip(tr("打开问题输入；在回答卡中按 Ctrl+Enter 发送"));
    connect(askAction, &QAction::triggered, this, &EditorWindow::askRequested);
    (void)toolbar_->addAction(askAction);

    auto* closeAction = new QAction(
        glyphIcon(Glyph::Close, iconColor),
        tr("关闭"),
        this);
    closeAction->setObjectName(QStringLiteral("closeEditorAction"));
    closeAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(closeAction, &QAction::triggered, this, &EditorWindow::close);
    (void)toolbar_->addAction(closeAction);
    toolbar_->adjustSize();
    setMinimumWidth(toolbar_->sizeHint().width() + 16);
}

void EditorWindow::setCanvasTool(snapask::ui::canvas::CanvasTool tool)
{
    canvas_->setTool(tool);
}

void EditorWindow::applyLineWidth(qreal width)
{
    if (width <= 0.0) {
        return;
    }
    snapask::AnnotationStyle style = canvas_->currentStyle();
    if (qFuzzyCompare(style.strokeWidth, width)) {
        return;
    }
    style.strokeWidth = width;
    canvas_->setCurrentStyle(style);
    QSettings().setValue(QString::fromLatin1(lineWidthSettingsKey), width);
}

void EditorWindow::applyTextFont(const QFont& font)
{
    const QString family = font.family().trimmed();
    if (family.isEmpty()) {
        return;
    }

    snapask::AnnotationStyle style = canvas_->currentStyle();
    if (style.font.family() == family) {
        return;
    }
    style.font.setFamily(family);
    canvas_->setCurrentStyle(style);
    QSettings().setValue(QString::fromLatin1(fontSettingsKey), style.font);
}

void EditorWindow::updateColorActionIcon()
{
    if (colorAction_ == nullptr || canvas_ == nullptr) {
        return;
    }
    QPixmap swatch(18, 18);
    swatch.fill(canvas_->currentStyle().strokeColor);
    colorAction_->setIcon(QIcon(swatch));
}

void EditorWindow::showOperationError(const QString& title, const QString& error)
{
    QMessageBox::warning(
        this,
        title,
        error.isEmpty() ? tr("操作未完成，当前截图和标注已保留。") : error);
}

void EditorWindow::setStatusText(const QString& text)
{
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(text);
    }
    if (toolbar_ != nullptr) {
        toolbar_->setAccessibleDescription(text);
    }
}

void EditorWindow::applyCaptureDesktopGeometry()
{
    if (!captureDesktopRectPx_.isValid() || internalWinId() == 0) {
        return;
    }

    const qreal ratio = std::max<qreal>(1.0, devicePixelRatioF());
    const int minimumWidthPx = toolbar_ != nullptr
        ? qRound(toolbar_->sizeHint().width() * ratio) : 480;
    const QRect desktop(
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
    QSize requested(
        std::max(captureDesktopRectPx_.width(), minimumWidthPx),
        captureDesktopRectPx_.height());
    QPoint position = captureDesktopRectPx_.topLeft();
    if (desktop.isValid()) {
        requested.setWidth(std::min(requested.width(), desktop.width()));
        requested.setHeight(std::min(requested.height(), desktop.height()));
        position.setX(std::clamp(
            position.x(), desktop.left(), desktop.right() - requested.width() + 1));
        position.setY(std::clamp(
            position.y(), desktop.top(), desktop.bottom() - requested.height() + 1));
    }
    SetWindowPos(
        reinterpret_cast<HWND>(winId()),
        HWND_TOPMOST,
        position.x(),
        position.y(),
        requested.width(),
        requested.height(),
        SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void EditorWindow::updateToolbarGeometry()
{
    if (toolbar_ == nullptr || canvas_ == nullptr) {
        return;
    }
    const QSize toolbarSize = toolbar_->sizeHint();
    const int x = std::max(8, (canvas_->width() - toolbarSize.width()) / 2);
    const int y = std::max(8, canvas_->height() - toolbarSize.height() - 14);
    toolbar_->setGeometry(QRect(QPoint(x, y), toolbarSize));
    toolbar_->raise();
    scheduleToolbarBackdropUpdate();
}

void EditorWindow::scheduleToolbarBackdropUpdate()
{
    if (toolbarBackdropUpdatePending_) {
        return;
    }
    toolbarBackdropUpdatePending_ = true;
    QTimer::singleShot(0, this, [this] {
        toolbarBackdropUpdatePending_ = false;
        updateToolbarBackdrop();
    });
}

void EditorWindow::updateToolbarBackdrop()
{
    if (toolbar_ == nullptr || canvas_ == nullptr) {
        return;
    }
    const QRect sourceRect = toolbar_->geometry().intersected(canvas_->rect());
    if (sourceRect.isEmpty()) {
        toolbar_->clearBackdropImage();
        return;
    }
    const qreal ratio = std::max<qreal>(1.0, canvas_->devicePixelRatioF());
    const QSize physicalSize(
        std::max(1, qRound(sourceRect.width() * ratio)),
        std::max(1, qRound(sourceRect.height() * ratio)));
    QImage localBackdrop(
        physicalSize,
        QImage::Format_ARGB32_Premultiplied);
    localBackdrop.setDevicePixelRatio(ratio);
    localBackdrop.fill(Qt::transparent);
    QPainter painter(&localBackdrop);
    canvas_->render(
        &painter,
        -sourceRect.topLeft(),
        QRegion(sourceRect),
        QWidget::DrawWindowBackground);
    painter.end();
    ++toolbarBackdropRevision_;
    toolbar_->setBackdropImage(
        localBackdrop,
        localBackdrop.rect(),
        toolbarBackdropRevision_);
}

}  // namespace snapask::ui::editor
