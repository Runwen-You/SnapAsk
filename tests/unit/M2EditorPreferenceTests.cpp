#include "domain/annotation/Annotation.h"
#include "domain/capture/ScreenshotSession.h"
#include "ui/canvas/CanvasWidget.h"
#include "ui/editor/EditorWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFontComboBox>
#include <QInputDialog>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace snapask::ui::editor {
namespace {

QImage sourceImage() {
    QImage image(QSize(320, 200), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(245, 246, 248));
    return image;
}

class M2EditorPreferenceTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void selectedFontPersistsAndStylesSubsequentText();

private:
    QTemporaryDir settingsDirectory_;
};

void M2EditorPreferenceTests::initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("SnapAskM2Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("SnapAskM2EditorPreferenceTests"));
}

void M2EditorPreferenceTests::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
}

void M2EditorPreferenceTests::selectedFontPersistsAndStylesSubsequentText() {
    QString selectedFamily;
    int selectedPixelSize = -1;

    {
        EditorWindow editor(sourceImage());
        auto* fontCombo =
            editor.findChild<QFontComboBox*>(QStringLiteral("annotationFontCombo"));
        auto* canvas = editor.canvasWidget();
        QVERIFY(fontCombo != nullptr);
        QVERIFY(canvas != nullptr);
        QCOMPARE(fontCombo->accessibleName(), QStringLiteral("文字字体"));

        const int originalPixelSize = canvas->currentStyle().font.pixelSize();
        QFont selectedFont = canvas->currentStyle().font;
        selectedFamily = QStringLiteral("SnapAsk M2 Test Sans");
        selectedFont.setFamily(selectedFamily);
        QVERIFY(QMetaObject::invokeMethod(
            fontCombo,
            "currentFontChanged",
            Qt::DirectConnection,
            Q_ARG(QFont, selectedFont)));
        QTRY_COMPARE(canvas->currentStyle().font.family(), selectedFamily);
        QCOMPARE(canvas->currentStyle().font.pixelSize(), originalPixelSize);
        selectedPixelSize = canvas->currentStyle().font.pixelSize();

        QSettings settings;
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(settings.contains(QStringLiteral("annotation/lastFont")));
        const QFont persisted = settings.value(
            QStringLiteral("annotation/lastFont")).value<QFont>();
        QCOMPARE(persisted.family(), selectedFamily);
        QCOMPARE(persisted.pixelSize(), selectedPixelSize);

        editor.show();
        canvas->setTool(snapask::ui::canvas::CanvasTool::Text);
        QCoreApplication::processEvents();

        bool dialogAccepted = false;
        QTimer dialogTimer;
        dialogTimer.setInterval(1);
        connect(&dialogTimer, &QTimer::timeout, this, [&dialogAccepted, &dialogTimer] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QInputDialog*>(widget);
                if (dialog != nullptr && dialog->isVisible()) {
                    dialog->setTextValue(QStringLiteral("后续文字"));
                    dialogAccepted = true;
                    dialogTimer.stop();
                    dialog->accept();
                    return;
                }
            }
        });
        dialogTimer.start();

        const QPoint start =
            canvas->mapImageToWidget(QPointF(30.0, 35.0)).toPoint();
        const QPoint end =
            canvas->mapImageToWidget(QPointF(180.0, 95.0)).toPoint();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, end);
        QVERIFY(dialogAccepted);
        QCOMPARE(editor.session().annotations().size(), 1);
        const Annotation& text = editor.session().annotations().annotations().front();
        QCOMPARE(text.type, AnnotationType::Text);
        QCOMPARE(text.style.font.family(), selectedFamily);
        QCOMPARE(text.style.font.pixelSize(), selectedPixelSize);
    }

    // A fresh editor reads the exact annotation/lastFont value and uses it as
    // both the toolbar selection and the creation style.
    EditorWindow reopened(sourceImage());
    auto* reopenedCombo =
        reopened.findChild<QFontComboBox*>(QStringLiteral("annotationFontCombo"));
    QVERIFY(reopenedCombo != nullptr);
    QCOMPARE(reopened.canvasWidget()->currentStyle().font.family(), selectedFamily);
    QCOMPARE(reopened.canvasWidget()->currentStyle().font.pixelSize(),
             selectedPixelSize);
    QCOMPARE(reopenedCombo->accessibleName(), QStringLiteral("文字字体"));
}

}  // namespace
}  // namespace snapask::ui::editor

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    snapask::ui::editor::M2EditorPreferenceTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "M2EditorPreferenceTests.moc"
