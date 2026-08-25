#include "services/SaveService.h"

#include "services/SnapshotRenderer.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace snapask {

bool SaveService::savePng(
    const RenderedSnapshot& snapshot,
    const QString& filePath,
    QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!snapshot.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("The rendered snapshot is invalid and cannot be saved.");
        }
        return false;
    }
    if (filePath.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("No PNG destination was selected.");
        }
        return false;
    }

    const QFileInfo destination(filePath);
    if (!destination.dir().exists()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected destination folder does not exist.");
        }
        return false;
    }

    QSaveFile output(destination.absoluteFilePath());
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The PNG could not be opened for writing: %1")
                         .arg(output.errorString());
        }
        return false;
    }

    const QByteArray& bytes = snapshot.pngBytes();
    const qint64 written = output.write(bytes);
    if (written != bytes.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("The complete PNG could not be written: %1")
                         .arg(output.errorString());
        }
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("The PNG could not be committed atomically: %1")
                         .arg(output.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace snapask
