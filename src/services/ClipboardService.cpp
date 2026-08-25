#include "services/ClipboardService.h"

#include "services/SnapshotRenderer.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QThread>

namespace snapask {

bool ClipboardService::copy(const RenderedSnapshot& snapshot, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!snapshot.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("The rendered snapshot is invalid and cannot be copied.");
        }
        return false;
    }

    auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (application == nullptr || QThread::currentThread() != application->thread()) {
        if (error != nullptr) {
            *error = QStringLiteral("The clipboard must be updated on the GUI thread.");
        }
        return false;
    }

    QClipboard* clipboard = application->clipboard();
    if (clipboard == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("The Windows clipboard is currently unavailable.");
        }
        return false;
    }

    auto* mimeData = new QMimeData;
    mimeData->setImageData(snapshot.image());
    mimeData->setData(QStringLiteral("image/png"), snapshot.pngBytes());
    clipboard->setMimeData(mimeData, QClipboard::Clipboard);
    return true;
}

}  // namespace snapask
