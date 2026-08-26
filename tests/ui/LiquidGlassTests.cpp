#include "ui/common/ThemeTokens.h"
#include "ui/glass/GlassBackdropCache.h"
#include "ui/glass/GlassMaterial.h"
#include "ui/glass/GlassPainter.h"
#include "ui/glass/GlassSurface.h"
#include "ui/glass/GlassToolButton.h"
#include "ui/glass/GlassToolbar.h"

#include <QAction>
#include <QApplication>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QSet>
#include <QTest>

#include <array>
#include <cmath>

using snapask::ui::ThemeMode;
using snapask::ui::ThemeTokens;
using snapask::ui::glass::GlassBackdropMode;
using snapask::ui::glass::GlassBackdropCache;
using snapask::ui::glass::GlassMaterialRole;
using snapask::ui::glass::GlassPainter;
using snapask::ui::glass::GlassSurface;
using snapask::ui::glass::GlassToolButton;
using snapask::ui::glass::GlassToolbar;
using snapask::ui::glass::materialFor;

class LiquidGlassTests final : public QObject {
    Q_OBJECT

private slots:
    void tokensProvideDistinctLightAndDarkMaterials();
    void painterProducesLayeredOutputAtCommonScaleFactors();
    void imageBackdropCachesLocalProcessing();
    void surfaceProvidesOpaqueAccessibleFallback();
    void toolbarPreservesActionStatesAndAnimatedFeedback();
};

void LiquidGlassTests::tokensProvideDistinctLightAndDarkMaterials()
{
    const auto light = ThemeTokens::resolve(ThemeMode::Light);
    const auto dark = ThemeTokens::resolve(ThemeMode::Dark);

    QVERIFY(light.glassTint.isValid());
    QVERIFY(dark.glassTint.isValid());
    QVERIFY(light.glassTint != dark.glassTint);
    QVERIFY(light.glassEdgeBright.alpha() > light.glassEdgeDim.alpha());
    QVERIFY(dark.glassEdgeBright.alpha() > 0);
    QVERIFY(light.glassRadius >= light.panelRadius);
    QVERIFY(light.glassCapsuleRadius > light.controlRadius);
    QVERIFY(light.animationFastMs < light.animationNormalMs);
    QVERIFY(light.animationNormalMs < light.animationSlowMs);
}

