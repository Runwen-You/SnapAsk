#include "domain/annotation/commands/AnnotationCommands.h"
#include "domain/capture/ScreenshotSession.h"
#include "services/ClipboardService.h"
#include "services/SaveService.h"
#include "services/SnapshotRenderer.h"
#include "ui/editor/EditorWindow.h"
#include "ui/pin/PinWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVector>
#include <QtTest>

#include <memory>
#include <utility>

namespace snapask {
namespace {

struct MimeEntry {
    QString format;
    QByteArray data;
};

// ClipboardService intentionally exercises the real Windows clipboard. Preserve
// every exposed MIME flavor plus Qt's typed representations so a passing or
// failing test does not consume the user's clipboard contents.
class ClipboardBackup final {
public:
    explicit ClipboardBackup(QClipboard* clipboard) : clipboard_(clipboard) {
        if (clipboard_ == nullptr) {
            return;
        }
        const QMimeData* mime = clipboard_->mimeData(QClipboard::Clipboard);
        if (mime == nullptr) {
            return;
        }
        const QStringList formats = mime->formats();
        entries_.reserve(formats.size());
        for (const QString& format : formats) {
            entries_.push_back(MimeEntry{format, mime->data(format)});
        }

        hasText_ = mime->hasText();
        hasHtml_ = mime->hasHtml();
        hasUrls_ = mime->hasUrls();
        hasImage_ = mime->hasImage();
        hasColor_ = mime->hasColor();
        if (hasText_) {
            text_ = mime->text();
        }
        if (hasHtml_) {
            html_ = mime->html();
        }
        if (hasUrls_) {
            urls_ = mime->urls();
        }
        if (hasImage_) {
            image_ = mime->imageData();
        }
        if (hasColor_) {
            color_ = mime->colorData();
        }
    }

    ~ClipboardBackup() {
        if (clipboard_ == nullptr) {
            return;
        }
        auto* restored = new QMimeData;
        for (const MimeEntry& entry : std::as_const(entries_)) {
            restored->setData(entry.format, entry.data);
        }
        if (hasText_) {
            restored->setText(text_);
        }
        if (hasHtml_) {
            restored->setHtml(html_);
        }
        if (hasUrls_) {
            restored->setUrls(urls_);
        }
        if (hasImage_) {
            restored->setImageData(image_);
        }
        if (hasColor_) {
            restored->setColorData(color_);
        }
        clipboard_->setMimeData(restored, QClipboard::Clipboard);
        QCoreApplication::processEvents();
    }

    ClipboardBackup(const ClipboardBackup&) = delete;
    ClipboardBackup& operator=(const ClipboardBackup&) = delete;

private:
    QClipboard* clipboard_{nullptr};
    QVector<MimeEntry> entries_;
    QString text_;
    QString html_;
    QList<QUrl> urls_;
    QVariant image_;
    QVariant color_;
    bool hasText_{false};
    bool hasHtml_{false};
    bool hasUrls_{false};
    bool hasImage_{false};
    bool hasColor_{false};
};

class RecordingAiSink final {
public:
    void submit(const RenderedSnapshot& snapshot) {
        receivedAddress_ = &snapshot;
        pngBytes_ = snapshot.pngBytes();
        sha256_ = snapshot.sha256();
        revision_ = snapshot.revision();
    }

