#include "SchematicPainter.h"

#include "core/AppSettings.h"
#include "core/Netlister.h"

#include <QFontMetricsF>
#include <QPainterPath>

#include <cmath>

SchematicStyle SchematicStyle::fromSettings()
{
    const AppSettings &s = AppSettings::instance();
    SchematicStyle st;
    st.background = s.color(QStringLiteral("sch.background"));
    st.wire = s.color(QStringLiteral("sch.wire"));
    st.junction = s.color(QStringLiteral("sch.junction"));
    st.component = s.color(QStringLiteral("sch.component"));
    st.componentFill = s.color(QStringLiteral("sch.componentFill"));
    st.pin = s.color(QStringLiteral("sch.pin"));
    st.text = s.color(QStringLiteral("sch.text"));
    st.directive = s.color(QStringLiteral("sch.directive"));
    st.comment = s.color(QStringLiteral("sch.comment"));
    st.flag = s.color(QStringLiteral("sch.flag"));
    st.grid = s.color(QStringLiteral("sch.grid"));
    st.selection = s.color(QStringLiteral("sch.selection"));
    st.unconnected = s.color(QStringLiteral("sch.unconnected"));
    st.drawing = s.color(QStringLiteral("sch.drawing"));
    st.highlight = QColor(255, 128, 0);
    st.showGrid = s.showGrid();
    st.markUnconnected = s.markUnconnectedPins();
    st.thick = s.thickLines();
    st.fontFamily = s.fontFamily();
    return st;
}

SchematicStyle SchematicStyle::monochrome()
{
    SchematicStyle st = fromSettings();
    st.background = Qt::white;
    st.wire = st.junction = st.component = st.pin = st.text = st.directive = st.comment = st.flag = st.drawing = Qt::black;
    st.componentFill = Qt::white;
    st.showGrid = false;
    st.markUnconnected = false;
    return st;
}

QFont SchematicPainter::font(int fontSize, const QString &family)
{
    QFont f(family.isEmpty() ? QStringLiteral("Sans Serif") : family);
    f.setPixelSize(std::max(1, int(std::lround(kTextHeight * fontScale(fontSize) * 16))));
    f.setHintingPreference(QFont::PreferNoHinting);
    f.setStyleStrategy(QFont::ForceOutline);
    return f;
}

namespace {

// Fonts are created 16x larger and drawn with a 1/16 scale so that small text
// still gets smooth outlines and correct metrics at any zoom.
constexpr double kFontOversample = 16.0;

QStringList textLines(const QString &text)
{
    return text.split('\n');
}

} // namespace

QRectF SchematicPainter::textRect(QPointF anchor, const Justify &just, int fontSize, const QString &text, const QString &family)
{
    const QFontMetricsF fm(font(fontSize, family));
    const QStringList lines = textLines(text);
    double w = 0;
    for (const QString &l : lines)
        w = std::max(w, fm.horizontalAdvance(l));
    const double lineH = fm.height();
    double h = lineH * lines.size();
    w /= kFontOversample;
    h /= kFontOversample;
    // Box in text-local coordinates (reading direction = +x, up = -y).
    double x0 = 0, y0 = 0;
    switch (just.align) {
    case Justify::Start: x0 = 0; break;
    case Justify::Middle: x0 = -w / 2; break;
    case Justify::End: x0 = -w; break;
    }
    switch (just.perp) {
    case Justify::PerpTop: y0 = 0; break;
    case Justify::PerpCenter: y0 = -h / 2; break;
    case Justify::PerpBottom: y0 = -h; break;
    }
    QRectF local(x0, y0, w, h);
    if (!just.vertical)
        return local.translated(anchor);
    // Rotate -90° (text reads bottom to top): (x, y) -> (y, -x)
    const QRectF r(QPointF(local.top(), -local.right()), QPointF(local.bottom(), -local.left()));
    return r.normalized().translated(anchor);
}

