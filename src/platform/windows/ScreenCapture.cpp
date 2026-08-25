#include "platform/windows/ScreenCapture.h"

#include "platform/windows/MonitorCoordinateMapper.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QDateTime>
#include <QImage>

#include <limits>
#include <optional>
#include <utility>

namespace snapask::platform::windows {

namespace {

[[nodiscard]] QString formatWindowsError(const QString& operation, DWORD errorCode)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    const QString description = length > 0 && message != nullptr
        ? QString::fromWCharArray(message, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Unknown Windows error");
    if (message != nullptr) {
        LocalFree(message);
    }
    return QStringLiteral("%1 failed (%2): %3").arg(operation).arg(errorCode).arg(description);
}

void setError(QString* target, const QString& message)
{
    if (target != nullptr) {
        *target = message;
    }
}

}  // namespace

std::optional<snapask::capture::CaptureFrame> GdiScreenCapture::captureVirtualDesktop(
    QString* error) const
{
    if (error != nullptr) {
        error->clear();
    }

    QString monitorError;
    snapask::capture::MonitorLayout monitorLayout = MonitorCoordinateMapper::query(&monitorError);
    if (!monitorLayout.isValid()) {
        setError(error, monitorError);
        return std::nullopt;
    }

    const QRect desktopRect = monitorLayout.virtualDesktopPx();
    if (desktopRect.width() <= 0 || desktopRect.height() <= 0
        || desktopRect.width() > (std::numeric_limits<int>::max() / 4)) {
        setError(error, QStringLiteral("The Windows virtual desktop dimensions are invalid."));
        return std::nullopt;
    }

    SetLastError(ERROR_SUCCESS);
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        setError(error, formatWindowsError(QStringLiteral("GetDC"), GetLastError()));
        return std::nullopt;
    }

    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        const DWORD code = GetLastError();
        ReleaseDC(nullptr, screenDc);
        setError(error, formatWindowsError(QStringLiteral("CreateCompatibleDC"), code));
        return std::nullopt;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = desktopRect.width();
    bitmapInfo.bmiHeader.biHeight = -desktopRect.height();  // top-down DIB
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixelBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &pixelBits,
        nullptr,
        0);
    if (bitmap == nullptr || pixelBits == nullptr) {
        const DWORD code = GetLastError();
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        setError(error, formatWindowsError(QStringLiteral("CreateDIBSection"), code));
        return std::nullopt;
    }

    const HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        const DWORD code = GetLastError();
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        setError(error, formatWindowsError(QStringLiteral("SelectObject"), code));
        return std::nullopt;
    }

    SetLastError(ERROR_SUCCESS);
    const BOOL copied = BitBlt(
        memoryDc,
        0,
        0,
        desktopRect.width(),
        desktopRect.height(),
        screenDc,
        desktopRect.x(),
        desktopRect.y(),
        SRCCOPY | CAPTUREBLT);
    const DWORD copyError = copied == FALSE ? GetLastError() : ERROR_SUCCESS;
    GdiFlush();

    QImage ownedImage;
    if (copied != FALSE) {
        // GDI's unused high byte is not guaranteed to contain opaque alpha. RGB32
        // intentionally ignores it; conversion then creates deterministic alpha=255.
        const QImage dibView(
            static_cast<const uchar*>(pixelBits),
            desktopRect.width(),
            desktopRect.height(),
            desktopRect.width() * 4,
            QImage::Format_RGB32);
        ownedImage = dibView.copy().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (copied == FALSE || ownedImage.isNull()) {
        setError(
            error,
            copied == FALSE
                ? formatWindowsError(QStringLiteral("BitBlt"), copyError)
                : QStringLiteral("The captured GDI bitmap could not be copied into a QImage."));
        return std::nullopt;
    }

    return snapask::capture::CaptureFrame(
        std::move(ownedImage),
        desktopRect,
        std::move(monitorLayout),
        QDateTime::currentDateTimeUtc());
}

}  // namespace snapask::platform::windows
