#include "ui/common/GlyphIcon.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

#include <array>

namespace snapask::ui {
namespace {

void drawGlyph(QPainter& painter, const Glyph glyph)
{
    const QPen pen = painter.pen();
    switch (glyph) {
    case Glyph::Select: {
        QPainterPath path;
        path.moveTo(5.0, 3.5);
        path.lineTo(5.0, 15.0);
        path.lineTo(8.5, 12.0);
        path.lineTo(11.2, 17.0);
        path.lineTo(13.5, 15.8);
        path.lineTo(10.8, 11.0);
        path.lineTo(15.0, 10.5);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case Glyph::Rectangle:
        painter.drawRoundedRect(QRectF(3.5, 4.5, 13.0, 11.0), 1.5, 1.5);
        break;
    case Glyph::Arrow:
        painter.drawLine(QPointF(3.5, 15.5), QPointF(15.5, 4.0));
        painter.drawLine(QPointF(10.5, 4.0), QPointF(15.5, 4.0));
        painter.drawLine(QPointF(15.5, 4.0), QPointF(15.5, 9.0));
        break;
    case Glyph::Text:
        painter.drawLine(QPointF(4.0, 4.5), QPointF(16.0, 4.5));
        painter.drawLine(QPointF(10.0, 4.5), QPointF(10.0, 16.0));
        painter.drawLine(QPointF(7.0, 16.0), QPointF(13.0, 16.0));
        break;
    case Glyph::Mosaic:
        for (const QRectF& square : std::array{
                 QRectF(3.5, 3.5, 5.5, 5.5), QRectF(11.0, 3.5, 5.5, 5.5),
                 QRectF(3.5, 11.0, 5.5, 5.5), QRectF(11.0, 11.0, 5.5, 5.5)}) {
            painter.drawRect(square);
        }
        break;
    case Glyph::Color:
        painter.drawEllipse(QRectF(3.0, 3.0, 14.0, 14.0));
        painter.drawLine(QPointF(5.5, 14.5), QPointF(14.5, 5.5));
        break;
    case Glyph::Undo:
    case Glyph::Redo: {
        const bool redo = glyph == Glyph::Redo;
        QPainterPath path;
        path.moveTo(redo ? 4.0 : 16.0, 14.8);
        path.cubicTo(
            redo ? 7.0 : 13.0, 7.0,
            redo ? 12.0 : 8.0, 7.0,
            redo ? 15.5 : 4.5, 10.0);
        painter.drawPath(path);
        const qreal headX = redo ? 15.3 : 4.7;
        painter.drawLine(QPointF(headX, 10.0), QPointF(headX, 5.8));
        painter.drawLine(
            QPointF(headX, 10.0), QPointF(redo ? 11.2 : 8.8, 10.0));
        break;
    }
    case Glyph::Clear:
        painter.drawLine(QPointF(5.0, 6.0), QPointF(15.0, 16.0));
        painter.drawLine(QPointF(15.0, 6.0), QPointF(5.0, 16.0));
        break;
    case Glyph::Restore:
        painter.drawArc(QRectF(3.5, 3.5, 13.0, 13.0), 30 * 16, 285 * 16);
        painter.drawLine(QPointF(4.2, 3.8), QPointF(4.0, 8.0));
        painter.drawLine(QPointF(4.2, 3.8), QPointF(8.2, 4.2));
        break;
    case Glyph::Copy:
        painter.drawRoundedRect(QRectF(6.5, 3.5, 10.0, 11.0), 1.5, 1.5);
        painter.drawRoundedRect(QRectF(3.5, 6.5, 10.0, 10.0), 1.5, 1.5);
        break;
    case Glyph::Save:
        painter.drawRoundedRect(QRectF(3.5, 3.5, 13.0, 13.0), 1.5, 1.5);
        painter.drawRect(QRectF(6.0, 3.5, 7.0, 4.5));
        painter.drawRoundedRect(QRectF(6.0, 11.0, 8.0, 5.5), 1.0, 1.0);
        break;
    case Glyph::Pin:
        painter.drawLine(QPointF(10.0, 11.0), QPointF(10.0, 17.0));
        painter.drawLine(QPointF(5.0, 10.5), QPointF(15.0, 10.5));
        painter.drawLine(QPointF(7.0, 4.0), QPointF(7.0, 7.0));
        painter.drawLine(QPointF(13.0, 4.0), QPointF(13.0, 7.0));
        painter.drawLine(QPointF(7.0, 7.0), QPointF(5.0, 10.5));
        painter.drawLine(QPointF(13.0, 7.0), QPointF(15.0, 10.5));
        break;
    case Glyph::Ask:
        painter.drawRoundedRect(QRectF(3.0, 4.0, 14.0, 10.5), 3.0, 3.0);
        painter.drawLine(QPointF(7.0, 14.5), QPointF(5.5, 17.0));
        painter.drawLine(QPointF(7.0, 14.5), QPointF(10.0, 14.5));
        painter.drawEllipse(QRectF(7.0, 8.0, 1.0, 1.0));
        painter.drawEllipse(QRectF(9.5, 8.0, 1.0, 1.0));
        painter.drawEllipse(QRectF(12.0, 8.0, 1.0, 1.0));
        break;
    case Glyph::Send:
        painter.drawPolygon(QPolygonF{
            QPointF(3.2, 4.0), QPointF(17.0, 10.0),
            QPointF(3.2, 16.0), QPointF(6.5, 10.0)});
        painter.drawLine(QPointF(6.5, 10.0), QPointF(17.0, 10.0));
        break;
    case Glyph::Stop:
        painter.drawRoundedRect(QRectF(5.0, 5.0, 10.0, 10.0), 1.5, 1.5);
        break;
    case Glyph::Close:
        painter.drawLine(QPointF(5.0, 5.0), QPointF(15.0, 15.0));
        painter.drawLine(QPointF(15.0, 5.0), QPointF(5.0, 15.0));
        break;
    case Glyph::Capture:
        painter.drawRoundedRect(QRectF(3.0, 5.0, 14.0, 11.0), 2.0, 2.0);
        painter.drawEllipse(QRectF(7.0, 8.0, 6.0, 6.0));
        painter.drawLine(QPointF(6.0, 5.0), QPointF(7.5, 3.5));
        painter.drawLine(QPointF(7.5, 3.5), QPointF(12.5, 3.5));
        painter.drawLine(QPointF(12.5, 3.5), QPointF(14.0, 5.0));
        break;
    case Glyph::General:
        painter.drawEllipse(QRectF(4.5, 4.5, 11.0, 11.0));
        painter.drawEllipse(QRectF(8.0, 8.0, 4.0, 4.0));
        painter.drawLine(QPointF(10.0, 1.8), QPointF(10.0, 4.5));
        painter.drawLine(QPointF(10.0, 15.5), QPointF(10.0, 18.2));
        painter.drawLine(QPointF(1.8, 10.0), QPointF(4.5, 10.0));
        painter.drawLine(QPointF(15.5, 10.0), QPointF(18.2, 10.0));
        break;
    case Glyph::Service:
        painter.drawEllipse(QRectF(3.5, 3.5, 5.5, 5.5));
        painter.drawEllipse(QRectF(11.0, 3.5, 5.5, 5.5));
        painter.drawEllipse(QRectF(7.3, 11.0, 5.5, 5.5));
        painter.drawLine(QPointF(8.0, 8.0), QPointF(9.0, 11.0));
        painter.drawLine(QPointF(12.0, 8.0), QPointF(11.0, 11.0));
        break;
    case Glyph::Privacy:
        painter.drawRoundedRect(QRectF(4.0, 8.0, 12.0, 9.0), 2.0, 2.0);
        painter.drawArc(QRectF(6.5, 3.0, 7.0, 10.0), 0, 180 * 16);
        painter.drawEllipse(QRectF(9.0, 11.0, 2.0, 2.0));
        break;
    case Glyph::Sun:
        painter.drawEllipse(QRectF(7.0, 7.0, 6.0, 6.0));
        painter.drawLine(QPointF(10.0, 2.0), QPointF(10.0, 4.5));
        painter.drawLine(QPointF(10.0, 15.5), QPointF(10.0, 18.0));
        painter.drawLine(QPointF(2.0, 10.0), QPointF(4.5, 10.0));
        painter.drawLine(QPointF(15.5, 10.0), QPointF(18.0, 10.0));
        break;
    case Glyph::Moon:
        painter.drawArc(QRectF(4.0, 3.0, 12.0, 14.0), 70 * 16, 230 * 16);
        painter.drawArc(QRectF(8.0, 2.0, 9.0, 13.0), 105 * 16, 180 * 16);
        break;
    case Glyph::System:
        painter.drawRoundedRect(QRectF(3.0, 4.0, 14.0, 10.0), 1.5, 1.5);
        painter.drawLine(QPointF(7.0, 17.0), QPointF(13.0, 17.0));
        painter.drawLine(QPointF(10.0, 14.0), QPointF(10.0, 17.0));
        break;
    }
    painter.setPen(pen);
}

}  // namespace

QIcon glyphIcon(const Glyph glyph, QColor foreground)
{
    if (!foreground.isValid()) {
        foreground = QColor(38, 38, 42);
    }

    QIcon icon;
    for (const qreal ratio : std::array{1.0, 2.0}) {
        QPixmap pixmap(QSize(qRound(20.0 * ratio), qRound(20.0 * ratio)));
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(foreground, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        drawGlyph(painter, glyph);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

}  // namespace snapask::ui
