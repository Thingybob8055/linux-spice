#include "SchematicView.h"

#include "core/AppSettings.h"
#include "core/SymbolLibrary.h"
#include "core/Units.h"
#include "dialogs/AttributeDialog.h"
#include "dialogs/NetLabelDialog.h"
#include "dialogs/PassiveDialog.h"
#include "dialogs/SourceDialog.h"
#include "dialogs/TextDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPrinter>
#include <QSvgGenerator>
#include <QWheelEvent>

#include <cmath>

namespace {

const char *kClipMime = "application/x-linspice-asc";

QCursor probeCursor(const QString &kind)
{
    // Simple painted cursors: voltage probe (red tip), current clamp, power (thermometer-like).
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    if (kind == QLatin1String("V")) {
        p.setPen(QPen(Qt::black, 2));
        p.drawLine(QPointF(2, 30), QPointF(12, 20));
        p.setPen(QPen(QColor(220, 0, 0), 4));
        p.drawLine(QPointF(12, 20), QPointF(26, 6));
        return QCursor(pm, 2, 30);
    }
    if (kind == QLatin1String("I")) {
        p.setPen(QPen(QColor(220, 0, 0), 3));
        p.drawArc(QRectF(4, 4, 24, 24), 30 * 16, 300 * 16);
        p.setPen(QPen(Qt::black, 2));
        p.drawLine(QPointF(16, 16), QPointF(28, 28));
        return QCursor(pm, 16, 16);
    }
    if (kind == QLatin1String("P")) {
        p.setPen(QPen(Qt::black, 2));
        p.drawRoundedRect(QRectF(12, 2, 8, 22), 4, 4);
        p.setBrush(QColor(220, 0, 0));
        p.drawEllipse(QPointF(16, 26), 5, 5);
        return QCursor(pm, 16, 30);
    }
    return QCursor(Qt::ArrowCursor);
}

QPoint rotatePoint(QPoint p, QPoint c, bool mirror)
{
    const QPoint d = p - c;
    if (mirror)
        return c + QPoint(-d.x(), d.y());
    return c + QPoint(-d.y(), d.x()); // 90° clockwise on screen (y down)
}

} // namespace

SchematicView::SchematicView(QWidget *parent)
    : DocumentWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_style = SchematicStyle::fromSettings();
    m_sch = emptySchematic();
}

Schematic SchematicView::emptySchematic()
{
    Schematic s;
    s.versionLine = QStringLiteral("Version 4.1");
    s.sheetSize = QSize(880, 680);
    return s;
}

void SchematicView::reloadStyle()
{
    const bool grid = m_style.showGrid;
    m_style = SchematicStyle::fromSettings();
    m_style.showGrid = grid;
    update();
}

// ---------------------------------------------------------------- file

bool SchematicView::load(const QString &path, QString *error)
{
    Schematic s;
    if (!s.load(path, error))
        return false;
    m_sch = s;
    m_undo.clear();
    m_redo.clear();
    m_selection.clear();
    invalidateConnectivity();
    setModified(false);
    emit titleChanged();
    QMetaObject::invokeMethod(this, [this]() { zoomFit(); }, Qt::QueuedConnection);
    return true;
}

void SchematicView::setSchematic(const Schematic &sch)
{
    m_sch = sch;
    m_sch.resolveSymbols();
    invalidateConnectivity();
    update();
}

bool SchematicView::save()
{
    if (m_sch.filePath.isEmpty())
        return false;
    return saveAs(m_sch.filePath);
}

bool SchematicView::saveAs(const QString &path)
{
    QString err;
    if (!m_sch.save(path, &err)) {
        QMessageBox::warning(this, tr("Save failed"), tr("Could not save %1:\n%2").arg(path, err));
        return false;
    }
    setModified(false);
    emit titleChanged();
    return true;
}

void SchematicView::commitExternalChange()
{
    afterEdit();
}

NetlistResult SchematicView::netlist() const
{
    Netlister n(m_sch);
    return n.build();
}

const Connectivity &SchematicView::connectivity() const
{
    if (!m_connValid) {
        m_conn = Netlister::connectivity(m_sch);
        m_connValid = true;
    }
    return m_conn;
}

void SchematicView::invalidateConnectivity()
{
    m_connValid = false;
}

void SchematicView::setModified(bool m)
{
    if (m_modified == m)
        return;
    m_modified = m;
    emit modifiedChanged(m);
}

void SchematicView::pushUndo()
{
    m_undo << m_sch.serialize();
    while (m_undo.size() > AppSettings::instance().undoDepth())
        m_undo.removeFirst();
    m_redo.clear();
}

void SchematicView::afterEdit()
{
    m_sch.normalizeWires();
    invalidateConnectivity();
    setModified(true);
    update();
}

void SchematicView::undo()
{
    if (m_moving) {
        cancelMove();
        return;
    }
    if (m_undo.isEmpty())
        return;
    m_redo << m_sch.serialize();
    const QString path = m_sch.filePath;
    const auto enc = m_sch.encoding;
    m_sch.parse(m_undo.takeLast());
    m_sch.filePath = path;
    m_sch.encoding = enc;
    m_sch.resolveSymbols();
    m_selection.clear();
    invalidateConnectivity();
    setModified(true);
    update();
}

void SchematicView::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo << m_sch.serialize();
    const QString path = m_sch.filePath;
    const auto enc = m_sch.encoding;
    m_sch.parse(m_redo.takeLast());
    m_sch.filePath = path;
    m_sch.encoding = enc;
    m_sch.resolveSymbols();
    m_selection.clear();
    invalidateConnectivity();
    setModified(true);
    update();
}

// ---------------------------------------------------------------- coordinates

QTransform SchematicView::worldTransform() const
{
    QTransform t;
    t.scale(m_scale, m_scale);
    t.translate(-m_origin.x(), -m_origin.y());
    return t;
}

QPointF SchematicView::toWorld(QPointF w) const
{
    return QPointF(w.x() / m_scale + m_origin.x(), w.y() / m_scale + m_origin.y());
}

QPointF SchematicView::toWidget(QPointF p) const
{
    return QPointF((p.x() - m_origin.x()) * m_scale, (p.y() - m_origin.y()) * m_scale);
}

QPoint SchematicView::snap(QPointF w) const
{
    const int g = 16;
    return QPoint(int(std::lround(w.x() / g)) * g, int(std::lround(w.y() / g)) * g);
}