void SchematicPainter::drawText(QPainter &p, QPointF anchor, const Justify &just, int fontSize, const QString &text,
                                const QColor &color, const QString &family)
{
    if (text.isEmpty() || just.invisible)
        return;
    const QFont f = font(fontSize, family);
    const QFontMetricsF fm(f);
    const QStringList lines = textLines(text);
    double w = 0;
    for (const QString &l : lines)
        w = std::max(w, fm.horizontalAdvance(l));
    const double lineH = fm.height();
    const double h = lineH * lines.size();
    double x0 = 0, y0 = 0;
    switch (just.align) {
    case Justify::Start: x0 = 0; break;
    case Justify::Middle: x0 = -w / 2; break;
    case Justify::End: x0 = -w; break;
    }
    switch (just.perp) {
    case Justify::PerpTop: y0 = 0; break;
    case Justify::PerpCenter: y0 = -h / 2; break;
    case Justify::PerpBottom: y0 = -h; break;
    }
    p.save();
    p.translate(anchor);
    if (just.vertical)
        p.rotate(-90);
    p.scale(1.0 / kFontOversample, 1.0 / kFontOversample);
    p.setFont(f);
    p.setPen(color);
    for (int i = 0; i < lines.size(); ++i) {
        double lx = x0;
        const double lw = fm.horizontalAdvance(lines[i]);
        if (just.align == Justify::Middle)
            lx = -lw / 2;
        else if (just.align == Justify::End)
            lx = -lw;
        p.drawText(QPointF(lx, y0 + i * lineH + fm.ascent()), lines[i]);
    }
    p.restore();
}

void SchematicPainter::drawShape(QPainter &p, const Shape &s)
{
    QPen pen = p.pen();
    switch (s.style) {
    case LineStyle::Dash: pen.setStyle(Qt::DashLine); break;
    case LineStyle::Dot: pen.setStyle(Qt::DotLine); break;
    case LineStyle::DashDot: pen.setStyle(Qt::DashDotLine); break;
    case LineStyle::DashDotDot: pen.setStyle(Qt::DashDotDotLine); break;
    default: break;
    }
    p.save();
    p.setPen(pen);
    switch (s.kind) {
    case Shape::Line:
        p.drawLine(s.p[0], s.p[1]);
        break;
    case Shape::Rect:
        p.drawRect(QRectF(s.p[0], s.p[1]).normalized());
        break;
    case Shape::Circle:
        p.drawEllipse(QRectF(s.p[0], s.p[1]).normalized());
        break;
    case Shape::Arc: {
        const QRectF box = QRectF(s.p[0], s.p[1]).normalized();
        if (box.width() <= 0 || box.height() <= 0)
            break;
        const QPointF c = box.center();
        const double rx = box.width() / 2, ry = box.height() / 2;
        auto angle = [&](QPoint pt) { return std::atan2(-(pt.y() - c.y()) / ry, (pt.x() - c.x()) / rx) * 180.0 / M_PI; };
        double a0 = angle(s.p[2]);
        double a1 = angle(s.p[3]);
        double span = a1 - a0;
        while (span <= 0)
            span += 360.0;
        p.drawArc(box, int(std::lround(a0 * 16)), int(std::lround(span * 16)));
        break;
    }
    }
    p.restore();
}

void SchematicPainter::drawSymbolBody(QPainter &p, const SymbolDef &def, QPoint pos, Orient o, const QColor &color,
                                      const QColor &pinColor, bool drawPinLabels, const QString &family)
{
    QPen pen(color, p.pen().widthF());
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    for (const Shape &s : def.shapes)
        drawShape(p, s.transformed(o, pos));
    for (const SymText &t : def.texts)
        drawText(p, pos + orientApply(o, t.pos), justifyTransform(t.just, o), t.fontSize, t.text, color, family);
    if (drawPinLabels) {
        for (const SymPin &pin : def.pins) {
            if (pin.labelNone || pin.name.isEmpty())
                continue;
            // The label sits `offset` units from the pin, toward the inside of the symbol.
            QPoint dir;
            Justify j = pin.labelJust;
            if (!j.vertical) {
                if (j.perp == Justify::PerpTop) dir = QPoint(0, 1);
                else if (j.perp == Justify::PerpBottom) dir = QPoint(0, -1);
                else if (j.align == Justify::Start) dir = QPoint(1, 0);
                else if (j.align == Justify::End) dir = QPoint(-1, 0);
            } else {
                if (j.perp == Justify::PerpTop) dir = QPoint(1, 0);
                else if (j.perp == Justify::PerpBottom) dir = QPoint(-1, 0);
                else if (j.align == Justify::Start) dir = QPoint(0, -1);
                else if (j.align == Justify::End) dir = QPoint(0, 1);
            }
            const QPoint local = pin.pos + dir * pin.labelOffset;
            drawText(p, pos + orientApply(o, local), justifyTransform(j, o), 1, pin.name, pinColor, family);
        }
    }
}

