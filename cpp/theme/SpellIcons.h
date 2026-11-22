#pragma once

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace spell::icons {

namespace detail {

inline constexpr double kPi = 3.14159265358979323846;

inline qreal currentDevicePixelRatio() {
    if (auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        const qreal dpr = guiApp->devicePixelRatio();
        if (dpr >= 1.0 && dpr <= 4.0) {
            return dpr;
        }
    }
    return 1.0;
}

/// Draws a sleek monochrome witch hat silhouette normalized to a 24x24 viewBox.
inline void drawFallbackWitchHat(QPainter& p, const QColor& tint) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(tint);

    QPainterPath hat;
    // Wide curved brim
    hat.moveTo(2.0, 18.2);
    hat.cubicTo(5.5, 16.4, 18.5, 16.4, 22.0, 18.2);
    hat.cubicTo(22.4, 19.6, 17.5, 21.0, 12.0, 21.0);
    hat.cubicTo(6.5, 21.0, 1.6, 19.6, 2.0, 18.2);
    hat.closeSubpath();

    // Tall pointed cone with subtle magical bend at the tip
    QPainterPath crown;
    crown.moveTo(6.4, 17.3);
    crown.cubicTo(7.4, 12.8, 9.2, 7.6, 11.4, 3.6);
    crown.cubicTo(12.2, 2.2, 14.4, 2.1, 15.8, 3.8);
    crown.cubicTo(14.5, 4.1, 13.7, 5.2, 14.0, 6.7);
    crown.cubicTo(15.0, 10.6, 16.4, 14.0, 17.6, 17.3);
    crown.closeSubpath();

    hat = hat.united(crown);
    p.drawPath(hat);
    p.restore();
}

inline QImage loadCachedImage(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    QImage img(path);
    if (img.isNull()) {
        return {};
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

inline void paintVectorGlyph(QPainter& p,
                             const QString& rawName,
                             const QColor& color,
                             qreal strokeWidth = 1.65) {
    const QString name = rawName.trimmed().toLower();

    QPen stroke(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(stroke);
    p.setBrush(Qt::NoBrush);

    auto drawDot = [&](double cx, double cy, double r = 1.05) {
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(cx, cy), r, r);
        p.restore();
    };

    if (name == QLatin1String("gear") || name == QLatin1String("settings")) {
        QPainterPath gear;
        constexpr int teeth = 8;
        constexpr double rOuter = 9.3;
        constexpr double rInner = 6.6;
        constexpr double step = (2.0 * kPi) / teeth;
        constexpr double halfTooth = step * 0.21;
        constexpr double halfValley = step * 0.29;
        for (int i = 0; i < teeth; ++i) {
            const double base = i * step - kPi / 2.0;
            const double a0 = base - halfValley;
            const double a1 = base - halfTooth;
            const double a2 = base + halfTooth;
            const double a3 = base + halfValley;
            const QPointF p0(12.0 + rInner * std::cos(a0), 12.0 + rInner * std::sin(a0));
            const QPointF p1(12.0 + rOuter * std::cos(a1), 12.0 + rOuter * std::sin(a1));
            const QPointF p2(12.0 + rOuter * std::cos(a2), 12.0 + rOuter * std::sin(a2));
            const QPointF p3(12.0 + rInner * std::cos(a3), 12.0 + rInner * std::sin(a3));
            if (i == 0) {
                gear.moveTo(p0);
            } else {
                gear.lineTo(p0);
            }
            gear.lineTo(p1);
            gear.lineTo(p2);
            gear.lineTo(p3);
        }
        gear.closeSubpath();
        p.drawPath(gear);
        p.drawEllipse(QPointF(12.0, 12.0), 3.0, 3.0);
        return;
    }

    if (name == QLatin1String("plus")) {
        p.drawLine(QPointF(12.0, 5.0), QPointF(12.0, 19.0));
        p.drawLine(QPointF(5.0, 12.0), QPointF(19.0, 12.0));
        return;
    }

    if (name == QLatin1String("minus")) {
        p.drawLine(QPointF(5.0, 12.0), QPointF(19.0, 12.0));
        return;
    }

    if (name == QLatin1String("close") || name == QLatin1String("x")) {
        p.drawLine(QPointF(6.2, 6.2), QPointF(17.8, 17.8));
        p.drawLine(QPointF(17.8, 6.2), QPointF(6.2, 17.8));
        return;
    }

    if (name == QLatin1String("folder")) {
        QPainterPath path;
        path.moveTo(3.2, 7.2);
        path.quadTo(3.2, 5.5, 4.9, 5.5);
        path.lineTo(9.3, 5.5);
        path.lineTo(11.3, 8.2);
        path.lineTo(19.1, 8.2);
        path.quadTo(20.8, 8.2, 20.8, 9.9);
        path.lineTo(20.8, 17.3);
        path.quadTo(20.8, 19.0, 19.1, 19.0);
        path.lineTo(4.9, 19.0);
        path.quadTo(3.2, 19.0, 3.2, 17.3);
        path.closeSubpath();
        p.drawPath(path);
        return;
    }

    if (name == QLatin1String("folder-open")) {
        QPainterPath back;
        back.moveTo(3.2, 18.2);
        back.lineTo(3.2, 7.0);
        back.quadTo(3.2, 5.5, 4.7, 5.5);
        back.lineTo(9.0, 5.5);
        back.lineTo(11.0, 8.0);
        back.lineTo(18.0, 8.0);
        back.quadTo(19.5, 8.0, 19.5, 9.5);
        back.lineTo(19.5, 11.2);
        p.drawPath(back);

        QPainterPath flap;
        flap.moveTo(3.2, 18.2);
        flap.lineTo(5.4, 11.8);
        flap.quadTo(5.8, 11.0, 6.8, 11.0);
        flap.lineTo(20.4, 11.0);
        flap.quadTo(21.4, 11.0, 21.0, 12.1);
        flap.lineTo(19.2, 17.8);
        flap.quadTo(18.8, 18.8, 17.7, 18.8);
        flap.lineTo(4.4, 18.8);
        flap.quadTo(3.2, 18.8, 3.2, 18.2);
        p.drawPath(flap);
        return;
    }

    if (name == QLatin1String("file") || name == QLatin1String("doc")) {
        QPainterPath doc;
        doc.moveTo(5.6, 5.0);
        doc.quadTo(5.6, 3.4, 7.2, 3.4);
        doc.lineTo(13.8, 3.4);
        doc.lineTo(18.4, 8.0);
        doc.lineTo(18.4, 19.0);
        doc.quadTo(18.4, 20.6, 16.8, 20.6);
        doc.lineTo(7.2, 20.6);
        doc.quadTo(5.6, 20.6, 5.6, 19.0);
        doc.closeSubpath();
        p.drawPath(doc);

        QPainterPath fold;
        fold.moveTo(13.8, 3.4);
        fold.lineTo(13.8, 6.8);
        fold.quadTo(13.8, 8.0, 15.0, 8.0);
        fold.lineTo(18.4, 8.0);
        p.drawPath(fold);
        return;
    }

    if (name == QLatin1String("terminal")) {
        p.drawRoundedRect(QRectF(3.0, 4.5, 18.0, 15.0), 2.5, 2.5);
        QPainterPath prompt;
        prompt.moveTo(6.8, 9.4);
        prompt.lineTo(10.2, 12.2);
        prompt.lineTo(6.8, 15.0);
        p.drawPath(prompt);
        p.drawLine(QPointF(12.2, 15.0), QPointF(16.8, 15.0));
        return;
    }

    if (name == QLatin1String("play")) {
        QPainterPath tri;
        tri.moveTo(7.2, 5.2);
        tri.lineTo(18.8, 12.0);
        tri.lineTo(7.2, 18.8);
        tri.closeSubpath();
        p.setBrush(color);
        p.drawPath(tri);
        return;
    }

    if (name == QLatin1String("stop")) {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(6.2, 6.2, 11.6, 11.6), 2.0, 2.0);
        return;
    }

    if (name == QLatin1String("refresh")) {
        const QRectF arcRect(4.8, 4.8, 14.4, 14.4);
        p.drawArc(arcRect, 35 * 16, 295 * 16);
        QPainterPath head;
        head.moveTo(15.4, 4.4);
        head.lineTo(18.6, 7.4);
        head.lineTo(14.5, 8.2);
        p.drawPath(head);
        return;
    }

    if (name == QLatin1String("search")) {
        p.drawEllipse(QPointF(10.6, 10.6), 6.1, 6.1);
        p.drawLine(QPointF(15.2, 15.2), QPointF(20.0, 20.0));
        return;
    }

    if (name == QLatin1String("save")) {
        QPainterPath outer;
        outer.moveTo(4.2, 5.6);
        outer.quadTo(4.2, 4.2, 5.6, 4.2);
        outer.lineTo(16.2, 4.2);
        outer.lineTo(19.8, 7.8);
        outer.lineTo(19.8, 18.4);
        outer.quadTo(19.8, 19.8, 18.4, 19.8);
        outer.lineTo(5.6, 19.8);
        outer.quadTo(4.2, 19.8, 4.2, 18.4);
        outer.closeSubpath();
        p.drawPath(outer);
        p.drawRect(QRectF(8.0, 4.2, 6.4, 4.8));
        p.drawRect(QRectF(7.2, 13.4, 9.6, 6.4));
        return;
    }

    if (name == QLatin1String("copy")) {
        p.drawRoundedRect(QRectF(8.8, 8.8, 11.2, 11.2), 2.0, 2.0);
        QPainterPath back;
        back.moveTo(8.8, 15.2);
        back.lineTo(5.8, 15.2);
        back.quadTo(4.2, 15.2, 4.2, 13.6);
        back.lineTo(4.2, 5.8);
        back.quadTo(4.2, 4.2, 5.8, 4.2);
        back.lineTo(13.6, 4.2);
        back.quadTo(15.2, 4.2, 15.2, 5.8);
        back.lineTo(15.2, 8.8);
        p.drawPath(back);
        return;
    }

    if (name == QLatin1String("check")) {
        QPainterPath chk;
        chk.moveTo(4.8, 12.6);
        chk.lineTo(9.6, 17.4);
        chk.lineTo(19.4, 7.0);
        p.drawPath(chk);
        return;
    }

    if (name == QLatin1String("trash")) {
        p.drawLine(QPointF(4.4, 6.6), QPointF(19.6, 6.6));
        QPainterPath handle;
        handle.moveTo(9.0, 6.6);
        handle.lineTo(9.0, 4.8);
        handle.quadTo(9.0, 4.0, 9.8, 4.0);
        handle.lineTo(14.2, 4.0);
        handle.quadTo(15.0, 4.0, 15.0, 4.8);
        handle.lineTo(15.0, 6.6);
        p.drawPath(handle);

        QPainterPath bin;
        bin.moveTo(6.2, 6.6);
        bin.lineTo(7.2, 18.6);
        bin.quadTo(7.3, 19.8, 8.6, 19.8);
        bin.lineTo(15.4, 19.8);
        bin.quadTo(16.7, 19.8, 16.8, 18.6);
        bin.lineTo(17.8, 6.6);
        p.drawPath(bin);

        p.drawLine(QPointF(10.2, 10.2), QPointF(10.5, 16.2));
        p.drawLine(QPointF(13.8, 10.2), QPointF(13.5, 16.2));
        return;
    }

    if (name == QLatin1String("edit")) {
        QPainterPath pencil;
        pencil.moveTo(4.2, 19.8);
        pencil.lineTo(5.4, 15.4);
        pencil.lineTo(16.0, 4.8);
        pencil.quadTo(17.2, 3.6, 18.4, 4.8);
        pencil.lineTo(19.2, 5.6);
        pencil.quadTo(20.4, 6.8, 19.2, 8.0);
        pencil.lineTo(8.6, 18.6);
        pencil.closeSubpath();
        p.drawPath(pencil);
        p.drawLine(QPointF(14.0, 6.8), QPointF(17.2, 10.0));
        return;
    }

    if (name == QLatin1String("chevron-right")) {
        QPainterPath ch;
        ch.moveTo(9.2, 6.0);
        ch.lineTo(15.2, 12.0);
        ch.lineTo(9.2, 18.0);
        p.drawPath(ch);
        return;
    }

    if (name == QLatin1String("chevron-down")) {
        QPainterPath ch;
        ch.moveTo(6.0, 9.2);
        ch.lineTo(12.0, 15.2);
        ch.lineTo(18.0, 9.2);
        p.drawPath(ch);
        return;
    }

    if (name == QLatin1String("chevron-left")) {
        QPainterPath ch;
        ch.moveTo(14.8, 6.0);
        ch.lineTo(8.8, 12.0);
        ch.lineTo(14.8, 18.0);
        p.drawPath(ch);
        return;
    }

    if (name == QLatin1String("chevron-up")) {
        QPainterPath ch;
        ch.moveTo(6.0, 14.8);
        ch.lineTo(12.0, 8.8);
        ch.lineTo(18.0, 14.8);
        p.drawPath(ch);
        return;
    }

    if (name == QLatin1String("split")) {
        p.drawRoundedRect(QRectF(3.4, 4.6, 17.2, 14.8), 2.2, 2.2);
        p.drawLine(QPointF(12.0, 4.6), QPointF(12.0, 19.4));
        return;
    }

    if (name == QLatin1String("terminal-popout") || name == QLatin1String("popout")) {
        // Sleek macwindow.on.rectangle / Spell CLI terminal pop-out mark:
        // offset back window frame peeking cleanly behind front rounded window with `>_` prompt.
        QPainterPath back;
        back.moveTo(7.0, 7.6);
        back.lineTo(7.0, 5.2);
        back.quadTo(7.0, 3.6, 8.6, 3.6);
        back.lineTo(19.4, 3.6);
        back.quadTo(21.0, 3.6, 21.0, 5.2);
        back.lineTo(21.0, 14.4);
        back.quadTo(21.0, 16.0, 19.4, 16.0);
        back.lineTo(17.6, 16.0);
        p.drawPath(back);

        p.drawRoundedRect(QRectF(3.0, 7.6, 14.6, 12.8), 2.1, 2.1);

        QPen promptPen(
            color,
            std::max<qreal>(static_cast<qreal>(1.30), strokeWidth * static_cast<qreal>(0.94)),
            Qt::SolidLine,
            Qt::RoundCap,
            Qt::RoundJoin);
        p.setPen(promptPen);
        QPainterPath chevron;
        chevron.moveTo(6.2, 11.6);
        chevron.lineTo(9.1, 14.0);
        chevron.lineTo(6.2, 16.4);
        p.drawPath(chevron);
        p.drawLine(QPointF(10.8, 16.4), QPointF(14.4, 16.4));
        return;
    }

    if (name == QLatin1String("external-link")) {
        QPainterPath box;
        box.moveTo(11.0, 5.2);
        box.lineTo(6.4, 5.2);
        box.quadTo(4.8, 5.2, 4.8, 6.8);
        box.lineTo(4.8, 17.6);
        box.quadTo(4.8, 19.2, 6.4, 19.2);
        box.lineTo(17.2, 19.2);
        box.quadTo(18.8, 19.2, 18.8, 17.6);
        box.lineTo(18.8, 13.0);
        p.drawPath(box);

        p.drawLine(QPointF(11.4, 12.6), QPointF(19.4, 4.6));
        QPainterPath tip;
        tip.moveTo(14.2, 4.6);
        tip.lineTo(19.4, 4.6);
        tip.lineTo(19.4, 9.8);
        p.drawPath(tip);
        return;
    }

    if (name == QLatin1String("expand")) {
        // Opposing diagonal outward arrows (arrow.up.left.and.arrow.down.right)
        p.drawLine(QPointF(4.6, 4.6), QPointF(10.0, 10.0));
        QPainterPath topLeft;
        topLeft.moveTo(9.8, 4.6);
        topLeft.lineTo(4.6, 4.6);
        topLeft.lineTo(4.6, 9.8);
        p.drawPath(topLeft);

        p.drawLine(QPointF(19.4, 19.4), QPointF(14.0, 14.0));
        QPainterPath bottomRight;
        bottomRight.moveTo(19.4, 14.2);
        bottomRight.lineTo(19.4, 19.4);
        bottomRight.lineTo(14.2, 19.4);
        p.drawPath(bottomRight);
        return;
    }

    if (name == QLatin1String("collapse")) {
        // Opposing diagonal inward arrows (arrow.down.right.and.arrow.up.left)
        p.drawLine(QPointF(4.2, 4.2), QPointF(10.0, 10.0));
        QPainterPath topLeft;
        topLeft.moveTo(10.0, 5.2);
        topLeft.lineTo(10.0, 10.0);
        topLeft.lineTo(5.2, 10.0);
        p.drawPath(topLeft);

        p.drawLine(QPointF(19.8, 19.8), QPointF(14.0, 14.0));
        QPainterPath bottomRight;
        bottomRight.moveTo(14.0, 18.8);
        bottomRight.lineTo(14.0, 14.0);
        bottomRight.lineTo(18.8, 14.0);
        p.drawPath(bottomRight);
        return;
    }

    if (name == QLatin1String("bell")) {
        QPainterPath bell;
        bell.moveTo(5.0, 16.6);
        bell.cubicTo(6.4, 15.2, 6.8, 13.2, 6.8, 10.6);
        bell.cubicTo(6.8, 7.2, 9.1, 4.8, 12.0, 4.8);
        bell.cubicTo(14.9, 4.8, 17.2, 7.2, 17.2, 10.6);
        bell.cubicTo(17.2, 13.2, 17.6, 15.2, 19.0, 16.6);
        bell.closeSubpath();
        p.drawPath(bell);
        p.drawLine(QPointF(12.0, 3.2), QPointF(12.0, 4.6));
        p.drawArc(QRectF(9.6, 16.6, 4.8, 3.6), 200 * 16, 140 * 16);
        return;
    }

    if (name == QLatin1String("shield")) {
        QPainterPath sh;
        sh.moveTo(12.0, 3.2);
        sh.lineTo(19.2, 6.2);
        sh.lineTo(19.2, 11.8);
        sh.cubicTo(19.2, 16.2, 16.1, 19.2, 12.0, 20.8);
        sh.cubicTo(7.9, 19.2, 4.8, 16.2, 4.8, 11.8);
        sh.lineTo(4.8, 6.2);
        sh.closeSubpath();
        p.drawPath(sh);
        return;
    }

    if (name == QLatin1String("lock-shield")) {
        QPainterPath sh;
        sh.moveTo(12.0, 2.8);
        sh.lineTo(19.6, 5.8);
        sh.lineTo(19.6, 11.6);
        sh.cubicTo(19.6, 16.4, 16.3, 19.5, 12.0, 21.2);
        sh.cubicTo(7.7, 19.5, 4.4, 16.4, 4.4, 11.6);
        sh.lineTo(4.4, 5.8);
        sh.closeSubpath();
        p.drawPath(sh);

        QPen shacklePen(
            color,
            std::max<qreal>(static_cast<qreal>(1.30), strokeWidth * static_cast<qreal>(0.88)),
            Qt::SolidLine,
            Qt::RoundCap,
            Qt::RoundJoin);
        p.setPen(shacklePen);
        QPainterPath shackle;
        shackle.moveTo(9.9, 11.2);
        shackle.lineTo(9.9, 9.4);
        shackle.cubicTo(9.9, 7.9, 10.8, 7.1, 12.0, 7.1);
        shackle.cubicTo(13.2, 7.1, 14.1, 7.9, 14.1, 9.4);
        shackle.lineTo(14.1, 11.2);
        p.drawPath(shackle);

        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(8.5, 11.0, 7.0, 5.4), 1.3, 1.3);
        return;
    }

    if (name == QLatin1String("globe")) {
        p.drawEllipse(QPointF(12.0, 12.0), 8.4, 8.4);
        p.drawLine(QPointF(3.6, 12.0), QPointF(20.4, 12.0));
        p.drawEllipse(QRectF(8.3, 3.6, 7.4, 16.8));
        return;
    }

    if (name == QLatin1String("cpu")) {
        p.drawRoundedRect(QRectF(6.8, 6.8, 10.4, 10.4), 1.6, 1.6);
        p.drawRect(QRectF(10.0, 10.0, 4.0, 4.0));
        // Top & bottom pins
        p.drawLine(QPointF(9.5, 3.6), QPointF(9.5, 6.8));
        p.drawLine(QPointF(14.5, 3.6), QPointF(14.5, 6.8));
        p.drawLine(QPointF(9.5, 17.2), QPointF(9.5, 20.4));
        p.drawLine(QPointF(14.5, 17.2), QPointF(14.5, 20.4));
        // Left & right pins
        p.drawLine(QPointF(3.6, 9.5), QPointF(6.8, 9.5));
        p.drawLine(QPointF(3.6, 14.5), QPointF(6.8, 14.5));
        p.drawLine(QPointF(17.2, 9.5), QPointF(20.4, 9.5));
        p.drawLine(QPointF(17.2, 14.5), QPointF(20.4, 14.5));
        return;
    }

    if (name == QLatin1String("speedometer")) {
        p.drawArc(QRectF(3.8, 5.6, 16.4, 16.4), -25 * 16, 230 * 16);
        p.drawLine(QPointF(12.0, 14.0), QPointF(16.6, 9.4));
        drawDot(12.0, 14.0, 1.45);
        return;
    }

    if (name == QLatin1String("tag")) {
        QPainterPath t;
        t.moveTo(4.2, 11.0);
        t.lineTo(4.2, 5.6);
        t.quadTo(4.2, 4.2, 5.6, 4.2);
        t.lineTo(11.0, 4.2);
        t.lineTo(19.4, 12.6);
        t.quadTo(20.2, 13.4, 19.4, 14.2);
        t.lineTo(14.2, 19.4);
        t.quadTo(13.4, 20.2, 12.6, 19.4);
        t.closeSubpath();
        p.drawPath(t);
        drawDot(8.2, 8.2, 1.15);
        return;
    }

    if (name == QLatin1String("stethoscope")) {
        p.drawLine(QPointF(4.6, 4.2), QPointF(6.6, 4.2));
        p.drawLine(QPointF(13.4, 4.2), QPointF(15.4, 4.2));

        QPainterPath binaural;
        binaural.moveTo(5.6, 4.2);
        binaural.lineTo(5.6, 8.8);
        binaural.cubicTo(5.6, 11.8, 7.6, 13.6, 10.0, 13.6);
        binaural.cubicTo(12.4, 13.6, 14.4, 11.8, 14.4, 8.8);
        binaural.lineTo(14.4, 4.2);
        p.drawPath(binaural);

        QPainterPath tube;
        tube.moveTo(10.0, 13.6);
        tube.lineTo(10.0, 16.4);
        tube.cubicTo(10.0, 18.8, 11.8, 20.2, 14.2, 20.2);
        tube.cubicTo(16.6, 20.2, 18.4, 18.8, 18.4, 16.4);
        tube.lineTo(18.4, 13.8);
        p.drawPath(tube);

        p.drawEllipse(QPointF(18.4, 11.8), 2.1, 2.1);
        return;
    }

    if (name == QLatin1String("info")) {
        p.drawEllipse(QPointF(12.0, 12.0), 8.4, 8.4);
        p.drawLine(QPointF(12.0, 11.0), QPointF(12.0, 16.2));
        drawDot(12.0, 7.8, 1.15);
        return;
    }

    if (name == QLatin1String("paintbrush")) {
        // Handle
        QPainterPath handle;
        handle.moveTo(18.6, 3.8);
        handle.cubicTo(19.6, 4.8, 19.6, 5.8, 18.4, 7.0);
        handle.lineTo(12.4, 13.0);
        handle.lineTo(11.0, 11.6);
        handle.lineTo(17.0, 5.6);
        handle.cubicTo(17.8, 4.8, 18.1, 4.2, 18.6, 3.8);
        handle.closeSubpath();
        p.drawPath(handle);

        // Bristle teardrop
        QPainterPath tip;
        tip.moveTo(11.2, 12.8);
        tip.cubicTo(9.4, 12.2, 7.2, 13.2, 6.8, 15.2);
        tip.cubicTo(6.4, 17.0, 5.2, 18.4, 3.8, 19.2);
        tip.cubicTo(6.4, 20.2, 10.6, 19.4, 11.8, 16.8);
        tip.cubicTo(12.4, 15.4, 12.0, 13.6, 11.2, 12.8);
        tip.closeSubpath();
        p.drawPath(tip);
        return;
    }

    if (name == QLatin1String("server")) {
        p.drawRoundedRect(QRectF(3.6, 4.4, 16.8, 6.2), 1.6, 1.6);
        p.drawRoundedRect(QRectF(3.6, 13.4, 16.8, 6.2), 1.6, 1.6);
        drawDot(7.0, 7.5, 1.05);
        drawDot(7.0, 16.5, 1.05);
        p.drawLine(QPointF(14.4, 7.5), QPointF(17.4, 7.5));
        p.drawLine(QPointF(14.4, 16.5), QPointF(17.4, 16.5));
        return;
    }

    if (name == QLatin1String("repeat")) {
        QPainterPath top;
        top.moveTo(4.6, 11.0);
        top.lineTo(4.6, 9.2);
        top.quadTo(4.6, 7.0, 6.8, 7.0);
        top.lineTo(19.0, 7.0);
        p.drawPath(top);

        QPainterPath topHead;
        topHead.moveTo(16.2, 4.4);
        topHead.lineTo(19.2, 7.0);
        topHead.lineTo(16.2, 9.6);
        p.drawPath(topHead);

        QPainterPath bottom;
        bottom.moveTo(19.4, 13.0);
        bottom.lineTo(19.4, 14.8);
        bottom.quadTo(19.4, 17.0, 17.2, 17.0);
        bottom.lineTo(5.0, 17.0);
        p.drawPath(bottom);

        QPainterPath bottomHead;
        bottomHead.moveTo(7.8, 14.4);
        bottomHead.lineTo(4.8, 17.0);
        bottomHead.lineTo(7.8, 19.6);
        p.drawPath(bottomHead);
        return;
    }

    if (name == QLatin1String("arrow-left")) {
        p.drawLine(QPointF(19.0, 12.0), QPointF(5.4, 12.0));
        QPainterPath head;
        head.moveTo(11.0, 6.4);
        head.lineTo(5.4, 12.0);
        head.lineTo(11.0, 17.6);
        p.drawPath(head);
        return;
    }

    if (name == QLatin1String("arrow-right")) {
        p.drawLine(QPointF(5.0, 12.0), QPointF(18.6, 12.0));
        QPainterPath head;
        head.moveTo(13.0, 6.4);
        head.lineTo(18.6, 12.0);
        head.lineTo(13.0, 17.6);
        p.drawPath(head);
        return;
    }

    if (name == QLatin1String("arrow-up")) {
        QPen boldStroke(
            color,
            strokeWidth * static_cast<qreal>(1.18),
            Qt::SolidLine,
            Qt::RoundCap,
            Qt::RoundJoin);
        p.setPen(boldStroke);
        p.drawLine(QPointF(12.0, 19.0), QPointF(12.0, 5.0));
        QPainterPath head;
        head.moveTo(6.4, 10.6);
        head.lineTo(12.0, 5.0);
        head.lineTo(17.6, 10.6);
        p.drawPath(head);
        return;
    }

    if (name == QLatin1String("lock")) {
        p.drawRoundedRect(QRectF(5.4, 11.0, 13.2, 9.2), 2.0, 2.0);
        QPainterPath shackle;
        shackle.moveTo(8.4, 11.0);
        shackle.lineTo(8.4, 7.8);
        shackle.cubicTo(8.4, 5.2, 10.0, 4.0, 12.0, 4.0);
        shackle.cubicTo(14.0, 4.0, 15.6, 5.2, 15.6, 7.8);
        shackle.lineTo(15.6, 11.0);
        p.drawPath(shackle);
        drawDot(12.0, 15.4, 1.15);
        return;
    }

    if (name == QLatin1String("unlock")) {
        p.drawRoundedRect(QRectF(5.4, 11.0, 13.2, 9.2), 2.0, 2.0);
        QPainterPath shackle;
        shackle.moveTo(8.4, 11.0);
        shackle.lineTo(8.4, 7.8);
        shackle.cubicTo(8.4, 5.2, 10.0, 4.0, 12.0, 4.0);
        shackle.cubicTo(14.0, 4.0, 15.5, 5.2, 15.6, 7.4);
        p.drawPath(shackle);
        drawDot(12.0, 15.4, 1.15);
        return;
    }

    if (name == QLatin1String("send")) {
        QPainterPath plane;
        plane.moveTo(20.0, 4.0);
        plane.lineTo(13.4, 20.0);
        plane.lineTo(10.6, 13.4);
        plane.lineTo(4.0, 10.6);
        plane.closeSubpath();
        p.drawPath(plane);
        p.drawLine(QPointF(20.0, 4.0), QPointF(10.6, 13.4));
        return;
    }

    if (name == QLatin1String("paperclip")) {
        QPainterPath clip;
        clip.moveTo(15.2, 7.4);
        clip.lineTo(8.4, 14.2);
        clip.cubicTo(7.2, 15.4, 7.2, 17.2, 8.4, 18.4);
        clip.cubicTo(9.6, 19.6, 11.4, 19.6, 12.6, 18.4);
        clip.lineTo(19.4, 11.6);
        clip.cubicTo(21.2, 9.8, 21.2, 6.8, 19.4, 5.0);
        clip.cubicTo(17.6, 3.2, 14.6, 3.2, 12.8, 5.0);
        clip.lineTo(5.6, 12.2);
        clip.cubicTo(4.4, 13.4, 4.4, 15.2, 5.6, 16.4);
        p.drawPath(clip);
        return;
    }

    if (name == QLatin1String("hat")) {
        drawFallbackWitchHat(p, color);
        return;
    }

    if (name == QLatin1String("sparkles")) {
        QPainterPath star;
        star.moveTo(12.0, 3.2);
        star.quadTo(12.6, 11.4, 20.8, 12.0);
        star.quadTo(12.6, 12.6, 12.0, 20.8);
        star.quadTo(11.4, 12.6, 3.2, 12.0);
        star.quadTo(11.4, 11.4, 12.0, 3.2);
        star.closeSubpath();
        p.drawPath(star);
        return;
    }

    if (name == QLatin1String("warning")) {
        QPainterPath tri;
        tri.moveTo(12.0, 4.0);
        tri.lineTo(20.4, 19.2);
        tri.lineTo(3.6, 19.2);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawLine(QPointF(12.0, 9.6), QPointF(12.0, 13.8));
        drawDot(12.0, 16.6, 1.0);
        return;
    }

    if (name == QLatin1String("code")) {
        QPainterPath left;
        left.moveTo(8.4, 7.6);
        left.lineTo(4.0, 12.0);
        left.lineTo(8.4, 16.4);
        p.drawPath(left);
        QPainterPath right;
        right.moveTo(15.6, 7.6);
        right.lineTo(20.0, 12.0);
        right.lineTo(15.6, 16.4);
        p.drawPath(right);
        p.drawLine(QPointF(13.4, 6.2), QPointF(10.6, 17.8));
        return;
    }

    if (name == QLatin1String("menu")) {
        p.drawLine(QPointF(4.5, 7.0), QPointF(19.5, 7.0));
        p.drawLine(QPointF(4.5, 12.0), QPointF(19.5, 12.0));
        p.drawLine(QPointF(4.5, 17.0), QPointF(19.5, 17.0));
        return;
    }

    // Minimal geometric fallback for unrecognized icon names
    p.drawRoundedRect(QRectF(5.5, 5.5, 13.0, 13.0), 3.0, 3.0);
}

inline QPixmap renderVectorPixmap(const QString& name, const QColor& color, int logicalSize, qreal dpr) {
    const int safeSize = std::max(1, logicalSize);
    const int px = std::max(1, qRound(safeSize * dpr));
    QImage canvas(px, px, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.scale(static_cast<qreal>(px) / 24.0, static_cast<qreal>(px) / 24.0);
    const qreal strokeWidth = std::clamp(
        static_cast<qreal>(1.4 * 24.0) / static_cast<qreal>(safeSize),
        static_cast<qreal>(1.40),
        static_cast<qreal>(1.75));
    paintVectorGlyph(p, name, color, strokeWidth);
    p.end();

    QPixmap pm = QPixmap::fromImage(canvas);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

inline QPixmap renderAppLogoPixmap(int logicalSize, qreal dpr, const QImage& logoImage) {
    const int safeSize = std::max(1, logicalSize);
    const int px = std::max(1, qRound(safeSize * dpr));

    QImage canvas(px, px, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal radius = static_cast<qreal>(px) * 0.225;
    const QRectF bounds(0.0, 0.0, static_cast<qreal>(px), static_cast<qreal>(px));
    QPainterPath clipPath;
    clipPath.addRoundedRect(bounds, radius, radius);

    p.save();
    p.setClipPath(clipPath);
    if (!logoImage.isNull()) {
        const QImage scaled = logoImage.scaled(
            px, px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int sx = (scaled.width() - px) / 2;
        const int sy = (scaled.height() - px) / 2;
        p.drawImage(QRect(0, 0, px, px), scaled, QRect(sx, sy, px, px));
    } else {
        p.fillRect(bounds, QColor(38, 34, 35));
        p.save();
        const qreal inset = static_cast<qreal>(px) * 0.10;
        const qreal drawSpan = static_cast<qreal>(px) - 2.0 * inset;
        p.translate(inset, inset);
        p.scale(drawSpan / 24.0, drawSpan / 24.0);
        drawFallbackWitchHat(p, QColor(255, 255, 255));
        p.restore();
    }
    p.restore();

    if (logoImage.isNull()) {
        p.setPen(QPen(QColor(255, 255, 255, 31), std::max<qreal>(1.0, px * 0.015)));
        p.setBrush(Qt::NoBrush);
        p.drawPath(clipPath);
    }

    p.end();

    QPixmap pm = QPixmap::fromImage(canvas);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

} // namespace detail

/// Locates a shipped Spell IDE resource file across macOS bundle Contents/Resources,
/// executable /Resources, current working directory /Resources, and parent /Resources dirs.
inline QString findResourceFile(const QString& filename) {
    if (filename.isEmpty()) {
        return {};
    }
    if (QFileInfo::exists(filename) && QFileInfo(filename).isFile()) {
        return QFileInfo(filename).canonicalFilePath();
    }

    QStringList searchDirs;
    auto addDir = [&searchDirs](const QString& dirPath) {
        if (dirPath.isEmpty()) {
            return;
        }
        const QString cleaned = QDir::cleanPath(dirPath);
        if (!searchDirs.contains(cleaned)) {
            searchDirs.append(cleaned);
        }
    };

    if (QCoreApplication::instance() != nullptr) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            // macOS .app/Contents/MacOS -> ../Resources
            addDir(appDir + QStringLiteral("/../Resources"));
            // Linux FHS / AppDir usr/bin -> ../share/BeatrixLeslieIDE/Resources
            addDir(appDir + QStringLiteral("/../share/BeatrixLeslieIDE/Resources"));
            addDir(appDir + QStringLiteral("/Resources"));
            addDir(appDir);

            QDir walker(appDir);
            for (int depth = 0; depth < 5; ++depth) {
                if (!walker.cdUp()) {
                    break;
                }
                addDir(walker.filePath(QStringLiteral("Resources")));
                addDir(walker.absolutePath());
            }
        }
    }

    const QString cwd = QDir::currentPath();
    if (!cwd.isEmpty()) {
        addDir(cwd + QStringLiteral("/Resources"));
        addDir(cwd);

        QDir walker(cwd);
        for (int depth = 0; depth < 5; ++depth) {
            if (!walker.cdUp()) {
                break;
            }
            addDir(walker.filePath(QStringLiteral("Resources")));
        }
    }

    for (const QString& dirPath : searchDirs) {
        const QString candidate = QDir(dirPath).filePath(filename);
        const QFileInfo fi(candidate);
        if (fi.exists() && fi.isFile()) {
            return fi.absoluteFilePath();
        }
    }
    return {};
}

namespace detail {

inline const QImage& sharedAppLogoImage() {
    static QImage cachedSource;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        QString path = findResourceFile(QStringLiteral("AppIconRounded.png"));
        if (path.isEmpty()) {
            path = findResourceFile(QStringLiteral("AppLogo.jpg"));
        }
        cachedSource = loadCachedImage(path);
    }
    return cachedSource;
}

} // namespace detail

/// Loads AppIconRounded.png (or AppLogo.jpg fallback) clipped into a rounded square.
inline QPixmap appLogoPixmap(int size = 64) {
    return detail::renderAppLogoPixmap(size, detail::currentDevicePixelRatio(), detail::sharedAppLogoImage());
}

/// Multi-resolution application window & dock/taskbar icon for BeatrixLeslie IDE.
inline QIcon appWindowIcon() {
    const QImage& source = detail::sharedAppLogoImage();
    QIcon icon;
    for (int sz : {16, 32, 64, 128, 256, 512}) {
        icon.addPixmap(detail::renderAppLogoPixmap(sz, 1.0, source));
    }
    return icon;
}

/// Loads HatSilhouetteTemplate.png and tints it cleanly using CompositionMode_SourceIn,
/// falling back to a crisp vector witch hat silhouette if the PNG is unavailable.
inline QPixmap hatMarkPixmap(int size, const QColor& tint) {
    static QImage cachedHat;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const QString path = findResourceFile(QStringLiteral("HatSilhouetteTemplate.png"));
        cachedHat = detail::loadCachedImage(path);
    }

    const int safeSize = std::max(1, size);
    const qreal dpr = detail::currentDevicePixelRatio();
    const int px = std::max(1, qRound(safeSize * dpr));

    QImage canvas(px, px, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (!cachedHat.isNull()) {
        const QImage scaled = cachedHat.scaled(
            px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const int x = (px - scaled.width()) / 2;
        const int y = (px - scaled.height()) / 2;
        p.drawImage(x, y, scaled);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(canvas.rect(), tint);
    } else {
        p.scale(static_cast<qreal>(px) / 24.0, static_cast<qreal>(px) / 24.0);
        detail::drawFallbackWitchHat(p, tint);
    }
    p.end();

    QPixmap pm = QPixmap::fromImage(canvas);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

/// Renders a crisp, monochrome-friendly vector UI icon at the requested logical size.
inline QIcon vectorIcon(const QString& name, const QColor& color, int size = 16) {
    const int safeSize = std::max(1, size);
    QColor disabledColor(color);
    disabledColor.setAlpha(std::max(1, qRound(color.alpha() * 0.40)));

    QIcon icon;
    icon.addPixmap(detail::renderVectorPixmap(name, color, safeSize, 1.0), QIcon::Normal);
    icon.addPixmap(detail::renderVectorPixmap(name, color, safeSize, 2.0), QIcon::Normal);
    icon.addPixmap(detail::renderVectorPixmap(name, disabledColor, safeSize, 1.0), QIcon::Disabled);
    icon.addPixmap(detail::renderVectorPixmap(name, disabledColor, safeSize, 2.0), QIcon::Disabled);
    return icon;
}

} // namespace spell::icons