void SchematicView::zoomIn()
{
    m_zoomHistory << qMakePair(m_scale, m_origin);
    const QPointF c = toWorld(QPointF(width() / 2.0, height() / 2.0));
    m_scale = std::min(m_scale * 1.5, 40.0);
    m_origin = c - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

void SchematicView::zoomOut()
{
    m_zoomHistory << qMakePair(m_scale, m_origin);
    const QPointF c = toWorld(QPointF(width() / 2.0, height() / 2.0));
    m_scale = std::max(m_scale / 1.5, 0.02);
    m_origin = c - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

void SchematicView::zoomBack()
{
    if (m_zoomHistory.isEmpty()) {
        zoomOut();
        return;
    }
    const auto z = m_zoomHistory.takeLast();
    m_scale = z.first;
    m_origin = z.second;
    update();
}

void SchematicView::zoomFit()
{
    QRectF b = m_sch.bounds();
    if (b.isNull() || b.width() < 1)
        b = QRectF(0, 0, std::max(m_sch.sheetSize.width(), 400), std::max(m_sch.sheetSize.height(), 300));
    b.adjust(-48, -48, 48, 48);
    if (width() < 10 || height() < 10)
        return;
    m_zoomHistory << qMakePair(m_scale, m_origin);
    m_scale = std::clamp(std::min(width() / b.width(), height() / b.height()), 0.02, 8.0);
    m_origin = b.center() - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

void SchematicView::ensureVisibleWorld(QPointF world)
{
    const QPointF w = toWidget(world);
    if (rect().adjusted(20, 20, -20, -20).contains(w.toPoint()))
        return;
    m_origin = world - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

void SchematicView::resizeEvent(QResizeEvent *)
{
    update();
}

// ---------------------------------------------------------------- painting

void SchematicView::paintEvent(QPaintEvent *ev)
{
    QPainter p(this);
    p.fillRect(rect(), m_style.background);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setTransform(worldTransform());
    const QRectF visible = worldTransform().inverted().mapRect(QRectF(ev->rect()));

    SchematicPainter::Options opt;
    opt.selected = m_selection;
    if (m_moving)
        opt.selected = expandWithAttrs(m_moveItems);
    opt.highlightNets = m_highlightNets;
    if (!m_highlightNets.isEmpty())
        opt.wireNets = connectivity().wireNet;
    if (m_showOpLabels && !m_op.isEmpty()) {
        const Connectivity &c = connectivity();
        QSet<QString> done;
        for (int i = 0; i < m_sch.wires.size() && i < c.wireNet.size(); ++i) {
            const QString net = c.wireNet[i];
            if (done.contains(net) || net == QLatin1String("0"))
                continue;
            auto it = m_op.constFind(QStringLiteral("V(%1)").arg(m_instancePath.isEmpty() ? net : m_instancePath + ':' + net));
            if (it == m_op.constEnd()) {
                for (auto j = m_op.constBegin(); j != m_op.constEnd(); ++j)
                    if (j.key().compare(QStringLiteral("V(%1)").arg(net), Qt::CaseInsensitive) == 0)
                        it = j;
            }
            if (it == m_op.constEnd())
                continue;
            done.insert(net);
            const Wire &w = m_sch.wires[i];
            opt.opLabels.insert(Connectivity::key((w.a + w.b) / 2), units::formatEng(it.value(), QStringLiteral("V")));
        }
        for (const DataFlag &d : m_sch.dataFlags) {
            const QString net = c.netAt(d.pos);
            auto it = m_op.constFind(QStringLiteral("V(%1)").arg(net));
            if (it != m_op.constEnd())
                opt.opLabels.insert(Connectivity::key(d.pos), units::formatEng(it.value(), QStringLiteral("V")));
        }
    }
    SchematicPainter::paint(p, m_sch, m_style, visible, opt);

    const double lw = 1.0 / m_scale;
    // Floating items (placement previews)
    if (m_mode == Mode::Place && m_placeSym.def) {
        QPen pen(m_style.selection, lw);
        p.setPen(pen);
        SchematicPainter::drawSymbolBody(p, *m_placeSym.def, m_placeSym.pos, m_placeSym.orient, m_style.selection, m_style.pin,
                                         true, m_style.fontFamily);
    }
    if (m_mode == Mode::PlaceFlag) {
        p.setPen(QPen(m_style.selection, lw));
        SchematicPainter::drawFlag(p, m_placeFlag, QPoint(0, -1), m_style, true);
    }
    if (m_mode == Mode::PlaceText) {
        SchematicPainter::drawText(p, m_placeText.pos, m_placeText.just, m_placeText.fontSize, m_placeText.text, m_style.selection,
                                   m_style.fontFamily);
    }
    if (!m_floatingPaste.isEmpty()) {
        p.save();
        SchematicStyle fs = m_style;
        fs.showGrid = false;
        fs.markUnconnected = false;
        fs.wire = fs.component = fs.text = fs.directive = fs.comment = fs.flag = fs.drawing = fs.junction = m_style.selection;
        SchematicPainter::Options none;
        SchematicPainter::paint(p, m_floatingSch, fs, visible, none);
        p.restore();
    }
    if (m_wireActive) {
        p.setPen(QPen(m_style.selection, lw * 1.5, Qt::DashLine));
        const QPoint end = snap(m_mouseWorld);
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            p.drawLine(m_wireStart, end);
        } else {
            const QPoint corner = m_wireHorizontalFirst ? QPoint(end.x(), m_wireStart.y()) : QPoint(m_wireStart.x(), end.y());
            p.drawLine(m_wireStart, corner);
            p.drawLine(corner, end);
        }
    }
    if (m_shapeActive) {
        p.setPen(QPen(m_style.selection, lw, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        Shape s = m_shape;
        SchematicPainter::drawShape(p, s);
    }
    if (m_rubber) {
        p.setPen(QPen(m_style.selection, lw, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(toWorld(m_pressWidget), toWorld(m_lastWidget)).normalized());
    }
    // Cross-hair in wire mode
    if (m_mode == Mode::Wire && underMouse()) {
        const QPoint s = snap(m_mouseWorld);
        p.setPen(QPen(m_style.grid, lw, Qt::DotLine));
        p.drawLine(QPointF(visible.left(), s.y()), QPointF(visible.right(), s.y()));
        p.drawLine(QPointF(s.x(), visible.top()), QPointF(s.x(), visible.bottom()));
    }
}

// ---------------------------------------------------------------- hit testing

QRectF SchematicView::itemRect(const SchItem &it) const
{
    switch (it.type) {
    case SchItem::Wire: {
        const Wire &w = m_sch.wires[it.index];
        return QRectF(w.a, w.b).normalized().adjusted(-2, -2, 2, 2);
    }
    case SchItem::Symbol:
        return QRectF(m_sch.symbols[it.index].worldBounds());
    case SchItem::SymbolAttr:
        return SchematicPainter::windowRect(m_sch.symbols[it.index], it.sub, m_style.fontFamily);
    case SchItem::Flag: {
        const NetFlag &f = m_sch.flags[it.index];
        return SchematicPainter::flagRect(f, SchematicPainter::flagDirection(m_sch, f), m_style.fontFamily);
    }
    case SchItem::Text: {
        const SchText &t = m_sch.texts[it.index];
        return SchematicPainter::textRect(t.pos, t.just, t.fontSize, t.text, m_style.fontFamily);
    }
    case SchItem::Shape:
        return QRectF(m_sch.shapes[it.index].bounds().normalized()).adjusted(-2, -2, 2, 2);
    case SchItem::DataFlag: {
        const QPoint p = m_sch.dataFlags[it.index].pos;
        return QRectF(p.x() - 6, p.y() - 20, 12, 26);
    }
    case SchItem::BusTap: {
        const BusTap &b = m_sch.busTaps[it.index];
        return QRectF(b.a, b.b).normalized().adjusted(-2, -2, 2, 2);
    }
    default:
        return QRectF();
    }
}

int SchematicView::wireAt(QPointF w) const
{
    const double tol = hitTolerance();
    int best = -1;
    double bestD = tol;
    for (int i = 0; i < m_sch.wires.size(); ++i) {
        const double d = distToSegment(w, m_sch.wires[i].a, m_sch.wires[i].b);
        if (d <= bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

SchItem SchematicView::itemAt(QPointF w, bool includeAttrs) const
{
    const double tol = hitTolerance();
    // Attribute texts first (they sit on top), then flags, texts, wires, symbols, shapes.
    if (includeAttrs) {
        for (int i = m_sch.symbols.size() - 1; i >= 0; --i) {
            const SymbolInstance &s = m_sch.symbols[i];
            if (!s.def)
                continue;
            QSet<int> ids;
            for (const AttrWindow &aw : s.def->windows)
                ids.insert(aw.id);
            for (const AttrWindow &aw : s.windows)
                ids.insert(aw.id);
            for (int id : ids) {
                const QRectF r = SchematicPainter::windowRect(s, id, m_style.fontFamily);
                if (r.adjusted(-1, -1, 1, 1).contains(w))
                    return SchItem{SchItem::SymbolAttr, i, id};
            }
        }
    }
    for (int i = m_sch.flags.size() - 1; i >= 0; --i)
        if (itemRect(SchItem{SchItem::Flag, i}).adjusted(-tol, -tol, tol, tol).contains(w))
            return SchItem{SchItem::Flag, i};
    for (int i = m_sch.dataFlags.size() - 1; i >= 0; --i)
        if (itemRect(SchItem{SchItem::DataFlag, i}).contains(w))
            return SchItem{SchItem::DataFlag, i};
    for (int i = m_sch.texts.size() - 1; i >= 0; --i)
        if (itemRect(SchItem{SchItem::Text, i}).adjusted(-2, -2, 2, 2).contains(w))
            return SchItem{SchItem::Text, i};
    const int wi = wireAt(w);
    // Prefer symbols when the click is inside the body and not exactly on a wire.
    for (int i = m_sch.symbols.size() - 1; i >= 0; --i) {
        const SymbolInstance &s = m_sch.symbols[i];
        QRectF r = QRectF(s.worldBounds());
        if (r.width() < 8 || r.height() < 8)
            r.adjust(-4, -4, 4, 4);
        if (r.contains(w)) {
            if (wi >= 0 && distToSegment(w, m_sch.wires[wi].a, m_sch.wires[wi].b) < tol / 2)
                return SchItem{SchItem::Wire, wi};
            return SchItem{SchItem::Symbol, i};
        }
    }
    if (wi >= 0)
        return SchItem{SchItem::Wire, wi};
    for (int i = m_sch.busTaps.size() - 1; i >= 0; --i)
        if (distToSegment(w, m_sch.busTaps[i].a, m_sch.busTaps[i].b) < tol)
            return SchItem{SchItem::BusTap, i};
    for (int i = m_sch.shapes.size() - 1; i >= 0; --i) {
        const Shape &s = m_sch.shapes[i];
        if (s.kind == Shape::Line) {
            if (distToSegment(w, s.p[0], s.p[1]) < tol)
                return SchItem{SchItem::Shape, i};
        } else {
            const QRectF r = QRectF(s.p[0], s.p[1]).normalized();
            // Hit on the outline only, so shapes do not block clicks inside them.
            if (r.adjusted(-tol, -tol, tol, tol).contains(w) && !r.adjusted(tol, tol, -tol, -tol).contains(w))
                return SchItem{SchItem::Shape, i};
        }
    }
    return SchItem();
}

QList<SchItem> SchematicView::itemsIn(const QRectF &r) const
{
    QList<SchItem> out;
    for (int i = 0; i < m_sch.wires.size(); ++i)
        if (r.contains(m_sch.wires[i].a) || r.contains(m_sch.wires[i].b))
            out << SchItem{SchItem::Wire, i};
    for (int i = 0; i < m_sch.symbols.size(); ++i)
        if (r.contains(m_sch.symbols[i].pos) || r.contains(QRectF(m_sch.symbols[i].worldBounds())))
            out << SchItem{SchItem::Symbol, i};
    for (int i = 0; i < m_sch.flags.size(); ++i)
        if (r.contains(m_sch.flags[i].pos))
            out << SchItem{SchItem::Flag, i};
    for (int i = 0; i < m_sch.texts.size(); ++i)
        if (r.contains(m_sch.texts[i].pos))
            out << SchItem{SchItem::Text, i};
    for (int i = 0; i < m_sch.shapes.size(); ++i)
        if (r.contains(m_sch.shapes[i].p[0]) || r.contains(m_sch.shapes[i].p[1]))
            out << SchItem{SchItem::Shape, i};
    for (int i = 0; i < m_sch.dataFlags.size(); ++i)
        if (r.contains(m_sch.dataFlags[i].pos))
            out << SchItem{SchItem::DataFlag, i};
    for (int i = 0; i < m_sch.busTaps.size(); ++i)
        if (r.contains(m_sch.busTaps[i].a))
            out << SchItem{SchItem::BusTap, i};
    return out;
}

bool SchematicView::isConnectionPoint(QPoint p, QPoint exclude) const
{
    if (p == exclude)
        return false;
    for (const Wire &w : m_sch.wires)
        if (pointOnSegment(p, w.a, w.b))
            return true;
    for (const SymbolInstance &s : m_sch.symbols)
        for (const QPoint &q : s.pinPositions())
            if (q == p)
                return true;
    for (const NetFlag &f : m_sch.flags)
        if (f.pos == p)
            return true;
    return false;
}

bool SchematicView::isUnconnectedPin(QPoint p) const
{
    for (const QPoint &u : m_sch.unconnectedPins())
        if (u == p)
            return true;
    return false;
}

// ---------------------------------------------------------------- modes

void SchematicView::setMode(Mode m)
{
    if (m_moving)
        cancelMove();
    m_wireActive = false;
    m_shapeActive = false;
    m_shapeStage = 0;
    m_floatingPaste.clear();
    if (m != Mode::Place)
        m_placeSym = SymbolInstance();
    m_mode = m;
    switch (m) {
    case Mode::Normal: setCursor(Qt::ArrowCursor); break;
    case Mode::Wire: setCursor(Qt::CrossCursor); break;
    case Mode::Delete: setCursor(QCursor(Qt::ForbiddenCursor)); break;
    case Mode::Move:
    case Mode::Drag: setCursor(Qt::OpenHandCursor); break;
    case Mode::Duplicate: setCursor(Qt::DragCopyCursor); break;
    case Mode::ZoomArea: setCursor(Qt::CrossCursor); break;
    default: setCursor(Qt::CrossCursor); break;
    }
    emit modeChanged(m);
    update();
}

void SchematicView::startPlaceSymbol(const QString &symbolName, const AttrList &presetAttrs)
{
    SymbolPtr def = SymbolLibrary::instance().find(symbolName, m_sch.directory());
    if (!def) {
        QMessageBox::warning(this, tr("Symbol not found"), tr("Could not find symbol \"%1\".").arg(symbolName));
        return;
    }
    setMode(Mode::Place);
    m_placeSym = SymbolInstance();
    m_placeSym.symbolName = symbolName;
    m_placeSym.def = def;
    m_placeSym.attrs = presetAttrs;
    m_placeSym.pos = snap(m_mouseWorld);
    update();
}

void SchematicView::startPlaceFlag(const QString &name, const QString &ioDirection)
{
    setMode(Mode::PlaceFlag);
    m_placeFlag = NetFlag();
    m_placeFlag.name = name;
    m_placeFlag.ioDirection = ioDirection;
    m_placeFlag.pos = snap(m_mouseWorld);
    update();
}

void SchematicView::startPlaceText(const QString &text, bool directive, int fontSize, const Justify &just)
{
    setMode(Mode::PlaceText);
    m_placeText = SchText();
    m_placeText.text = text;
    m_placeText.directive = directive;
    m_placeText.fontSize = fontSize;
    m_placeText.just = just;
    m_placeText.pos = snap(m_mouseWorld);
    update();
}

void SchematicView::setSimulationAvailable(bool available, bool hasTransient)
{
    m_simAvailable = available;
    m_simTransient = hasTransient;
}

void SchematicView::setOperatingPoint(const QHash<QString, double> &op)
{
    m_op = op;
    update();
}

void SchematicView::showOperatingPointLabels(bool on)
{
    m_showOpLabels = on;
    update();
}

void SchematicView::setHighlightNets(const QSet<QString> &nets)
{
    m_highlightNets.clear();
    for (const QString &n : nets)
        m_highlightNets.insert(n.toLower());
    update();
}

void SchematicView::setGridVisible(bool on)
{
    m_style.showGrid = on;
    update();
}

void SchematicView::setMarkUnconnected(bool on)
{
    m_style.markUnconnected = on;
    update();
}

void SchematicView::setShowAnchors(bool on)
{
    m_style.showAnchors = on;
    update();
}

// ---------------------------------------------------------------- selection ops

QSet<SchItem> SchematicView::expandWithAttrs(const QSet<SchItem> &items) const
{
    QSet<SchItem> out;
    for (const SchItem &i : items) {
        if (i.type == SchItem::SymbolAttr)
            out.insert(SchItem{SchItem::Symbol, i.index});
        else
            out.insert(i);
    }
    return out;
}

void SchematicView::clearSelection()
{
    m_selection.clear();
    emit selectionChanged();
    update();
}

void SchematicView::selectAll()
{
    m_selection.clear();
    for (const SchItem &i : itemsIn(QRectF(-1e7, -1e7, 2e7, 2e7)))
        m_selection.insert(i);
    emit selectionChanged();
    update();
}

void SchematicView::deleteItems(QSet<SchItem> items)
{
    items = expandWithAttrs(items);
    if (items.isEmpty())
        return;
    pushUndo();
    auto removeIdx = [](auto &list, QList<int> idx) {
        std::sort(idx.begin(), idx.end(), std::greater<int>());
        for (int i : idx)
            if (i >= 0 && i < list.size())
                list.removeAt(i);
    };
    QList<int> wires, symbols, flags, texts, shapes, dflags, taps;
    for (const SchItem &i : items) {
        switch (i.type) {
        case SchItem::Wire: wires << i.index; break;
        case SchItem::Symbol: symbols << i.index; break;
        case SchItem::Flag: flags << i.index; break;
        case SchItem::Text: texts << i.index; break;
        case SchItem::Shape: shapes << i.index; break;
        case SchItem::DataFlag: dflags << i.index; break;
        case SchItem::BusTap: taps << i.index; break;
        default: break;
        }
    }
    removeIdx(m_sch.wires, wires);
    removeIdx(m_sch.symbols, symbols);
    removeIdx(m_sch.flags, flags);
    removeIdx(m_sch.texts, texts);
    removeIdx(m_sch.shapes, shapes);
    removeIdx(m_sch.dataFlags, dflags);
    removeIdx(m_sch.busTaps, taps);
    m_selection.clear();
    emit selectionChanged();
    afterEdit();
}

void SchematicView::deleteSelection()
{
    if (m_selection.isEmpty()) {
        setMode(Mode::Delete);
        return;
    }
    deleteItems(m_selection);
}

QString SchematicView::serializeItems(const QSet<SchItem> &itemsIn) const
{
    const QSet<SchItem> items = expandWithAttrs(itemsIn);
    Schematic part;
    for (const SchItem &i : items) {
        switch (i.type) {
        case SchItem::Wire: part.wires << m_sch.wires[i.index]; break;
        case SchItem::Symbol: part.symbols << m_sch.symbols[i.index]; break;
        case SchItem::Flag: part.flags << m_sch.flags[i.index]; break;
        case SchItem::Text: part.texts << m_sch.texts[i.index]; break;
        case SchItem::Shape: part.shapes << m_sch.shapes[i.index]; break;
        case SchItem::DataFlag: part.dataFlags << m_sch.dataFlags[i.index]; break;
        case SchItem::BusTap: part.busTaps << m_sch.busTaps[i.index]; break;
        default: break;
        }
    }
    return part.serialize();
}

void SchematicView::copySelection()
{
    if (m_selection.isEmpty()) {
        setMode(Mode::Duplicate);
        return;
    }
    const QString text = serializeItems(m_selection);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kClipMime), text.toUtf8());
    mime->setText(text);
    QApplication::clipboard()->setMimeData(mime);
    emit statusMessage(tr("Copied %n item(s)", nullptr, int(expandWithAttrs(m_selection).size())), 2000);
}

void SchematicView::cutSelection()
{
    if (m_selection.isEmpty())
        return;
    copySelection();
    deleteSelection();
}

void SchematicView::paste()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QString text;
    if (mime && mime->hasFormat(QString::fromLatin1(kClipMime)))
        text = QString::fromUtf8(mime->data(QString::fromLatin1(kClipMime)));
    else if (mime && mime->text().startsWith(QLatin1String("Version 4")))
        text = mime->text();
    if (text.isEmpty())
        return;
    pasteText(text, snap(m_mouseWorld), true);
}

void SchematicView::pasteText(const QString &text, QPoint at, bool follow)
{
    Schematic part;
    part.parse(text);
    part.filePath = m_sch.filePath;
    part.resolveSymbols();
    // Anchor = top-left-ish point of the pasted content (snapped).
    QRect b = part.bounds();
    QPoint anchor = snap(b.topLeft());
    for (Wire &w : part.wires) {
        w.a -= anchor;
        w.b -= anchor;
    }
    for (SymbolInstance &s : part.symbols)
        s.pos -= anchor;
    for (NetFlag &f : part.flags)
        f.pos -= anchor;
    for (SchText &t : part.texts)
        t.pos -= anchor;
    for (Shape &s : part.shapes)
        for (QPoint &p : s.p)
            p -= anchor;
    for (DataFlag &d : part.dataFlags)
        d.pos -= anchor;
    for (BusTap &bt : part.busTaps) {
        bt.a -= anchor;
        bt.b -= anchor;
    }
    m_floatingPaste = part.serialize();
    m_floatingAnchor = at;
    m_floatingSch = part;
    // Shift the preview to the cursor.
    for (Wire &w : m_floatingSch.wires) { w.a += at; w.b += at; }
    for (SymbolInstance &s : m_floatingSch.symbols) s.pos += at;
    for (NetFlag &f : m_floatingSch.flags) f.pos += at;
    for (SchText &t : m_floatingSch.texts) t.pos += at;
    for (Shape &s : m_floatingSch.shapes) for (QPoint &p : s.p) p += at;
    for (DataFlag &d : m_floatingSch.dataFlags) d.pos += at;
    if (!follow) {
        // Drop immediately.
        QMouseEvent fake(QEvent::MouseButtonPress, toWidget(at), mapToGlobal(toWidget(at)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        Q_UNUSED(fake);
    }
    setCursor(Qt::DragCopyCursor);
    update();
}

void SchematicView::applyRotate(const QSet<SchItem> &itemsIn, bool mirror)
{
    const QSet<SchItem> items = expandWithAttrs(itemsIn);
    if (items.isEmpty())
        return;
    QRectF bb;
    for (const SchItem &i : items)
        bb = bb.isNull() ? itemRect(i) : bb.united(itemRect(i));
    const QPoint c = snap(bb.center());
    pushUndo();
    for (const SchItem &i : items) {
        switch (i.type) {
        case SchItem::Wire: {
            Wire &w = m_sch.wires[i.index];
            w.a = rotatePoint(w.a, c, mirror);
            w.b = rotatePoint(w.b, c, mirror);
            break;
        }
        case SchItem::Symbol: {
            SymbolInstance &s = m_sch.symbols[i.index];
            s.pos = rotatePoint(s.pos, c, mirror);
            s.orient = mirror ? orientMirror(s.orient) : orientRotateCW(s.orient);
            break;
        }
        case SchItem::Flag: m_sch.flags[i.index].pos = rotatePoint(m_sch.flags[i.index].pos, c, mirror); break;
        case SchItem::Text: m_sch.texts[i.index].pos = rotatePoint(m_sch.texts[i.index].pos, c, mirror); break;
        case SchItem::DataFlag: m_sch.dataFlags[i.index].pos = rotatePoint(m_sch.dataFlags[i.index].pos, c, mirror); break;
        case SchItem::Shape: {
            Shape &s = m_sch.shapes[i.index];
            const int n = s.kind == Shape::Arc ? 4 : 2;
            for (int k = 0; k < n; ++k)
                s.p[k] = rotatePoint(s.p[k], c, mirror);
            if (s.kind == Shape::Arc && mirror)
                std::swap(s.p[2], s.p[3]);
            break;
        }
        case SchItem::BusTap: {
            BusTap &b = m_sch.busTaps[i.index];
            b.a = rotatePoint(b.a, c, mirror);
            b.b = rotatePoint(b.b, c, mirror);
            break;
        }
        default: break;
        }
    }
    afterEdit();
}

void SchematicView::rotateSelection()
{
    if (m_mode == Mode::Place) {
        m_placeSym.orient = orientRotateCW(m_placeSym.orient);
        update();
        return;
    }
    if (!m_floatingPaste.isEmpty()) {
        // Rotate the floating copy around its anchor.
        for (Wire &w : m_floatingSch.wires) { w.a = rotatePoint(w.a, m_floatingAnchor, false); w.b = rotatePoint(w.b, m_floatingAnchor, false); }
        for (SymbolInstance &s : m_floatingSch.symbols) { s.pos = rotatePoint(s.pos, m_floatingAnchor, false); s.orient = orientRotateCW(s.orient); }
        for (NetFlag &f : m_floatingSch.flags) f.pos = rotatePoint(f.pos, m_floatingAnchor, false);
        for (SchText &t : m_floatingSch.texts) t.pos = rotatePoint(t.pos, m_floatingAnchor, false);
        update();
        return;
    }
    if (m_moving) {
        finishMove();
        applyRotate(m_selection, false);
        return;
    }
    applyRotate(m_selection, false);
}

void SchematicView::mirrorSelection()
{
    if (m_mode == Mode::Place) {
        m_placeSym.orient = orientMirror(m_placeSym.orient);
        update();
        return;
    }
    if (!m_floatingPaste.isEmpty()) {
        for (Wire &w : m_floatingSch.wires) { w.a = rotatePoint(w.a, m_floatingAnchor, true); w.b = rotatePoint(w.b, m_floatingAnchor, true); }
        for (SymbolInstance &s : m_floatingSch.symbols) { s.pos = rotatePoint(s.pos, m_floatingAnchor, true); s.orient = orientMirror(s.orient); }
        for (NetFlag &f : m_floatingSch.flags) f.pos = rotatePoint(f.pos, m_floatingAnchor, true);
        for (SchText &t : m_floatingSch.texts) t.pos = rotatePoint(t.pos, m_floatingAnchor, true);
        update();
        return;
    }
    applyRotate(m_selection, true);
}

// ---------------------------------------------------------------- wiring

void SchematicView::commitWireSegment(QPoint from, QPoint to)
{
    if (from == to)
        return;
    m_sch.wires << Wire{from, to};
}

void SchematicView::cutWiresThroughComponents()
{
    if (AppSettings::instance().allowPinShorts())
        return;
    // A wire that runs across two pins of the same component would short it:
    // remove the stretch of wire between those pins (as LTspice does).
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 20) {
        changed = false;
        for (int wi = 0; wi < m_sch.wires.size() && !changed; ++wi) {
            const Wire w = m_sch.wires[wi];
            for (const SymbolInstance &s : m_sch.symbols) {
                const QList<QPoint> pins = s.pinPositions();
                QList<QPoint> on;
                for (const QPoint &p : pins)
                    if (pointOnSegment(p, w.a, w.b))
                        on << p;
                if (on.size() < 2)
                    continue;
                std::sort(on.begin(), on.end(), [&](QPoint a, QPoint b) {
                    return (a - w.a).manhattanLength() < (b - w.a).manhattanLength();
                });
                const QPoint p1 = on.first(), p2 = on.last();
                m_sch.wires.removeAt(wi);
                if (w.a != p1)
                    m_sch.wires << Wire{w.a, p1};
                if (w.b != p2)
                    m_sch.wires << Wire{p2, w.b};
                changed = true;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- placing

void SchematicView::placeCurrentSymbol(QPoint at)
{
    if (!m_placeSym.def)
        return;
    pushUndo();
    SymbolInstance s = m_placeSym;
    s.pos = at;
    QString prefix = s.attr(QStringLiteral("Prefix"));
    if (!s.attrs.contains(QStringLiteral("InstName")) || s.instName().isEmpty() || s.instName().endsWith('?')) {
        // LTspice names instances by the first letter of the prefix (R1, Q1, M1, X->U1).
        QString base = prefix.isEmpty() ? QStringLiteral("X") : prefix.left(1).toUpper();
        if (base == QLatin1String("X"))
            base = QStringLiteral("U");
        if (s.def->type == SymbolDef::Type::Block && prefix.isEmpty())
            base = QStringLiteral("X");
        s.attrs.set(QStringLiteral("InstName"), m_sch.nextInstName(base));
    }
    // Move InstName to the front like LTspice writes it.
    AttrList ordered;
    ordered.set(QStringLiteral("InstName"), s.instName());
    for (const auto &kv : s.attrs.items())
        if (kv.first.compare(QLatin1String("InstName"), Qt::CaseInsensitive) != 0)
            ordered.set(kv.first, kv.second);
    s.attrs = ordered;
    m_sch.symbols << s;
    cutWiresThroughComponents();
    afterEdit();
    m_placeSym.attrs.remove(QStringLiteral("InstName"));
}

// ---------------------------------------------------------------- moving

void SchematicView::beginMove(const QSet<SchItem> &items, QPointF worldStart, bool stretch)
{
    m_moveItems = expandWithAttrs(items);
    if (m_moveItems.isEmpty())
        return;
    m_moving = true;
    m_moveStretch = stretch;
    m_moveStart = snap(worldStart);
    m_moveDelta = QPoint();
    m_moveBackup = m_sch;
    m_stretchEnds.clear();
    if (stretch) {
        // Collect connection points of moving items; wire ends there follow the move.
        QList<QPoint> pts;
        QSet<int> movingWires;
        for (const SchItem &i : m_moveItems) {
            if (i.type == SchItem::Symbol)
                pts << m_sch.symbols[i.index].pinPositions();
            else if (i.type == SchItem::Flag)
                pts << m_sch.flags[i.index].pos;
            else if (i.type == SchItem::Wire) {
                pts << m_sch.wires[i.index].a << m_sch.wires[i.index].b;
                movingWires.insert(i.index);
            }
        }
        for (int w = 0; w < m_sch.wires.size(); ++w) {
            if (movingWires.contains(w))
                continue;
            const bool aMoves = pts.contains(m_sch.wires[w].a);
            const bool bMoves = pts.contains(m_sch.wires[w].b);
            if (aMoves && bMoves) {
                m_moveItems.insert(SchItem{SchItem::Wire, w});
                continue;
            }
            if (aMoves)
                m_stretchEnds << qMakePair(w, 0);
            if (bMoves)
                m_stretchEnds << qMakePair(w, 1);
        }
        // Flags sitting on stretched wire ends that are not part of the move stay put.
    }
    setCursor(Qt::ClosedHandCursor);
}

void SchematicView::updateMove(QPointF world)
{
    if (!m_moving)
        return;
    const QPoint delta = snap(world) - m_moveStart;
    if (delta == m_moveDelta)
        return;
    m_moveDelta = delta;
    // Rebuild from backup each time to avoid accumulating error.
    Schematic s = m_moveBackup;
    for (const SchItem &i : m_moveItems) {
        switch (i.type) {
        case SchItem::Wire: s.wires[i.index].a += delta; s.wires[i.index].b += delta; break;
        case SchItem::Symbol: s.symbols[i.index].pos += delta; break;
        case SchItem::Flag: s.flags[i.index].pos += delta; break;
        case SchItem::Text: s.texts[i.index].pos += delta; break;
        case SchItem::DataFlag: s.dataFlags[i.index].pos += delta; break;
        case SchItem::Shape: {
            Shape &sh = s.shapes[i.index];
            for (QPoint &p : sh.p)
                p += delta;
            break;
        }
        case SchItem::BusTap: s.busTaps[i.index].a += delta; s.busTaps[i.index].b += delta; break;
        default: break;
        }
    }
    QList<Wire> extra;
    const bool ortho = AppSettings::instance().orthoWires();
    for (const auto &se : m_stretchEnds) {
        Wire &w = s.wires[se.first];
        const Wire orig = m_moveBackup.wires[se.first];
        if (se.second == 0)
            w.a += delta;
        else
            w.b += delta;
        // Keep stretched wires orthogonal by inserting a corner.
        if (ortho && w.a.x() != w.b.x() && w.a.y() != w.b.y()) {
            const bool wasHorizontal = orig.a.y() == orig.b.y();
            const QPoint fixed = se.second == 0 ? w.b : w.a;
            const QPoint moved = se.second == 0 ? w.a : w.b;
            const QPoint corner = wasHorizontal ? QPoint(moved.x(), fixed.y()) : QPoint(fixed.x(), moved.y());
            if (se.second == 0) {
                w.a = corner;
                extra << Wire{moved, corner};
            } else {
                w.b = corner;
                extra << Wire{corner, moved};
            }
        }
    }
    s.wires << extra;
    m_sch = s;
    invalidateConnectivity();
    update();
}

void SchematicView::finishMove()
{
    if (!m_moving)
        return;
    m_moving = false;
    if (m_moveDelta.isNull()) {
        m_sch = m_moveBackup;
        setCursor(m_mode == Mode::Normal ? Qt::ArrowCursor : Qt::OpenHandCursor);
        update();
        return;
    }
    // Undo point is the state before the move.
    m_undo << m_moveBackup.serialize();
    m_redo.clear();
    m_selection = m_moveItems;
    cutWiresThroughComponents();
    afterEdit();
    setCursor(m_mode == Mode::Normal ? Qt::ArrowCursor : Qt::OpenHandCursor);
}

void SchematicView::cancelMove()
{
    if (!m_moving)
        return;
    m_moving = false;
    m_sch = m_moveBackup;
    invalidateConnectivity();
    update();
}

// ---------------------------------------------------------------- probing

QString SchematicView::nodeExpr(const QString &net) const
{
    if (m_instancePath.isEmpty())
        return net;
    if (net == QLatin1String("0") || net.startsWith(QLatin1String("$G_"), Qt::CaseInsensitive))
        return net;
    return m_instancePath + ':' + net;
}

QString SchematicView::elementExpr(const QString &fn, const QString &elem) const
{
    const QString e = m_instancePath.isEmpty() ? elem : m_instancePath + ':' + elem;
    return QStringLiteral("%1(%2)").arg(fn, e);
}

QString SchematicView::probeExpressionAt(QPointF world, bool alt, QString *description) const
{
    const Connectivity &c = connectivity();
    const SchItem it = itemAt(world, false);
    // Pins: current into the pin.
    for (int i = 0; i < m_sch.symbols.size(); ++i) {
        const SymbolInstance &s = m_sch.symbols[i];
        if (!s.def)
            continue;
        for (int k = 0; k < s.def->pins.size(); ++k) {
            const QPointF pw = s.pinWorld(s.def->pins[k]);
            if (QLineF(pw, world).length() < hitTolerance() * 0.8) {
                const QString name = Netlister::spiceNameFor(s);
                const QChar kind = name.isEmpty() ? QChar() : name[0].toUpper();
                QString fn = QStringLiteral("I");
                const QString pinName = s.def->pins[k].name.toUpper();
                if (kind == 'Q') fn = QStringLiteral("I") + (pinName.isEmpty() ? QStringLiteral("c") : pinName.left(1).toLower());
                else if (kind == 'M' || kind == 'J') fn = QStringLiteral("I") + (pinName.isEmpty() ? QStringLiteral("d") : pinName.left(1).toLower());
                if (description)
                    *description = tr("Current into pin %1 of %2").arg(s.def->pins[k].name, s.instName());
                if (kind == 'Q' || kind == 'M' || kind == 'J')
                    return elementExpr(fn, name);
                if (kind == 'X')
                    return elementExpr(QStringLiteral("Ix"), name + ':' + s.def->pins[k].name);
                return k == 0 ? elementExpr(QStringLiteral("I"), name) : QStringLiteral("-") + elementExpr(QStringLiteral("I"), name);
            }
        }
    }
    if (it.type == SchItem::Wire || it.type == SchItem::Flag) {
        const QString net = it.type == SchItem::Wire ? c.wireNet.value(it.index) : c.flagNet.value(it.index);
        if (net.isEmpty() || net == QLatin1String("0"))
            return QString();
        if (description)
            *description = tr("Voltage on net %1").arg(net);
        return QStringLiteral("V(%1)").arg(nodeExpr(net));
    }
    if (it.type == SchItem::Symbol) {
        const SymbolInstance &s = m_sch.symbols[it.index];
        if (!s.def || s.def->pins.isEmpty())
            return QString();
        const QString name = Netlister::spiceNameFor(s);
        const QChar kind = name.isEmpty() ? QChar() : name[0].toUpper();
        if (alt) {
            // Instantaneous power: sum of V(pin)*I(pin) over terminals.
            const QStringList nets = c.symbolPinNets.value(it.index);
            if (description)
                *description = tr("Power dissipated in %1").arg(s.instName());
            if (s.def->pins.size() == 2 && nets.size() == 2) {
                const QString v = QStringLiteral("V(%1,%2)").arg(nodeExpr(nets[0]), nodeExpr(nets[1]));
                return QStringLiteral("%1*%2").arg(v, elementExpr(QStringLiteral("I"), name));
            }
            if (kind == 'Q' && nets.size() >= 3)
                return QStringLiteral("V(%1,%3)*%4+V(%2,%3)*%5")
                    .arg(nodeExpr(nets[0]), nodeExpr(nets[1]), nodeExpr(nets[2]), elementExpr(QStringLiteral("Ic"), name),
                         elementExpr(QStringLiteral("Ib"), name));
            if ((kind == 'M' || kind == 'J') && nets.size() >= 3)
                return QStringLiteral("V(%1,%2)*%3").arg(nodeExpr(nets[0]), nodeExpr(nets[2]), elementExpr(QStringLiteral("Id"), name));
            return QString();
        }
        if (description)
            *description = tr("Current through %1").arg(s.instName());
        if (kind == 'Q')
            return elementExpr(QStringLiteral("Ic"), name);
        if (kind == 'M' || kind == 'J')
            return elementExpr(QStringLiteral("Id"), name);
        if (kind == 'X')
            return QString();
        return elementExpr(QStringLiteral("I"), name);
    }
    return QString();
}

void SchematicView::updateCursorShape(QPointF world, Qt::KeyboardModifiers mods)
{
    if (m_mode != Mode::Normal || m_moving || m_panning)
        return;
    if (m_simAvailable) {
        QString desc;
        const QString expr = probeExpressionAt(world, mods & Qt::AltModifier, &desc);
        if (!expr.isEmpty()) {
            const bool power = (mods & Qt::AltModifier) && expr.contains('*');
            setCursor(probeCursor(power ? QStringLiteral("P") : (expr.startsWith(QLatin1String("V(")) ? QStringLiteral("V") : QStringLiteral("I"))));
            QString text = desc;
            // Operating point readout.
            auto it = m_op.constFind(expr);
            if (it != m_op.constEnd())
                text += QStringLiteral("   %1 = %2").arg(expr, units::formatEng(it.value(), expr.startsWith('V') ? QStringLiteral("V") : QStringLiteral("A")));
            emit probeHover(text);
            return;
        }
        emit probeHover(QString());
    }
    const SchItem it = itemAt(world);
    if (it.type == SchItem::Symbol || it.type == SchItem::SymbolAttr || it.type == SchItem::Text || it.type == SchItem::Flag)
        setCursor(Qt::PointingHandCursor);
    else if (isUnconnectedPin(snap(world)) && QLineF(snap(world), world).length() < hitTolerance())
        setCursor(Qt::CrossCursor);
    else
        setCursor(Qt::ArrowCursor);
}

// ---------------------------------------------------------------- mouse

void SchematicView::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    const QPointF world = toWorld(e->position());
    m_mouseWorld = world;
    m_pressWidget = e->position();
    m_lastWidget = e->position();

    if (e->button() == Qt::MiddleButton) {
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() == Qt::RightButton) {
        // Right click exits / cancels the current operation (LTspice convention).
        if (m_moving) {
            cancelMove();
            return;
        }
        if (m_wireActive) {
            m_wireActive = false;
            update();
            return;
        }
        if (m_shapeActive) {
            m_shapeActive = false;
            m_shapeStage = 0;
            update();
            return;
        }
        if (!m_floatingPaste.isEmpty()) {
            m_floatingPaste.clear();
            setMode(Mode::Normal);
            return;
        }
        if (m_mode != Mode::Normal) {
            setMode(Mode::Normal);
            return;
        }
        // Right-click editing happens in contextMenuEvent.
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    m_leftDown = true;
    const QPoint sp = snap(world);

    // Floating copy (duplicate/paste) drops on click.
    if (!m_floatingPaste.isEmpty()) {
        pushUndo();
        const int w0 = m_sch.wires.size(), s0 = m_sch.symbols.size(), f0 = m_sch.flags.size(), t0 = m_sch.texts.size(),
                  sh0 = m_sch.shapes.size();
        Schematic part = m_floatingSch;
        // Re-number pasted instance names that collide.
        for (SymbolInstance &s : part.symbols) {
            bool clash = false;
            for (const SymbolInstance &o : m_sch.symbols)
                if (o.instName().compare(s.instName(), Qt::CaseInsensitive) == 0)
                    clash = true;
            if (clash) {
                QString base = s.instName();
                while (!base.isEmpty() && base.back().isDigit())
                    base.chop(1);
                s.attrs.set(QStringLiteral("InstName"), m_sch.nextInstName(base.isEmpty() ? QStringLiteral("X") : base));
            }
            m_sch.symbols << s;
        }
        m_sch.wires << part.wires;
        m_sch.flags << part.flags;
        m_sch.texts << part.texts;
        m_sch.shapes << part.shapes;
        m_sch.dataFlags << part.dataFlags;
        m_sch.busTaps << part.busTaps;
        m_selection.clear();
        for (int i = w0; i < m_sch.wires.size(); ++i) m_selection.insert(SchItem{SchItem::Wire, i});
        for (int i = s0; i < m_sch.symbols.size(); ++i) m_selection.insert(SchItem{SchItem::Symbol, i});
        for (int i = f0; i < m_sch.flags.size(); ++i) m_selection.insert(SchItem{SchItem::Flag, i});
        for (int i = t0; i < m_sch.texts.size(); ++i) m_selection.insert(SchItem{SchItem::Text, i});
        for (int i = sh0; i < m_sch.shapes.size(); ++i) m_selection.insert(SchItem{SchItem::Shape, i});
        m_floatingPaste.clear();
        m_floatingSch = Schematic();
        cutWiresThroughComponents();
        afterEdit();
        if (m_mode == Mode::Duplicate)
            setMode(Mode::Normal);
        else
            setCursor(Qt::ArrowCursor);
        m_leftDown = false;
        return;
    }

    switch (m_mode) {
    case Mode::Wire: {
        if (!m_wireActive) {
            m_wireActive = true;
            m_wireStart = sp;
            m_wireDirectionDecided = false;
            update();
            return;
        }
        const bool diag = e->modifiers() & Qt::ControlModifier;
        pushUndo();
        if (diag || sp.x() == m_wireStart.x() || sp.y() == m_wireStart.y()) {
            commitWireSegment(m_wireStart, sp);
        } else {
            const QPoint corner = m_wireHorizontalFirst ? QPoint(sp.x(), m_wireStart.y()) : QPoint(m_wireStart.x(), sp.y());
            commitWireSegment(m_wireStart, corner);
            commitWireSegment(corner, sp);
        }
        // Clicking on an existing connection point ends the wire.
        const bool endHere = isConnectionPoint(sp, m_wireStart) && sp != m_wireStart;
        cutWiresThroughComponents();
        afterEdit();
        if (endHere)
            m_wireActive = false;
        else {
            m_wireStart = sp;
            m_wireDirectionDecided = false;
        }
        return;
    }
    case Mode::Place:
        placeCurrentSymbol(sp);
        return;
    case Mode::PlaceFlag: {
        pushUndo();
        NetFlag f = m_placeFlag;
        f.pos = sp;
        // Replace an existing flag at the same point.
        for (int i = m_sch.flags.size() - 1; i >= 0; --i)
            if (m_sch.flags[i].pos == sp)
                m_sch.flags.removeAt(i);
        m_sch.flags << f;
        afterEdit();
        if (!f.isGround())
            setMode(Mode::Normal);
        return;
    }
    case Mode::PlaceText: {
        pushUndo();
        SchText t = m_placeText;
        t.pos = sp;
        m_sch.texts << t;
        afterEdit();
        setMode(Mode::Normal);
        return;
    }
    case Mode::PlaceDataFlag: {
        pushUndo();
        m_sch.dataFlags << DataFlag{sp, QString()};
        afterEdit();
        return;
    }
    case Mode::DrawLine:
    case Mode::DrawRect:
    case Mode::DrawCircle:
    case Mode::DrawArc: {
        const bool offGrid = e->modifiers() & Qt::ControlModifier;
        const QPoint pt = offGrid ? world.toPoint() : sp;
        if (!m_shapeActive) {
            m_shapeActive = true;
            m_shapeStage = 1;
            m_shape = Shape();
            m_shape.kind = m_mode == Mode::DrawLine ? Shape::Line
                         : m_mode == Mode::DrawRect ? Shape::Rect
                         : m_mode == Mode::DrawCircle ? Shape::Circle : Shape::Arc;
            m_shape.p[0] = m_shape.p[1] = m_shape.p[2] = m_shape.p[3] = pt;
            return;
        }
        if (m_shape.kind == Shape::Arc && m_shapeStage < 3) {
            ++m_shapeStage; // 2: start point, 3: end point
            return;
        }
        pushUndo();
        m_sch.shapes << m_shape;
        m_shapeActive = false;
        m_shapeStage = 0;
        afterEdit();
        return;
    }
    case Mode::Delete: {
        const SchItem it = itemAt(world);
        if (it.isValid()) {
            deleteItems({it});
            m_leftDown = false;
        } else {
            m_rubber = true;
        }
        return;
    }
    case Mode::Duplicate: {
        const SchItem it = itemAt(world);
        if (it.isValid()) {
            const QSet<SchItem> items = m_selection.contains(it) ? m_selection : QSet<SchItem>{it};
            pasteText(serializeItems(items), sp, true);
            m_leftDown = false;
        } else {
            m_rubber = true;
        }
        return;
    }
    case Mode::Move:
    case Mode::Drag: {
        const SchItem it = itemAt(world);
        if (it.isValid()) {
            const QSet<SchItem> items = m_selection.contains(it) ? m_selection : QSet<SchItem>{it};
            beginMove(items, world, m_mode == Mode::Drag);
        } else if (!m_selection.isEmpty()) {
            beginMove(m_selection, world, m_mode == Mode::Drag);
        } else {
            m_rubber = true;
        }
        return;
    }
    case Mode::ZoomArea:
        m_rubber = true;
        return;
    case Mode::Normal:
    default:
        break;
    }

    // Normal mode.
    if (m_simAvailable && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        QString desc;
        const QString expr = probeExpressionAt(world, e->modifiers() & Qt::AltModifier, &desc);
        if (!expr.isEmpty()) {
            m_pressStartedProbe = true;
            m_probeStartNet = expr.startsWith(QLatin1String("V(")) ? expr.mid(2, expr.size() - 3) : QString();
            return;
        }
    }
    // Clicking an unconnected pin starts a wire.
    if (isUnconnectedPin(sp) && QLineF(sp, world).length() < hitTolerance()) {
        setMode(Mode::Wire);
        m_wireActive = true;
        m_wireStart = sp;
        m_wireDirectionDecided = false;
        m_leftDown = false;
        return;
    }
    const SchItem it = itemAt(world);
    m_pressItem = it;
    if (it.isValid()) {
        if (e->modifiers() & Qt::ControlModifier) {
            if (m_selection.contains(it))
                m_selection.remove(it);
            else
                m_selection.insert(it);
        } else if (!m_selection.contains(it)) {
            m_selection = {it};
        }
        emit selectionChanged();
        update();
    } else {
        if (!(e->modifiers() & Qt::ControlModifier) && !m_selection.isEmpty()) {
            m_selection.clear();
            emit selectionChanged();
            update();
        }
        if (e->modifiers() & Qt::ShiftModifier)
            m_rubber = true;
        else
            m_panning = true;
    }
}

void SchematicView::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF world = toWorld(e->position());
    m_mouseWorld = world;
    const QPointF deltaW = e->position() - m_lastWidget;
    m_lastWidget = e->position();
    const QPoint sp = snap(world);
    emit statusMessage(QStringLiteral("x=%1 y=%2").arg(sp.x()).arg(sp.y()));

    if (m_panning) {
        m_origin -= deltaW / m_scale;
        update();
        return;
    }
    if (m_moving) {
        updateMove(world);
        return;
    }
    if (m_rubber) {
        update();
        return;
    }
    // Normal-mode drag of an item starts a stretch move.
    if (m_leftDown && m_mode == Mode::Normal && m_pressItem.isValid() && !m_pressStartedProbe
        && (e->position() - m_pressWidget).manhattanLength() > 4) {
        const bool stretch = !(e->modifiers() & Qt::AltModifier);
        beginMove(m_selection.isEmpty() ? QSet<SchItem>{m_pressItem} : m_selection, toWorld(m_pressWidget), stretch);
        updateMove(world);
        return;
    }
    switch (m_mode) {
    case Mode::Place:
        m_placeSym.pos = sp;
        update();
        break;
    case Mode::PlaceFlag:
        m_placeFlag.pos = sp;
        update();
        break;
    case Mode::PlaceText:
        m_placeText.pos = sp;
        update();
        break;
    case Mode::Wire:
        if (m_wireActive && !m_wireDirectionDecided && (sp - m_wireStart).manhattanLength() >= 16) {
            // The first move decides whether the wire starts horizontally or vertically.
            m_wireHorizontalFirst = std::abs(sp.x() - m_wireStart.x()) >= std::abs(sp.y() - m_wireStart.y());
            m_wireDirectionDecided = true;
        } else if (m_wireActive && sp == m_wireStart) {
            m_wireDirectionDecided = false;
        }
        update();
        break;
    case Mode::DrawLine:
    case Mode::DrawRect:
    case Mode::DrawCircle:
    case Mode::DrawArc:
        if (m_shapeActive) {
            const QPoint pt = (e->modifiers() & Qt::ControlModifier) ? world.toPoint() : sp;
            if (m_shapeStage == 1)
                m_shape.p[1] = pt;
            else if (m_shapeStage == 2)
                m_shape.p[2] = m_shape.p[3] = pt;
            else if (m_shapeStage == 3)
                m_shape.p[3] = pt;
            update();
        }
        break;
    default:
        break;
    }
    if (!m_floatingPaste.isEmpty()) {
        const QPoint d = sp - m_floatingAnchor;
        if (!d.isNull()) {
            for (Wire &w : m_floatingSch.wires) { w.a += d; w.b += d; }
            for (SymbolInstance &s : m_floatingSch.symbols) s.pos += d;
            for (NetFlag &f : m_floatingSch.flags) f.pos += d;
            for (SchText &t : m_floatingSch.texts) t.pos += d;
            for (Shape &s : m_floatingSch.shapes) for (QPoint &p : s.p) p += d;
            for (DataFlag &df : m_floatingSch.dataFlags) df.pos += d;
            for (BusTap &bt : m_floatingSch.busTaps) { bt.a += d; bt.b += d; }
            m_floatingAnchor = sp;
            update();
        }
    }
    if (m_mode == Mode::Wire || m_mode == Mode::Normal)
        update();
    if (!m_leftDown)
        updateCursorShape(world, e->modifiers());
}

void SchematicView::mouseReleaseEvent(QMouseEvent *e)
{
    const QPointF world = toWorld(e->position());
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_panning)) {
        m_panning = false;
        updateCursorShape(world, e->modifiers());
        if (e->button() == Qt::MiddleButton)
            return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    m_leftDown = false;
    if (m_pressStartedProbe) {
        m_pressStartedProbe = false;
        QString desc;
        const QString expr = probeExpressionAt(world, e->modifiers() & Qt::AltModifier, &desc);
        // Dragging from one net to another plots the differential voltage.
        if (!m_probeStartNet.isEmpty() && expr.startsWith(QLatin1String("V(")) && (e->position() - m_pressWidget).manhattanLength() > 6) {
            const QString endNet = expr.mid(2, expr.size() - 3);
            if (endNet != m_probeStartNet) {
                emit probe(QStringLiteral("V(%1,%2)").arg(m_probeStartNet, endNet), true);
                return;
            }
        }
        if (!expr.isEmpty())
            emit probe(expr, true);
        return;
    }
    if (m_moving) {
        finishMove();
        return;
    }
    if (m_rubber) {
        m_rubber = false;
        const QRectF r = QRectF(toWorld(m_pressWidget), world).normalized();
        if (m_mode == Mode::ZoomArea) {
            if (r.width() > 2 && r.height() > 2) {
                m_zoomHistory << qMakePair(m_scale, m_origin);
                m_scale = std::clamp(std::min(width() / r.width(), height() / r.height()), 0.02, 40.0);
                m_origin = r.center() - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
            } else {
                zoomIn();
            }
            setMode(Mode::Normal);
            return;
        }
        const QList<SchItem> items = itemsIn(r);
        if (m_mode == Mode::Delete) {
            deleteItems(QSet<SchItem>(items.begin(), items.end()));
            return;
        }
        m_selection = QSet<SchItem>(items.begin(), items.end());
        emit selectionChanged();
        if (m_mode == Mode::Duplicate && !m_selection.isEmpty()) {
            pasteText(serializeItems(m_selection), snap(world), true);
        }
        update();
        return;
    }
    m_pressItem = SchItem();
}

void SchematicView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QPointF world = toWorld(e->position());
    if (m_mode == Mode::Wire) {
        m_wireActive = false;
        update();
        return;
    }
    if (m_mode != Mode::Normal)
        return;
    const SchItem it = itemAt(world);
    switch (it.type) {
    case SchItem::Symbol: editSymbol(it.index, e->modifiers() & Qt::ControlModifier); break;
    case SchItem::SymbolAttr: editAttributeText(it.index, it.sub); break;
    case SchItem::Text: editText(it.index); break;
    case SchItem::Flag: editFlag(it.index); break;
    case SchItem::Shape: editShapeStyle(it.index); break;
    default: break;
    }
}

void SchematicView::wheelEvent(QWheelEvent *e)
{
    const QPointF anchorW = toWorld(e->position());
    const double steps = e->angleDelta().y() / 120.0;
    if (e->modifiers() & Qt::ShiftModifier) {
        m_origin.rx() -= steps * 64 / m_scale;
    } else if (e->modifiers() & Qt::ControlModifier) {
        m_origin.ry() -= steps * 64 / m_scale;
    } else {
        const double factor = std::pow(1.2, steps);
        m_scale = std::clamp(m_scale * factor, 0.02, 40.0);
        m_origin = anchorW - e->position() / m_scale;
    }
    update();
}

void SchematicView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        if (m_moving)
            cancelMove();
        else if (m_wireActive) {
            m_wireActive = false;
            update();
        } else if (m_mode != Mode::Normal || !m_floatingPaste.isEmpty())
            setMode(Mode::Normal);
        else
            clearSelection();
        return;
    }
    // Arrow keys pan.
    const double step = 64 / m_scale;
    switch (e->key()) {
    case Qt::Key_Left: m_origin.rx() -= step; update(); return;
    case Qt::Key_Right: m_origin.rx() += step; update(); return;
    case Qt::Key_Up: m_origin.ry() -= step; update(); return;
    case Qt::Key_Down: m_origin.ry() += step; update(); return;
    default: break;
    }
    if (e->key() == Qt::Key_Slash && m_wireActive) {
        m_wireHorizontalFirst = !m_wireHorizontalFirst;
        m_wireDirectionDecided = true;
        update();
        return;
    }
    DocumentWidget::keyPressEvent(e);
}

void SchematicView::leaveEvent(QEvent *)
{
    emit probeHover(QString());
    update();
}

// ---------------------------------------------------------------- editing dialogs

void SchematicView::editSymbol(int index, bool superExpert)
{
    if (index < 0 || index >= m_sch.symbols.size())
        return;
    SymbolInstance s = m_sch.symbols[index];
    QString ascPath;
    // Hierarchical blocks open their sub-sheet.
    if (!superExpert && Netlister::isHierarchicalBlock(s, m_sch.directory(), &ascPath, m_sch.filePath)) {
        QMenu menu(this);
        QAction *open = menu.addAction(tr("Open hierarchical sheet"));
        QAction *attrs = menu.addAction(tr("Edit instance attributes..."));
        QAction *chosen = menu.exec(QCursor::pos());
        if (chosen == open) {
            const QString inst = Netlister::spiceNameFor(s).isEmpty() ? s.instName() : s.instName();
            QString path = m_instancePath;
            const QString xname = inst.startsWith('X', Qt::CaseInsensitive) ? inst : QStringLiteral("X") + inst;
            path = path.isEmpty() ? xname.toLower() : path + ':' + xname.toLower();
            emit openSubSheet(ascPath, path);
            return;
        }
        if (chosen != attrs)
            return;
        superExpert = true;
    }
    const QString prefix = s.attr(QStringLiteral("Prefix")).toUpper();
    const QChar k = prefix.isEmpty() ? QChar() : prefix[0];
    bool accepted = false;
    if (superExpert || prefix.isEmpty()) {
        AttributeDialog dlg(s, this);
        accepted = dlg.exec() == QDialog::Accepted;
        if (accepted)
            s = dlg.result();
    } else if (k == 'R' || k == 'C' || k == 'L') {
        PassiveDialog dlg(s, this);
        accepted = dlg.exec() == QDialog::Accepted;
        if (accepted)
            s = dlg.result();
    } else if ((k == 'V' || k == 'I') && !s.attr(QStringLiteral("SpiceLine")).contains(QLatin1String("load"))) {
        SourceDialog dlg(s, this);
        accepted = dlg.exec() == QDialog::Accepted;
        if (accepted)
            s = dlg.result();
    } else if (k == 'D' || k == 'Q' || k == 'M' || k == 'J') {
        ModelSelectDialog dlg(s, this);
        accepted = dlg.exec() == QDialog::Accepted;
        if (accepted)
            s = dlg.result();
    } else {
        // Behavioral and other elements: edit the value line directly; everything else in the attribute editor.
        if (k == 'B' || k == 'E' || k == 'G' || k == 'F' || k == 'H' || k == 'S' || k == 'W' || k == 'T' || k == 'A') {
            bool ok = false;
            const QString v = QInputDialog::getMultiLineText(this, tr("Edit %1").arg(s.instName()),
                                                             tr("Value (%1):").arg(s.def ? s.def->description() : QString()),
                                                             s.attr(QStringLiteral("Value")), &ok);
            if (ok) {
                s.attrs.set(QStringLiteral("Value"), QString(v).replace('\n', ' '));
                accepted = true;
            }
        } else {
            AttributeDialog dlg(s, this);
            accepted = dlg.exec() == QDialog::Accepted;
            if (accepted)
                s = dlg.result();
        }
    }
    if (!accepted)
        return;
    pushUndo();
    m_sch.symbols[index] = s;
    afterEdit();
}

void SchematicView::editAttributeText(int symbolIndex, int windowId)
{
    SymbolInstance &s = m_sch.symbols[symbolIndex];
    const QString attrName = windowId == AttrId::InstName ? QStringLiteral("InstName") : attrNameForId(windowId);
    if (attrName.isEmpty())
        return;
    bool ok = false;
    const QString cur = s.attr(attrName);
    const QString v = QInputDialog::getText(this, tr("Edit %1").arg(attrName), tr("Enter new %1 for %2:").arg(attrName, s.instName()),
                                            QLineEdit::Normal, cur, &ok);
    if (!ok || v == cur)
        return;
    pushUndo();
    s.attrs.set(attrName, v);
    afterEdit();
}

void SchematicView::editText(int index)
{
    SchText t = m_sch.texts[index];
    // Analysis directives open the analysis editor.
    const QString low = t.text.trimmed().toLower();
    if (t.directive && (low.startsWith(QLatin1String(".tran")) || low.startsWith(QLatin1String(".ac")) || low.startsWith(QLatin1String(".dc"))
                        || low.startsWith(QLatin1String(".noise")) || low.startsWith(QLatin1String(".op"))
                        || low.startsWith(QLatin1String(".tf"))) && !t.text.contains('\n')) {
        emit requestAnalysisDialog(index);
        return;
    }
    if (!TextDialog::edit(this, t))
        return;
    pushUndo();
    m_sch.texts[index] = t;
    afterEdit();
}

void SchematicView::editFlag(int index)
{
    NetFlag f = m_sch.flags[index];
    if (!NetLabelDialog::edit(this, f.name, f.ioDirection))
        return;
    pushUndo();
    m_sch.flags[index] = f;
    afterEdit();
}

void SchematicView::editShapeStyle(int index)
{
    Shape &s = m_sch.shapes[index];
    const QStringList styles = {tr("Solid"), tr("Dash"), tr("Dot"), tr("Dash-dot"), tr("Dash-dot-dot")};
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, tr("Line style"), tr("Style:"), styles, int(s.style), false, &ok);
    if (!ok)
        return;
    pushUndo();
    s.style = LineStyle(styles.indexOf(chosen));
    s.hasStyle = s.style != LineStyle::Solid;
    afterEdit();
}

void SchematicView::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_mode != Mode::Normal)
        return;
    const QPointF world = toWorld(e->pos());
    const SchItem it = itemAt(world);
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    // LTspice: right-click on an object edits it.
    switch (it.type) {
    case SchItem::Symbol: editSymbol(it.index, ctrl); return;
    case SchItem::SymbolAttr: editAttributeText(it.index, it.sub); return;
    case SchItem::Text: editText(it.index); return;
    case SchItem::Flag: editFlag(it.index); return;
    case SchItem::Shape: editShapeStyle(it.index); return;
    default: break;
    }
    showContextMenu(e->globalPos(), world);
}

void SchematicView::showContextMenu(QPoint globalPos, QPointF world)
{
    Q_UNUSED(world);
    QMenu menu(this);
    QMenu *edit = menu.addMenu(tr("Edit"));
    edit->addAction(tr("Undo"), this, &SchematicView::undo)->setEnabled(!m_undo.isEmpty());
    edit->addAction(tr("Redo"), this, &SchematicView::redo)->setEnabled(!m_redo.isEmpty());
    edit->addSeparator();
    edit->addAction(tr("Paste"), this, &SchematicView::paste);
    edit->addAction(tr("Select All"), this, &SchematicView::selectAll);
    edit->addSeparator();
    edit->addAction(tr("Wire"), this, [this]() { setMode(Mode::Wire); });
    edit->addAction(tr("Component..."), this, [this]() { emit requestComponentDialog(); });
    edit->addAction(tr("Move"), this, [this]() { setMode(Mode::Move); });
    edit->addAction(tr("Drag"), this, [this]() { setMode(Mode::Drag); });
    edit->addAction(tr("Delete"), this, [this]() { setMode(Mode::Delete); });
    edit->addAction(tr("Duplicate"), this, [this]() { setMode(Mode::Duplicate); });
    QMenu *draw = menu.addMenu(tr("Draft"));
    draw->addAction(tr("Line"), this, [this]() { setMode(Mode::DrawLine); });
    draw->addAction(tr("Rectangle"), this, [this]() { setMode(Mode::DrawRect); });
    draw->addAction(tr("Circle"), this, [this]() { setMode(Mode::DrawCircle); });
    draw->addAction(tr("Arc"), this, [this]() { setMode(Mode::DrawArc); });
    QMenu *view = menu.addMenu(tr("View"));
    view->addAction(tr("Zoom to Fit"), this, &SchematicView::zoomFit);
    view->addAction(tr("Zoom In"), this, &SchematicView::zoomIn);
    view->addAction(tr("Zoom Out"), this, &SchematicView::zoomOut);
    QAction *grid = view->addAction(tr("Show Grid"));
    grid->setCheckable(true);
    grid->setChecked(m_style.showGrid);
    connect(grid, &QAction::toggled, this, &SchematicView::setGridVisible);
    menu.addSeparator();
    menu.addAction(tr("Run"), this, [this]() { emit requestRun(); });
    menu.addAction(tr("Edit Simulation Cmd..."), this, [this]() { emit requestAnalysisDialog(-1); });
    menu.exec(globalPos);
}

bool SchematicView::findAndSelect(const QString &name)
{
    for (int i = 0; i < m_sch.symbols.size(); ++i) {
        if (m_sch.symbols[i].instName().compare(name, Qt::CaseInsensitive) == 0) {
            m_selection = {SchItem{SchItem::Symbol, i}};
            ensureVisibleWorld(m_sch.symbols[i].worldBounds().center());
            emit selectionChanged();
            update();
            return true;
        }
    }
    const Connectivity &c = connectivity();
    QSet<SchItem> sel;
    for (int i = 0; i < c.wireNet.size(); ++i)
        if (c.wireNet[i].compare(name, Qt::CaseInsensitive) == 0)
            sel.insert(SchItem{SchItem::Wire, i});
    if (!sel.isEmpty()) {
        m_selection = sel;
        ensureVisibleWorld(m_sch.wires[sel.begin()->index].a);
        emit selectionChanged();
        update();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- output

void SchematicView::print(QPrinter *printer)
{
    QPainter p(printer);
    QRectF b = m_sch.bounds();
    if (b.isNull())
        return;
    b.adjust(-32, -32, 32, 32);
    const QRectF page = printer->pageRect(QPrinter::DevicePixel);
    const double s = std::min(page.width() / b.width(), page.height() / b.height());
    p.translate(page.width() / 2, page.height() / 2);
    p.scale(s, s);
    p.translate(-b.center());
    p.setRenderHint(QPainter::Antialiasing);
    SchematicStyle st = AppSettings::instance().raw().value(QStringLiteral("print/monochrome"), true).toBool() ? SchematicStyle::monochrome()
                                                                                                             : m_style;
    st.showGrid = false;
    st.markUnconnected = false;
    SchematicPainter::paint(p, m_sch, st, b, {});
}

void SchematicView::copyImageToClipboard()
{
    QRectF b = m_sch.bounds().adjusted(-32, -32, 32, 32);
    const double s = 2.0;
    QImage img((b.size() * s).toSize(), QImage::Format_ARGB32);
    img.fill(m_style.background);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(s, s);
    p.translate(-b.topLeft());
    SchematicStyle st = m_style;
    st.showGrid = false;
    SchematicPainter::paint(p, m_sch, st, b, {});
    p.end();
    QApplication::clipboard()->setImage(img);
    emit statusMessage(tr("Schematic image copied to clipboard"), 2000);
}

bool SchematicView::exportImage(const QString &path)
{
    QRectF b = m_sch.bounds().adjusted(-32, -32, 32, 32);
    SchematicStyle st = m_style;
    st.showGrid = false;
    st.markUnconnected = false;
    if (path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QSvgGenerator gen;
        gen.setFileName(path);
        gen.setSize(b.size().toSize());
        gen.setViewBox(QRectF(QPointF(0, 0), b.size()));
        gen.setTitle(QFileInfo(m_sch.filePath).fileName());
        QPainter p(&gen);
        p.translate(-b.topLeft());
        SchematicPainter::paint(p, m_sch, st, b, {});
        return true;
    }
    const double s = 3.0;
    QImage img((b.size() * s).toSize(), QImage::Format_ARGB32);
    img.fill(st.background);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(s, s);
    p.translate(-b.topLeft());
    SchematicPainter::paint(p, m_sch, st, b, {});
    p.end();
    return img.save(path);
}