    [[nodiscard]] const RenderedSnapshot* receivedAddress() const noexcept {
        return receivedAddress_;
    }
    [[nodiscard]] const QByteArray& pngBytes() const noexcept {
        return pngBytes_;
    }
    [[nodiscard]] const QByteArray& sha256() const noexcept {
        return sha256_;
    }
    [[nodiscard]] quint64 revision() const noexcept {
        return revision_;
    }

private:
    const RenderedSnapshot* receivedAddress_{nullptr};
    QByteArray pngBytes_;
    QByteArray sha256_;
    quint64 revision_{0};
};

struct PipelineFixture {
    std::unique_ptr<ScreenshotSession> session;
    QUuid rectangleId;
    QRect mosaicGoldenBlock;
};

QImage makeSourceImage() {
    QImage source(96, 72, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(
                x, y,
                QColor((x * 3 + y) % 256, (x + y * 5) % 256,
                       (x * 2 + y * 3) % 256));
        }
    }
    return source;
}

PipelineFixture makePipelineFixture() {
    PipelineFixture fixture;
    fixture.session = std::make_unique<ScreenshotSession>(makeSourceImage());
    AnnotationDocument& document = fixture.session->annotations();

    const Annotation rectangle =
        Annotation::makeRectangle(QRectF(4, 4, 22, 16), {}, 0);
    fixture.rectangleId = rectangle.id;
    if (!document.addAnnotation(rectangle)) {
        return {};
    }

    if (!document.addAnnotation(Annotation::makeArrow(
            QPointF(32, 8), QPointF(54, 22), {}, 1))) {
        return {};
    }

    AnnotationStyle textStyle;
    textStyle.strokeColor = QColor(20, 20, 20);
    if (!document.addAnnotation(Annotation::makeText(
            QRectF(4, 34, 42, 28), QStringLiteral("SnapAsk\n中文"), textStyle,
            2))) {
        return {};
    }

    AnnotationStyle mosaicStyle;
    mosaicStyle.mosaicBlockSize = 4;
    if (!document.addAnnotation(Annotation::makeMosaic(
            {QPointF(60, 48), QPointF(90, 48)}, 16.0, mosaicStyle, 3))) {
        return {};
    }

    // This complete 4x4 block is well inside the round mosaic stroke.
    fixture.mosaicGoldenBlock = QRect(68, 48, 4, 4);
    return fixture;
}

class M2PipelineTests final : public QObject {
    Q_OBJECT

private slots:
    void oneSnapshotFeedsEveryConsumer();
    void editorConsumersReuseRevisionBoundCanonicalSnapshot();
    void laterEditingCannotMutateFrozenSnapshot();
    void mosaicIsFlattenedIntoCanonicalPng();
};

void M2PipelineTests::oneSnapshotFeedsEveryConsumer() {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    PipelineFixture fixture = makePipelineFixture();
    QVERIFY(fixture.session != nullptr);
    QCOMPARE(fixture.session->annotations().size(), 4);

    // This is the only render in the test. Every downstream consumer receives
    // this exact immutable value instead of redrawing the live session.
    const RenderedSnapshot snapshot =
        SnapshotRenderer::renderCurrent(*fixture.session);
    QVERIFY(snapshot.isValid());

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("snapshot.png"));
    QString error;
    QVERIFY2(SaveService::savePng(snapshot, outputPath, &error),
             qPrintable(error));
    QFile savedFile(outputPath);
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    const QByteArray savedBytes = savedFile.readAll();
    QCOMPARE(savedBytes, snapshot.pngBytes());
    QCOMPARE(QCryptographicHash::hash(savedBytes, QCryptographicHash::Sha256),
             snapshot.sha256());

    QClipboard* clipboard = QApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    ClipboardBackup restoreClipboard(clipboard);
    QVERIFY2(ClipboardService::copy(snapshot, &error), qPrintable(error));
    QCoreApplication::processEvents();
    const QMimeData* clipboardMime =
        clipboard->mimeData(QClipboard::Clipboard);
    QVERIFY(clipboardMime != nullptr);
    QVERIFY(clipboardMime->hasFormat(QStringLiteral("image/png")));
    const QByteArray clipboardPng =
        clipboardMime->data(QStringLiteral("image/png"));
    QCOMPARE(clipboardPng, snapshot.pngBytes());
    QCOMPARE(
        QCryptographicHash::hash(clipboardPng, QCryptographicHash::Sha256),
        snapshot.sha256());
    QVERIFY(clipboardMime->hasImage());
    QCOMPARE(qvariant_cast<QImage>(clipboardMime->imageData()), snapshot.image());

    snapask::ui::pin::PinWindow pinWindow(snapshot);
    QCOMPARE(pinWindow.snapshot().sha256(), snapshot.sha256());
    QCOMPARE(pinWindow.snapshot().pngBytes(), snapshot.pngBytes());
    QCOMPARE(pinWindow.snapshot().revision(), snapshot.revision());
    QCOMPARE(pinWindow.snapshot().image(), snapshot.image());

    RecordingAiSink aiSink;
    aiSink.submit(snapshot);
    QCOMPARE(aiSink.receivedAddress(), &snapshot);
    QCOMPARE(aiSink.pngBytes(), snapshot.pngBytes());
    QCOMPARE(aiSink.sha256(), snapshot.sha256());
    QCOMPARE(aiSink.revision(), snapshot.revision());
}