QString SchematicPainter::windowText(const SymbolInstance &s, int windowId)
{
    switch (windowId) {
    case AttrId::InstName: return s.instName();
    case AttrId::Value: return s.attr(QStringLiteral("Value"));
    case AttrId::Value2: return s.attr(QStringLiteral("Value2"));
    case AttrId::SpiceModel: return s.attr(QStringLiteral("SpiceModel"));
    case AttrId::SpiceLine: return s.attr(QStringLiteral("SpiceLine"));
    case AttrId::SpiceLine2: return s.attr(QStringLiteral("SpiceLine2"));
    case AttrId::Type: return s.attr(QStringLiteral("Type"));
    case AttrId::Prefix: return s.attr(QStringLiteral("Prefix"));
    case AttrId::Description: return s.attr(QStringLiteral("Description"));
    case AttrId::ModelFile: return s.attr(QStringLiteral("ModelFile"));
    default: {
        const QString name = attrNameForId(windowId);
        return name.isEmpty() ? QString() : s.attr(name);
    }
    }
}

bool SchematicPainter::windowGeometry(const SymbolInstance &s, int windowId, QPointF *anchor, Justify *just, int *fontSize)
{
    const AttrWindow *w = s.window(windowId);
    if (!w || w->fontSize == 0 || w->just.invisible)
        return false;
    *anchor = s.pos + orientApply(s.orient, w->pos);
    *just = justifyTransform(w->just, s.orient);
    *fontSize = w->fontSize;
    return true;
}

QRectF SchematicPainter::windowRect(const SymbolInstance &s, int windowId, const QString &family)
{
    QPointF a;
    Justify j;
    int fs;
    if (!windowGeometry(s, windowId, &a, &j, &fs))
        return QRectF();
    const QString text = windowText(s, windowId);
    if (text.isEmpty())
        return QRectF();
    return textRect(a, j, fs, text, family);
}

QPoint SchematicPainter::flagDirection(const Schematic &sch, const NetFlag &f)
{
    // Find connected wire ends and pins; the label goes opposite to the connection.
    QPoint sum;
    int count = 0;
    for (const Wire &w : sch.wires) {
        if (w.a == f.pos) {
            const QPoint d = w.b - w.a;
            sum += QPoint(d.x() > 0 ? 1 : (d.x() < 0 ? -1 : 0), d.y() > 0 ? 1 : (d.y() < 0 ? -1 : 0));
            ++count;
        } else if (w.b == f.pos) {
            const QPoint d = w.a - w.b;
            sum += QPoint(d.x() > 0 ? 1 : (d.x() < 0 ? -1 : 0), d.y() > 0 ? 1 : (d.y() < 0 ? -1 : 0));
            ++count;
        } else if (pointOnSegment(f.pos, w.a, w.b)) {
            // Mid-wire label: put it above a horizontal wire, left of a vertical one.
            return w.a.y() == w.b.y() ? QPoint(0, -1) : QPoint(-1, 0);
        }
    }
    if (count == 0) {
        for (const SymbolInstance &s : sch.symbols) {
            if (!s.def)
                continue;
            for (const SymPin &pin : s.def->pins) {
                if (s.pinWorld(pin) == f.pos) {
                    // Away from the symbol body.
                    const QPoint c = s.worldBounds().center();
                    const QPoint d = f.pos - c;
                    if (std::abs(d.x()) > std::abs(d.y()))
                        return QPoint(d.x() > 0 ? 1 : -1, 0);
                    return QPoint(0, d.y() > 0 ? 1 : -1);
                }
            }
        }
        return QPoint(0, -1);
    }
    if (count == 1)
        return -sum;
    if (sum.y() == 0 && sum.x() == 0)
        return QPoint(0, -1);
    // Corner: label above if there is a horizontal connection.
    return QPoint(0, -1);
}