void LiquidGlassTests::painterProducesLayeredOutputAtCommonScaleFactors()
{
    constexpr QSize logicalSize(240, 86);
    const std::array<qreal, 4> scaleFactors{1.0, 1.25, 1.5, 2.0};
    for (const ThemeMode mode : {ThemeMode::Light, ThemeMode::Dark}) {
        const auto material = materialFor(
            ThemeTokens::resolve(mode),
            GlassMaterialRole::Elevated);
        for (const qreal scale : scaleFactors) {
            const QSize physicalSize(
                static_cast<int>(std::ceil(logicalSize.width() * scale)),
                static_cast<int>(std::ceil(logicalSize.height() * scale)));
            QImage image(
                physicalSize,
                QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.scale(scale, scale);
            GlassPainter::paintSurface(
                painter,
                QRectF(8.0, 7.0, 224.0, 68.0),
                material,
                0.72,
                0.25,
                QPointF(62.0, 22.0));
            painter.end();

            QSet<QRgb> visibleColors;
            int visiblePixels = 0;
            for (int y = 0; y < image.height(); y += 2) {
                for (int x = 0; x < image.width(); x += 2) {
                    const QRgb pixel = image.pixel(x, y);
                    if (qAlpha(pixel) > 0) {
                        ++visiblePixels;
                        visibleColors.insert(pixel);
                    }
                }
            }
            QVERIFY(visiblePixels > 300);
            QVERIFY(visibleColors.size() > 24);
        }
    }
}

void LiquidGlassTests::imageBackdropCachesLocalProcessing()
{
    QImage source(QSize(420, 240), QImage::Format_ARGB32_Premultiplied);
    QPainter sourcePainter(&source);
    QLinearGradient gradient(source.rect().topLeft(), source.rect().bottomRight());
    gradient.setColorAt(0.0, QColor(34, 98, 180));
    gradient.setColorAt(0.5, QColor(230, 180, 72));
    gradient.setColorAt(1.0, QColor(82, 42, 128));
    sourcePainter.fillRect(source.rect(), gradient);
    sourcePainter.end();

    GlassBackdropCache cache;
    const QRect sourceRect(80, 70, 240, 70);
    const QImage first = cache.imageFor(
        source,
        sourceRect,
        QSize(200, 50),
        1.5,
        false,
        12,
        4);
    QVERIFY(!first.isNull());
    QCOMPARE(first.size(), QSize(300, 75));
    QCOMPARE(cache.generationCount(), quint64{1});

    const QImage second = cache.imageFor(
        source,
        sourceRect,
        QSize(200, 50),
        1.5,
        false,
        12,
        4);
    QCOMPARE(second.cacheKey(), first.cacheKey());
    QCOMPARE(cache.generationCount(), quint64{1});

    (void)cache.imageFor(
        source,
        sourceRect.translated(2, 0),
        QSize(200, 50),
        1.5,
        false,
        12,
        4);
    QCOMPARE(cache.generationCount(), quint64{2});

    GlassSurface surface;
    surface.resize(220, 78);
    surface.setBackdropMode(GlassBackdropMode::Image);
    surface.setBackdropImage(source, sourceRect, 8);
    QImage rendered(surface.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    surface.render(&rendered);
    const quint64 processed = surface.backdropGenerationCount();
    QCOMPARE(processed, quint64{1});
    surface.setPointerPosition(QPointF(180.0, 18.0));
    surface.setHoverProgress(1.0);
    surface.render(&rendered);
    QCOMPARE(surface.backdropGenerationCount(), processed);

    for (int frame = 0; frame < 240; ++frame) {
        surface.setPointerPosition(QPointF(
            static_cast<qreal>(frame % surface.width()),
            static_cast<qreal>((frame * 3) % surface.height())));
        surface.setHoverProgress(static_cast<qreal>(frame % 10) / 9.0);
        surface.render(&rendered);
    }
    QCOMPARE(surface.backdropGenerationCount(), processed);
}

void LiquidGlassTests::surfaceProvidesOpaqueAccessibleFallback()
{
    GlassSurface surface;
    surface.resize(220, 90);
    surface.setBackdropMode(GlassBackdropMode::SolidFallback);

    QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    surface.render(&image);

    QCOMPARE(qAlpha(image.pixel(image.rect().center())), 255);
}

void LiquidGlassTests::toolbarPreservesActionStatesAndAnimatedFeedback()
{
    GlassToolbar toolbar;
    auto* normalAction = new QAction(QStringLiteral("Normal"), &toolbar);
    auto* checkedAction = new QAction(QStringLiteral("Checked"), &toolbar);
    checkedAction->setCheckable(true);
    checkedAction->setChecked(true);
    auto* disabledAction = new QAction(QStringLiteral("Disabled"), &toolbar);
    disabledAction->setEnabled(false);

    GlassToolButton* normalButton = toolbar.addAction(normalAction);
    GlassToolButton* checkedButton = toolbar.addAction(checkedAction);
    toolbar.addSeparator();
    GlassToolButton* disabledButton = toolbar.addAction(disabledAction);
    toolbar.resize(toolbar.sizeHint());
    toolbar.show();
    QCoreApplication::processEvents();

    QVERIFY(normalButton != nullptr);
    QVERIFY(checkedButton != nullptr);
    QVERIFY(disabledButton != nullptr);
    QCOMPARE(normalButton->defaultAction(), normalAction);
    QVERIFY(checkedButton->isCheckable());
    QVERIFY(checkedButton->isChecked());
    QVERIFY(!disabledButton->isEnabled());
    QCOMPARE(normalButton->accessibleName(), QStringLiteral("Normal"));

    QTest::mouseMove(normalButton, normalButton->rect().center());
    QTRY_VERIFY_WITH_TIMEOUT(normalButton->hoverProgress() > 0.25, 500);
    QTest::mousePress(normalButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(normalButton->pressProgress() > 0.25, 500);
    QTest::mouseRelease(normalButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(normalButton->pressProgress() < 0.25, 500);
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    LiquidGlassTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "LiquidGlassTests.moc"
