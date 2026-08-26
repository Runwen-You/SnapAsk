#include "ui/common/GlyphIcon.h"
#include "ui/common/ThemeTokens.h"
#include "ui/glass/GlassMaterial.h"
#include "ui/glass/GlassSurface.h"
#include "ui/glass/GlassToolbar.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QWidget* prototypeColumn(
    const snapask::ui::ThemeMode mode,
    const QString& title,
    QWidget* parent)
{
    const auto tokens = snapask::ui::ThemeTokens::resolve(mode);
    auto* column = new QWidget(parent);
    QPalette palette = column->palette();
    palette.setColor(QPalette::Window, tokens.window);
    palette.setColor(QPalette::WindowText, tokens.textPrimary);
    palette.setColor(QPalette::Text, tokens.textPrimary);
    column->setPalette(palette);
    column->setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    auto* heading = new QLabel(title, column);
    QFont headingFont = heading->font();
    headingFont.setPointSize(15);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto* surface = new snapask::ui::glass::GlassSurface(column);
    surface->setMinimumSize(350, 150);
    auto* surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(26, 24, 26, 24);
    auto* surfaceTitle = new QLabel(
        QObject::tr("GlassSurface · move the pointer to shift the highlight"),
        surface);
    surfaceTitle->setWordWrap(true);
    surfaceLayout->addWidget(surfaceTitle);
    surfaceLayout->addStretch(1);
    layout->addWidget(surface);

    auto* toolbar = new snapask::ui::glass::GlassToolbar(column);
    const QColor iconColor = tokens.textPrimary;
    auto* select = new QAction(
        snapask::ui::glyphIcon(snapask::ui::Glyph::Select, iconColor),
        QObject::tr("Normal"),
        toolbar);
    auto* rectangle = new QAction(
        snapask::ui::glyphIcon(snapask::ui::Glyph::Rectangle, iconColor),
        QObject::tr("Checked"),
        toolbar);
    rectangle->setCheckable(true);
    rectangle->setChecked(true);
    auto* arrow = new QAction(
        snapask::ui::glyphIcon(snapask::ui::Glyph::Arrow, iconColor),
        QObject::tr("Disabled"),
        toolbar);
    arrow->setEnabled(false);
    auto* close = new QAction(
        snapask::ui::glyphIcon(snapask::ui::Glyph::Close, iconColor),
        QObject::tr("Press me"),
        toolbar);
    (void)toolbar->addAction(select);
    (void)toolbar->addAction(rectangle);
    (void)toolbar->addAction(arrow);
    toolbar->addSeparator();
    (void)toolbar->addAction(close);
    layout->addWidget(toolbar, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    return column;
}

}  // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QWidget window;
    window.setWindowTitle(QStringLiteral("SnapAsk Liquid Glass Prototype"));
    window.resize(860, 430);
    auto* layout = new QHBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    layout->addWidget(prototypeColumn(
        snapask::ui::ThemeMode::Light,
        QStringLiteral("Light"),
        &window));
    layout->addWidget(prototypeColumn(
        snapask::ui::ThemeMode::Dark,
        QStringLiteral("Dark"),
        &window));
    window.show();

    const QStringList arguments = QCoreApplication::arguments();
    const qsizetype snapshotArgument = arguments.indexOf(QStringLiteral("--snapshot"));
    if (snapshotArgument >= 0 && snapshotArgument + 1 < arguments.size()) {
        const QString snapshotPath = arguments.at(snapshotArgument + 1);
        QTimer::singleShot(150, &window, [&application, &window, snapshotPath]() {
            application.exit(window.grab().save(snapshotPath) ? 0 : 2);
        });
    }
    return application.exec();
}