QRectF SchematicPainter::flagRect(const NetFlag &f, QPoint dir, const QString &family)
{
    if (f.isGround() || f.name.compare(QLatin1String("COM"), Qt::CaseInsensitive) == 0)
        return QRectF(f.pos.x() - 18, f.pos.y() - 2, 36, 28);
    Justify j;
    QPointF anchor = f.pos;
    if (dir.y() < 0) { j.align = Justify::Middle; j.perp = Justify::PerpBottom; anchor += QPointF(0, -6); }
    else if (dir.y() > 0) { j.align = Justify::Middle; j.perp = Justify::PerpTop; anchor += QPointF(0, 6); }
    else if (dir.x() < 0) { j.align = Justify::End; j.perp = Justify::PerpCenter; anchor += QPointF(-6, 0); }
    else { j.align = Justify::Start; j.perp = Justify::PerpCenter; anchor += QPointF(6, 0); }
    QRectF r = textRect(anchor, j, 2, f.name, family);
    if (!f.ioDirection.isEmpty())
        r = r.united(QRectF(f.pos.x() - 10, f.pos.y() - 10, 20, 20));
    return r.united(QRectF(f.pos.x() - 3, f.pos.y() - 3, 6, 6));
}

void SchematicPainter::drawFlag(QPainter &p, const NetFlag &f, QPoint dir, const SchematicStyle &st, bool selected)
{
    const QColor c = selected ? st.selection : st.flag;
    QPen pen(c, p.pen().widthF());
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QPointF o = f.pos;
    if (f.isGround()) {
        QPainterPath path;
        path.moveTo(o + QPointF(-16, 0));
        path.lineTo(o + QPointF(16, 0));
        path.lineTo(o + QPointF(0, 16));
        path.closeSubpath();
        p.drawPath(path);
        return;
    }
    if (f.name.compare(QLatin1String("COM"), Qt::CaseInsensitive) == 0) {
        p.drawLine(o, o + QPointF(0, 8));
        p.drawLine(o + QPointF(-16, 8), o + QPointF(16, 8));
        p.drawLine(o + QPointF(-10, 14), o + QPointF(10, 14));
        p.drawLine(o + QPointF(-4, 20), o + QPointF(4, 20));
        return;
    }
    if (!f.ioDirection.isEmpty()) {
        // I/O port glyph pointing along the label direction.
        QPolygonF poly;
        const QPointF d(dir.x(), dir.y());
        const QPointF n(-d.y(), d.x());
        const QString io = f.ioDirection.toLower();
        const double L = 18, W = 6;
        if (io == QLatin1String("in")) {
            poly << o << o + d * 6 + n * W << o + d * L + n * W << o + d * L - n * W << o + d * 6 - n * W;
        } else if (io == QLatin1String("out")) {
            poly << o + n * W << o + d * (L - 6) + n * W << o + d * L << o + d * (L - 6) - n * W << o - n * W;
        } else {
            poly << o << o + d * 6 + n * W << o + d * (L - 6) + n * W << o + d * L << o + d * (L - 6) - n * W << o + d * 6 - n * W;
        }
        p.drawPolygon(poly);
        Justify j;
        QPointF anchor = o + d * (L + 4);
        if (dir.y() < 0) { j.align = Justify::Middle; j.perp = Justify::PerpBottom; }
        else if (dir.y() > 0) { j.align = Justify::Middle; j.perp = Justify::PerpTop; }
        else if (dir.x() < 0) { j.align = Justify::End; }
        else { j.align = Justify::Start; }
        drawText(p, anchor, j, 2, f.name, c, st.fontFamily);
        return;
    }
    Justify j;
    QPointF anchor = o;
    if (dir.y() < 0) { j.align = Justify::Middle; j.perp = Justify::PerpBottom; anchor += QPointF(0, -6); }
    else if (dir.y() > 0) { j.align = Justify::Middle; j.perp = Justify::PerpTop; anchor += QPointF(0, 6); }
    else if (dir.x() < 0) { j.align = Justify::End; j.perp = Justify::PerpCenter; anchor += QPointF(-6, 0); }
    else { j.align = Justify::Start; j.perp = Justify::PerpCenter; anchor += QPointF(6, 0); }
    // Small open square marks the label's connection point.
    p.drawRect(QRectF(o.x() - 3, o.y() - 3, 6, 6));
    drawText(p, anchor, j, 2, f.name, c, st.fontFamily);
}