void M2PipelineTests::editorConsumersReuseRevisionBoundCanonicalSnapshot()
{
    snapask::ui::editor::EditorWindow editor(makeSourceImage());
    editor.setAttribute(Qt::WA_DeleteOnClose, false);
    editor.session().undoStack().push(new AddAnnotationCommand(
        &editor.session().annotations(),
        Annotation::makeRectangle(QRectF(8, 7, 31, 19))));

    const RenderedSnapshot& canonical = editor.currentRenderedSnapshot();
    QVERIFY(canonical.isValid());
    const RenderedSnapshot* const canonicalAddress = &canonical;
    const QByteArray canonicalBytes = canonical.pngBytes();
    const QByteArray canonicalHash = canonical.sha256();
    const quint64 canonicalRevision = canonical.revision();

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString outputPath =
        temporaryDirectory.filePath(QStringLiteral("editor-canonical.png"));
    QString error;
    QVERIFY2(editor.saveCurrentSnapshot(outputPath, &error), qPrintable(error));
    QCOMPARE(&editor.currentRenderedSnapshot(), canonicalAddress);
    QFile savedFile(outputPath);
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    QCOMPARE(savedFile.readAll(), canonicalBytes);

    QClipboard* clipboard = QApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    ClipboardBackup restoreClipboard(clipboard);
    QVERIFY2(editor.copyCurrentSnapshot(&error), qPrintable(error));
    QCOMPARE(&editor.currentRenderedSnapshot(), canonicalAddress);
    QCOMPARE(
        clipboard->mimeData(QClipboard::Clipboard)
            ->data(QStringLiteral("image/png")),
        canonicalBytes);

    std::unique_ptr<snapask::ui::pin::PinWindow> pin(
        editor.pinCurrentSnapshot(&error));
    QVERIFY2(pin != nullptr, qPrintable(error));
    QCOMPARE(&editor.currentRenderedSnapshot(), canonicalAddress);
    QCOMPARE(pin->snapshot().pngBytes(), canonicalBytes);
    QCOMPARE(pin->snapshot().sha256(), canonicalHash);

    editor.session().undoStack().push(new AddAnnotationCommand(
        &editor.session().annotations(),
        Annotation::makeArrow(QPointF(5, 50), QPointF(70, 22))));
    const RenderedSnapshot& edited = editor.currentRenderedSnapshot();
    QVERIFY(edited.isValid());
    QVERIFY(edited.revision() > canonicalRevision);
    QVERIFY(edited.sha256() != canonicalHash);
    const RenderedSnapshot* const editedAddress = &edited;
    QCOMPARE(&editor.currentRenderedSnapshot(), editedAddress);
}

void M2PipelineTests::laterEditingCannotMutateFrozenSnapshot() {
    PipelineFixture fixture = makePipelineFixture();
    QVERIFY(fixture.session != nullptr);
    const RenderedSnapshot frozen =
        SnapshotRenderer::renderCurrent(*fixture.session);
    QVERIFY(frozen.isValid());
    const QByteArray frozenBytes = frozen.pngBytes();
    const QByteArray frozenHash = frozen.sha256();
    const QImage frozenImage = frozen.image();
    const quint64 frozenRevision = frozen.revision();

    const Annotation* rectangle =
        fixture.session->annotations().annotation(fixture.rectangleId);
    QVERIFY(rectangle != nullptr);
    const AnnotationGeometry moved =
        translatedGeometry(rectangle->geometry, QPointF(24, 10));
    fixture.session->undoStack().push(new TransformAnnotationCommand(
        &fixture.session->annotations(), fixture.rectangleId, moved));

    QCOMPARE(frozen.pngBytes(), frozenBytes);
    QCOMPARE(frozen.sha256(), frozenHash);
    QCOMPARE(frozen.image(), frozenImage);
    QCOMPARE(frozen.revision(), frozenRevision);

    const RenderedSnapshot edited =
        SnapshotRenderer::renderCurrent(*fixture.session);
    QVERIFY(edited.isValid());
    QVERIFY(edited.revision() > frozen.revision());
    QVERIFY(edited.sha256() != frozen.sha256());
    QCOMPARE(QCryptographicHash::hash(frozen.pngBytes(),
                                      QCryptographicHash::Sha256),
             frozen.sha256());
}

void M2PipelineTests::mosaicIsFlattenedIntoCanonicalPng() {
    PipelineFixture fixture = makePipelineFixture();
    QVERIFY(fixture.session != nullptr);
    const QImage untouchedSource = fixture.session->sourceImage();
    const RenderedSnapshot snapshot =
        SnapshotRenderer::renderCurrent(*fixture.session);
    QVERIFY(snapshot.isValid());

    QImage decodedPng;
    QVERIFY(decodedPng.loadFromData(snapshot.pngBytes(), "PNG"));
    decodedPng =
        decodedPng.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QCOMPARE(decodedPng, snapshot.image());
    QCOMPARE(fixture.session->sourceImage(), untouchedSource);

    const QRect block = fixture.mosaicGoldenBlock;
    const QColor flattened = snapshot.image().pixelColor(block.topLeft());
    for (int y = block.top(); y <= block.bottom(); ++y) {
        for (int x = block.left(); x <= block.right(); ++x) {
            QCOMPARE(snapshot.image().pixelColor(x, y), flattened);
            QCOMPARE(decodedPng.pixelColor(x, y), flattened);
        }
    }
    QVERIFY(untouchedSource.pixelColor(block.topLeft()) != flattened);
    QVERIFY(untouchedSource.pixelColor(block.topLeft()) !=
            untouchedSource.pixelColor(block.bottomRight()));
}

}  // namespace
}  // namespace snapask

QTEST_MAIN(snapask::M2PipelineTests)

#include "m2_pipeline_tests.moc"
