#include "Icons.h"

#include <QApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <functional>

namespace {

using DrawFn = std::function<void(QPainter &, const QColor &fg, const QColor &accent)>;

// Draws in a 24x24 logical box.
const QHash<QString, DrawFn> &drawers()
{
    static QHash<QString, DrawFn> d;
    if (!d.isEmpty())
        return d;
    auto pen = [](QPainter &p, const QColor &c, double w = 1.6) {
        QPen pn(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pn);
        p.setBrush(Qt::NoBrush);
    };
    d.insert(QStringLiteral("new"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        QPainterPath path;
        path.moveTo(6, 3); path.lineTo(14, 3); path.lineTo(19, 8); path.lineTo(19, 21); path.lineTo(6, 21); path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(14, 3), QPointF(14, 8));
        p.drawLine(QPointF(14, 8), QPointF(19, 8));
    });
    d.insert(QStringLiteral("open"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        QPainterPath path;
        path.moveTo(3, 6); path.lineTo(9, 6); path.lineTo(11, 8); path.lineTo(20, 8); path.lineTo(20, 19); path.lineTo(3, 19); path.closeSubpath();
        p.drawPath(path);
        pen(p, a);
        p.drawLine(QPointF(5, 12), QPointF(22, 12));
    });
    d.insert(QStringLiteral("save"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        p.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
        p.drawRect(QRectF(8, 4, 8, 5));
        pen(p, a);
        p.drawRect(QRectF(7, 13, 10, 7));
    });
    d.insert(QStringLiteral("run"), [](QPainter &p, const QColor &, const QColor &) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(32, 150, 60));
        QPainterPath path;
        path.moveTo(7, 4); path.lineTo(20, 12); path.lineTo(7, 20); path.closeSubpath();
        p.drawPath(path);
    });
    d.insert(QStringLiteral("pause"), [](QPainter &p, const QColor &, const QColor &) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 160, 20));
        p.drawRect(QRectF(6, 5, 4, 14));
        p.drawRect(QRectF(14, 5, 4, 14));
    });
    d.insert(QStringLiteral("stop"), [](QPainter &p, const QColor &, const QColor &) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(200, 40, 40));
        p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
    });
    d.insert(QStringLiteral("wire"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, a, 2);
        p.drawLine(QPointF(3, 18), QPointF(12, 18));
        p.drawLine(QPointF(12, 18), QPointF(12, 6));
        p.drawLine(QPointF(12, 6), QPointF(21, 6));
        pen(p, fg, 1);
        p.setBrush(fg);
        p.drawEllipse(QPointF(12, 18), 1.6, 1.6);
    });
    d.insert(QStringLiteral("ground"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawLine(QPointF(12, 3), QPointF(12, 11));
        QPainterPath path;
        path.moveTo(4, 11); path.lineTo(20, 11); path.lineTo(12, 20); path.closeSubpath();
        p.drawPath(path);
    });
    d.insert(QStringLiteral("label"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        QPainterPath path;
        path.moveTo(3, 7); path.lineTo(16, 7); path.lineTo(21, 12); path.lineTo(16, 17); path.lineTo(3, 17); path.closeSubpath();
        p.drawPath(path);
        pen(p, a);
        p.drawLine(QPointF(6, 12), QPointF(13, 12));
    });
    d.insert(QStringLiteral("resistor"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        QPainterPath path;
        path.moveTo(2, 12); path.lineTo(5, 12); path.lineTo(7, 7); path.lineTo(10, 17); path.lineTo(13, 7); path.lineTo(16, 17);
        path.lineTo(18, 12); path.lineTo(22, 12);
        p.drawPath(path);
    });
    d.insert(QStringLiteral("capacitor"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawLine(QPointF(2, 12), QPointF(10, 12));
        p.drawLine(QPointF(14, 12), QPointF(22, 12));
        pen(p, fg, 2.2);
        p.drawLine(QPointF(10, 5), QPointF(10, 19));
        p.drawLine(QPointF(14, 5), QPointF(14, 19));
    });
    d.insert(QStringLiteral("inductor"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawLine(QPointF(1, 14), QPointF(4, 14));
        for (int i = 0; i < 4; ++i)
            p.drawArc(QRectF(4 + i * 4, 9, 4, 10), 0, 180 * 16);
        p.drawLine(QPointF(20, 14), QPointF(23, 14));
    });
    d.insert(QStringLiteral("diode"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawLine(QPointF(2, 12), QPointF(8, 12));
        p.drawLine(QPointF(16, 12), QPointF(22, 12));
        QPainterPath path;
        path.moveTo(8, 6); path.lineTo(16, 12); path.lineTo(8, 18); path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(16, 6), QPointF(16, 18));
    });
    d.insert(QStringLiteral("component"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        p.drawRect(QRectF(7, 5, 10, 14));
        for (int y : {8, 12, 16}) {
            p.drawLine(QPointF(3, y), QPointF(7, y));
            p.drawLine(QPointF(17, y), QPointF(21, y));
        }
        pen(p, a);
        p.drawEllipse(QPointF(10, 8), 1, 1);
    });
    d.insert(QStringLiteral("text"), [](QPainter &p, const QColor &fg, const QColor &) {
        QFont f = p.font();
        f.setPixelSize(18);
        f.setBold(true);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, QStringLiteral("Aa"));
    });
    d.insert(QStringLiteral("directive"), [](QPainter &p, const QColor &fg, const QColor &a) {
        QFont f = p.font();
        f.setPixelSize(14);
        f.setBold(true);
        p.setFont(f);
        p.setPen(a);
        p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, QStringLiteral(".op"));
        Q_UNUSED(fg);
    });
    d.insert(QStringLiteral("move"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawLine(QPointF(12, 3), QPointF(12, 21));
        p.drawLine(QPointF(3, 12), QPointF(21, 12));
        for (auto [x, y, dx, dy] : {std::tuple{12, 3, 3, 3}, {12, 21, 3, -3}, {3, 12, 3, 3}, {21, 12, -3, 3}}) {
            if (dx == dy || dx == -dy) {}
            Q_UNUSED(dx);
            Q_UNUSED(dy);
            Q_UNUSED(x);
            Q_UNUSED(y);
        }
        p.drawLine(QPointF(12, 3), QPointF(9, 6)); p.drawLine(QPointF(12, 3), QPointF(15, 6));
        p.drawLine(QPointF(12, 21), QPointF(9, 18)); p.drawLine(QPointF(12, 21), QPointF(15, 18));
        p.drawLine(QPointF(3, 12), QPointF(6, 9)); p.drawLine(QPointF(3, 12), QPointF(6, 15));
        p.drawLine(QPointF(21, 12), QPointF(18, 9)); p.drawLine(QPointF(21, 12), QPointF(18, 15));
    });
    d.insert(QStringLiteral("drag"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        p.drawRect(QRectF(10, 4, 10, 8));
        pen(p, a);
        p.drawLine(QPointF(3, 20), QPointF(10, 20));
        p.drawLine(QPointF(10, 20), QPointF(10, 12));
    });
    d.insert(QStringLiteral("delete"), [pen](QPainter &p, const QColor &, const QColor &) {
        pen(p, QColor(200, 40, 40), 2.4);
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
    });
    d.insert(QStringLiteral("copy"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawRect(QRectF(4, 4, 11, 13));
        p.drawRect(QRectF(9, 8, 11, 13));
    });
    d.insert(QStringLiteral("paste"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        p.drawRect(QRectF(5, 5, 14, 16));
        pen(p, a);
        p.drawRect(QRectF(9, 3, 6, 4));
    });
    d.insert(QStringLiteral("undo"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg, 2);
        p.drawArc(QRectF(6, 6, 14, 12), 90 * 16, -200 * 16);
        p.drawLine(QPointF(13, 6), QPointF(8, 3));
        p.drawLine(QPointF(13, 6), QPointF(9, 10));
    });
    d.insert(QStringLiteral("redo"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg, 2);
        p.drawArc(QRectF(4, 6, 14, 12), 90 * 16, 200 * 16);
        p.drawLine(QPointF(11, 6), QPointF(16, 3));
        p.drawLine(QPointF(11, 6), QPointF(15, 10));
    });
    d.insert(QStringLiteral("rotate"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg, 2);
        p.drawArc(QRectF(5, 5, 14, 14), 30 * 16, 300 * 16);
        p.drawLine(QPointF(18, 8), QPointF(18, 3));
        p.drawLine(QPointF(18, 8), QPointF(13, 8));
    });
    d.insert(QStringLiteral("mirror"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, a, 1);
        p.drawLine(QPointF(12, 2), QPointF(12, 22));
        pen(p, fg);
        QPainterPath l, r;
        l.moveTo(10, 6); l.lineTo(3, 18); l.lineTo(10, 18); l.closeSubpath();
        r.moveTo(14, 6); r.lineTo(21, 18); r.lineTo(14, 18); r.closeSubpath();
        p.drawPath(l);
        p.drawPath(r);
    });
    d.insert(QStringLiteral("zoomin"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg, 2);
        p.drawEllipse(QRectF(3, 3, 13, 13));
        p.drawLine(QPointF(14, 14), QPointF(21, 21));
        p.drawLine(QPointF(6, 9.5), QPointF(13, 9.5));
        p.drawLine(QPointF(9.5, 6), QPointF(9.5, 13));
    });
    d.insert(QStringLiteral("zoomout"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg, 2);
        p.drawEllipse(QRectF(3, 3, 13, 13));
        p.drawLine(QPointF(14, 14), QPointF(21, 21));
        p.drawLine(QPointF(6, 9.5), QPointF(13, 9.5));
    });
    d.insert(QStringLiteral("zoomfit"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg, 2);
        p.drawLine(QPointF(3, 8), QPointF(3, 3)); p.drawLine(QPointF(3, 3), QPointF(8, 3));
        p.drawLine(QPointF(16, 3), QPointF(21, 3)); p.drawLine(QPointF(21, 3), QPointF(21, 8));
        p.drawLine(QPointF(3, 16), QPointF(3, 21)); p.drawLine(QPointF(3, 21), QPointF(8, 21));
        p.drawLine(QPointF(16, 21), QPointF(21, 21)); p.drawLine(QPointF(21, 21), QPointF(21, 16));
        pen(p, a);
        p.drawRect(QRectF(8, 8, 8, 8));
    });
    d.insert(QStringLiteral("pan"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        QPainterPath path;
        path.moveTo(8, 20); path.lineTo(6, 12); path.lineTo(8, 11); path.lineTo(10, 14); path.lineTo(10, 5); path.lineTo(12, 5);
        path.lineTo(12, 11); path.lineTo(12, 4); path.lineTo(14, 4); path.lineTo(14, 11); path.lineTo(14, 5); path.lineTo(16, 5);
        path.lineTo(16, 12); path.lineTo(16, 7); path.lineTo(18, 7); path.lineTo(18, 15); path.lineTo(16, 20); path.closeSubpath();
        p.drawPath(path);
    });
    d.insert(QStringLiteral("tile"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawRect(QRectF(3, 4, 18, 7));
        p.drawRect(QRectF(3, 13, 18, 7));
    });
    d.insert(QStringLiteral("cascade"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawRect(QRectF(3, 3, 12, 10));
        p.drawRect(QRectF(6, 7, 12, 10));
        p.drawRect(QRectF(9, 11, 12, 10));
    });
    d.insert(QStringLiteral("close"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawRect(QRectF(3, 4, 18, 16));
        p.drawLine(QPointF(9, 9), QPointF(15, 15));
        p.drawLine(QPointF(15, 9), QPointF(9, 15));
    });
    d.insert(QStringLiteral("settings"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        for (int i = 0; i < 8; ++i) {
            p.save();
            p.translate(12, 12);
            p.rotate(i * 45);
            p.drawLine(QPointF(0, 6), QPointF(0, 9));
            p.restore();
        }
        p.drawEllipse(QPointF(12, 12), 6, 6);
        pen(p, a);
        p.drawEllipse(QPointF(12, 12), 2.5, 2.5);
    });
    d.insert(QStringLiteral("halt"), d.value(QStringLiteral("stop")));
    d.insert(QStringLiteral("probe"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        p.drawLine(QPointF(4, 20), QPointF(11, 13));
        pen(p, a, 2.5);
        p.drawLine(QPointF(11, 13), QPointF(19, 5));
    });
    d.insert(QStringLiteral("netlist"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg);
        p.drawRect(QRectF(5, 3, 14, 18));
        pen(p, a, 1.2);
        for (int y : {7, 11, 15})
            p.drawLine(QPointF(8, y), QPointF(16, y));
    });
    d.insert(QStringLiteral("waveform"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, fg, 1.2);
        p.drawRect(QRectF(3, 4, 18, 16));
        pen(p, a, 1.8);
        QPainterPath path;
        path.moveTo(4, 12);
        for (int x = 4; x <= 20; ++x)
            path.lineTo(x, 12 - 5 * std::sin((x - 4) * 0.8));
        p.drawPath(path);
    });
    d.insert(QStringLiteral("print"), [pen](QPainter &p, const QColor &fg, const QColor &) {
        pen(p, fg);
        p.drawRect(QRectF(7, 3, 10, 6));
        p.drawRect(QRectF(3, 9, 18, 8));
        p.drawRect(QRectF(7, 14, 10, 7));
    });
    d.insert(QStringLiteral("find"), d.value(QStringLiteral("zoomin")));
    d.insert(QStringLiteral("cursor"), [pen](QPainter &p, const QColor &fg, const QColor &a) {
        pen(p, a, 1);
        p.drawLine(QPointF(12, 2), QPointF(12, 22));
        p.drawLine(QPointF(2, 12), QPointF(22, 12));
        pen(p, fg);
        p.drawEllipse(QPointF(12, 12), 3, 3);
    });
    return d;
}

class PaintedIconEngine : public QIconEngine
{
public:
    explicit PaintedIconEngine(const QString &name) : m_name(name) {}
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        const auto &dr = drawers();
        auto it = dr.constFind(m_name);
        if (it == dr.constEnd())
            return;
        const QPalette pal = QApplication::palette();
        QColor fg = pal.color(QPalette::WindowText);
        QColor accent = pal.color(QPalette::Highlight);
        if (mode == QIcon::Disabled) {
            fg = pal.color(QPalette::Disabled, QPalette::WindowText);
            accent = fg;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 24.0, rect.height() / 24.0);
        if (mode == QIcon::Disabled)
            painter->setOpacity(0.5);
        it.value()(*painter, fg, accent);
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }
    QIconEngine *clone() const override { return new PaintedIconEngine(m_name); }

private:
    QString m_name;
};

} // namespace

namespace icons {

QIcon get(const QString &name)
{
    return QIcon(new PaintedIconEngine(name));
}

QIcon appIcon()
{
    QIcon icon(QStringLiteral(":/icons/linspice.svg"));
    if (icon.isNull())
        return get(QStringLiteral("waveform"));
    return icon;
}

} // namespace icons