void SchematicPainter::drawGrid(QPainter &p, const QRectF &visible, double scale, const QColor &color)
{
    double step = 16;
    while (step * scale < 6)
        step *= 2;
    const double x0 = std::floor(visible.left() / step) * step;
    const double y0 = std::floor(visible.top() / step) * step;
    QPen pen(color);
    pen.setWidthF(std::max(1.0 / scale, 1.5 / scale));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    QVector<QPointF> pts;
    for (double y = y0; y <= visible.bottom(); y += step)
        for (double x = x0; x <= visible.right(); x += step)
            pts << QPointF(x, y);
    p.drawPoints(pts.constData(), int(pts.size()));
}

void SchematicPainter::paint(QPainter &p, const Schematic &sch, const SchematicStyle &st, const QRectF &visible, const Options &opt)
{
    const double scale = p.worldTransform().m11() != 0 ? std::fabs(p.worldTransform().m11()) : 1.0;
    const double lineW = (st.thick ? 2.5 : 1.0) / scale;
    const QRectF vis = visible.adjusted(-64, -64, 64, 64);

    if (st.showGrid)
        drawGrid(p, visible, scale, st.grid);

    auto isSel = [&](SchItem::Type t, int i) { return opt.selected.contains(SchItem{t, i, -1}); };

    // Graphic drawings
    for (int i = 0; i < sch.shapes.size(); ++i) {
        const Shape &s = sch.shapes[i];
        if (!s.bounds().normalized().adjusted(-2, -2, 2, 2).intersects(vis.toAlignedRect()))
            continue;
        p.setPen(QPen(isSel(SchItem::Shape, i) ? st.selection : st.drawing, lineW));
        p.setBrush(Qt::NoBrush);
        drawShape(p, s);
    }

    // Wires
    for (int i = 0; i < sch.wires.size(); ++i) {
        const Wire &w = sch.wires[i];
        QColor c = st.wire;
        if (isSel(SchItem::Wire, i))
            c = st.selection;
        else if (!opt.highlightNets.isEmpty() && i < opt.wireNets.size() && opt.highlightNets.contains(opt.wireNets[i].toLower()))
            c = st.highlight;
        p.setPen(QPen(c, lineW * (c == st.highlight ? 3 : 1), Qt::SolidLine, Qt::RoundCap));
        p.drawLine(w.a, w.b);
    }
    for (const BusTap &b : sch.busTaps) {
        p.setPen(QPen(st.wire, lineW * 2));
        p.drawLine(b.a, b.b);
    }

    // Symbols
    for (int i = 0; i < sch.symbols.size(); ++i) {
        const SymbolInstance &s = sch.symbols[i];
        const QRectF wb = QRectF(s.worldBounds()).adjusted(-80, -80, 80, 80);
        if (!wb.intersects(vis))
            continue;
        const bool sel = isSel(SchItem::Symbol, i);
        const QColor c = sel ? st.selection : st.component;
        p.setPen(QPen(c, lineW));
        if (s.def) {
            drawSymbolBody(p, *s.def, s.pos, s.orient, c, st.pin, s.def->type == SymbolDef::Type::Block || !s.def->pins.isEmpty(),
                           st.fontFamily);
            // Attribute windows: the symbol's default windows plus instance overrides.
            QSet<int> ids;
            for (const AttrWindow &w : s.def->windows)
                ids.insert(w.id);
            for (const AttrWindow &w : s.windows)
                ids.insert(w.id);
            for (int id : ids) {
                QPointF a;
                Justify j;
                int fs;
                if (!windowGeometry(s, id, &a, &j, &fs))
                    continue;
                const bool attrSel = opt.selected.contains(SchItem{SchItem::SymbolAttr, i, id});
                drawText(p, a, j, fs, windowText(s, id), sel || attrSel ? st.selection : st.text, st.fontFamily);
                if (st.showAnchors) {
                    p.setPen(QPen(st.unconnected, lineW));
                    p.drawEllipse(a, 2, 2);
                }
            }
        } else {
            // Unknown symbol: draw a placeholder box with its name.
            p.setPen(QPen(st.unconnected, lineW, Qt::DashLine));
            p.drawRect(QRectF(s.pos.x() - 16, s.pos.y() - 16, 64, 64));
            drawText(p, QPointF(s.pos.x() + 16, s.pos.y() + 16), Justify{Justify::Middle, Justify::PerpCenter, false, false}, 1,
                     s.symbolName, st.unconnected, st.fontFamily);
        }
        if (st.showAnchors) {
            p.setPen(QPen(st.unconnected, lineW));
            p.drawLine(QPointF(s.pos) + QPointF(-3, 0), QPointF(s.pos) + QPointF(3, 0));
            p.drawLine(QPointF(s.pos) + QPointF(0, -3), QPointF(s.pos) + QPointF(0, 3));
        }
    }

    // Flags
    for (int i = 0; i < sch.flags.size(); ++i) {
        const NetFlag &f = sch.flags[i];
        if (!vis.contains(f.pos))
            continue;
        p.setPen(QPen(st.flag, lineW));
        drawFlag(p, f, flagDirection(sch, f), st, isSel(SchItem::Flag, i));
    }
    for (int i = 0; i < sch.dataFlags.size(); ++i) {
        const DataFlag &d = sch.dataFlags[i];
        p.setPen(QPen(isSel(SchItem::DataFlag, i) ? st.selection : st.flag, lineW));
        p.drawEllipse(QPointF(d.pos), 3, 3);
        const QString label = opt.opLabels.value(Connectivity::key(d.pos), d.expr);
        drawText(p, QPointF(d.pos) + QPointF(0, -6), Justify{Justify::Middle, Justify::PerpBottom, false, false}, 1, label,
                 isSel(SchItem::DataFlag, i) ? st.selection : st.text, st.fontFamily);
    }

    // Text and directives
    for (int i = 0; i < sch.texts.size(); ++i) {
        const SchText &t = sch.texts[i];
        const QString shown = t.directive ? t.text : t.text;
        const QRectF r = textRect(t.pos, t.just, t.fontSize, shown, st.fontFamily);
        if (!r.intersects(vis))
            continue;
        QColor c = t.directive ? st.directive : st.comment;
        if (isSel(SchItem::Text, i))
            c = st.selection;
        drawText(p, t.pos, t.just, t.fontSize, shown, c, st.fontFamily);
    }

    // Junction dots
    p.setPen(Qt::NoPen);
    p.setBrush(st.junction);
    for (const QPoint &j : sch.junctions())
        if (vis.contains(j))
            p.drawEllipse(QPointF(j), 4.0, 4.0);

    // Unconnected pins
    if (st.markUnconnected) {
        p.setPen(QPen(st.unconnected, lineW));
        p.setBrush(Qt::NoBrush);
        for (const QPoint &u : sch.unconnectedPins())
            if (vis.contains(u))
                p.drawRect(QRectF(u.x() - 3, u.y() - 3, 6, 6));
    }

    // Operating point annotations
    if (!opt.opLabels.isEmpty()) {
        for (auto it = opt.opLabels.constBegin(); it != opt.opLabels.constEnd(); ++it) {
            const QPoint pt(int(it.key() >> 32), int(qint32(it.key() & 0xffffffff)));
            bool isDataFlag = false;
            for (const DataFlag &d : sch.dataFlags)
                if (d.pos == pt)
                    isDataFlag = true;
            if (isDataFlag || !vis.contains(pt))
                continue;
            drawText(p, QPointF(pt) + QPointF(4, -4), Justify{Justify::Start, Justify::PerpBottom, false, false}, 1, it.value(),
                     st.highlight, st.fontFamily);
        }
    }

    if (opt.drawSheetBorder) {
        p.setPen(QPen(st.grid, lineW, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(0, 0, sch.sheetSize.width(), sch.sheetSize.height()));
    }
}
