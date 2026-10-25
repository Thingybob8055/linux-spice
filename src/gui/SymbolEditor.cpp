#include "SymbolEditor.h"

#include "core/AppSettings.h"
#include "core/TextIO.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

SymbolEditor::SymbolEditor(QWidget *parent)
    : DocumentWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_sym.typeText = QStringLiteral("CELL");
}

bool SymbolEditor::load(const QString &path, QString *error)
{
    SymbolPtr s = SymbolDef::load(path, error);
    if (!s)
        return false;
    m_sym = *s;
    m_path = QFileInfo(path).absoluteFilePath();
    m_undo.clear();
    m_redo.clear();
    m_modified = false;
    emit titleChanged();
    QMetaObject::invokeMethod(this, [this]() { zoomFit(); }, Qt::QueuedConnection);
    return true;
}

void SymbolEditor::setSymbol(const SymbolDef &def)
{
    m_sym = def;
    m_modified = true;
    emit modifiedChanged(true);
    QMetaObject::invokeMethod(this, [this]() { zoomFit(); }, Qt::QueuedConnection);
    update();
}

bool SymbolEditor::save()
{
    if (m_path.isEmpty())
        return false;
    return saveAs(m_path);
}

bool SymbolEditor::saveAs(const QString &path)
{
    QString err;
    if (!m_sym.save(path, &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
        return false;
    }
    m_path = QFileInfo(path).absoluteFilePath();
    m_modified = false;
    emit modifiedChanged(false);
    emit titleChanged();
    return true;
}

void SymbolEditor::pushUndo()
{
    m_undo << m_sym.serialize();
    m_redo.clear();
}

void SymbolEditor::changed()
{
    if (!m_modified) {
        m_modified = true;
        emit modifiedChanged(true);
    }
    update();
}

void SymbolEditor::undo()
{
    if (m_undo.isEmpty())
        return;
    m_redo << m_sym.serialize();
    const QString path = m_sym.filePath;
    m_sym = *SymbolDef::parse(m_undo.takeLast());
    m_sym.filePath = path;
    m_selection.clear();
    changed();
}

void SymbolEditor::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo << m_sym.serialize();
    m_sym = *SymbolDef::parse(m_redo.takeLast());
    m_selection.clear();
    changed();
}

QTransform SymbolEditor::transform() const
{
    QTransform t;
    t.scale(m_scale, m_scale);
    t.translate(-m_origin.x(), -m_origin.y());
    return t;
}

QPointF SymbolEditor::toWorld(QPointF w) const
{
    return QPointF(w.x() / m_scale + m_origin.x(), w.y() / m_scale + m_origin.y());
}

QPoint SymbolEditor::snap(QPointF w, bool fine) const
{
    const int g = fine ? 1 : 16;
    return QPoint(int(std::lround(w.x() / g)) * g, int(std::lround(w.y() / g)) * g);
}

void SymbolEditor::zoomIn()
{
    const QPointF c = toWorld(QPointF(width() / 2.0, height() / 2.0));
    m_scale = std::min(m_scale * 1.5, 40.0);
    m_origin = c - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

void SymbolEditor::zoomOut()
{
    const QPointF c = toWorld(QPointF(width() / 2.0, height() / 2.0));
    m_scale = std::max(m_scale / 1.5, 0.1);
    m_origin = c - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

void SymbolEditor::zoomFit()
{
    QRectF b = m_sym.bounds().adjusted(-48, -48, 48, 48);
    if (width() < 10)
        return;
    m_scale = std::clamp(std::min(width() / b.width(), height() / b.height()), 0.2, 20.0);
    m_origin = b.center() - QPointF(width() / 2.0 / m_scale, height() / 2.0 / m_scale);
    update();
}

QString SymbolEditor::windowLabel(const AttrWindow &w) const
{
    const QString name = w.id == AttrId::InstName ? QStringLiteral("InstName") : attrNameForId(w.id);
    const QString v = m_sym.attrs.value(name);
    return v.isEmpty() ? QStringLiteral("<%1>").arg(name.isEmpty() ? QString::number(w.id) : name) : v;
}

void SymbolEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const SchematicStyle st = SchematicStyle::fromSettings();
    p.fillRect(rect(), st.background);
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(transform());
    const QRectF vis = transform().inverted().mapRect(QRectF(rect()));
    SchematicPainter::drawGrid(p, vis, m_scale, st.grid);
    // Origin cross
    p.setPen(QPen(st.unconnected, 1.0 / m_scale));
    p.drawLine(QPointF(-8, 0), QPointF(8, 0));
    p.drawLine(QPointF(0, -8), QPointF(0, 8));

    auto sel = [this](Item::Type t, int i) { return m_selection.contains(Item{t, i}); };
    const double lw = 1.2 / m_scale;
    for (int i = 0; i < m_sym.shapes.size(); ++i) {
        p.setPen(QPen(sel(Item::Shape, i) ? st.selection : st.component, lw));
        SchematicPainter::drawShape(p, m_sym.shapes[i]);
    }
    for (int i = 0; i < m_sym.texts.size(); ++i) {
        const SymText &t = m_sym.texts[i];
        SchematicPainter::drawText(p, t.pos, t.just, t.fontSize, t.text, sel(Item::Text, i) ? st.selection : st.component, st.fontFamily);
    }
    for (int i = 0; i < m_sym.pins.size(); ++i) {
        const SymPin &pin = m_sym.pins[i];
        p.setPen(QPen(sel(Item::Pin, i) ? st.selection : st.pin, lw));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(pin.pos.x() - 4, pin.pos.y() - 4, 8, 8));
        if (!pin.labelNone) {
            SymbolDef tmp;
            tmp.pins << pin;
            SchematicPainter::drawSymbolBody(p, tmp, QPoint(), Orient::R0, st.component, sel(Item::Pin, i) ? st.selection : st.pin, true,
                                             st.fontFamily);
        }
        SchematicPainter::drawText(p, QPointF(pin.pos) + QPointF(5, 5), Justify{Justify::Start, Justify::PerpTop, false, false}, 0,
                                   QString::number(pin.spiceOrder), st.unconnected, st.fontFamily);
    }
    for (int i = 0; i < m_sym.windows.size(); ++i) {
        const AttrWindow &w = m_sym.windows[i];
        if (w.fontSize == 0)
            continue;
        SchematicPainter::drawText(p, w.pos, w.just, w.fontSize, windowLabel(w), sel(Item::Window, i) ? st.selection : st.text, st.fontFamily);
        p.setPen(QPen(st.unconnected, lw));
        p.drawEllipse(QPointF(w.pos), 2, 2);
    }
    if (m_shapeActive) {
        p.setPen(QPen(st.selection, lw, Qt::DashLine));
        SchematicPainter::drawShape(p, m_shape);
    }
    if (m_mode == Mode::AddPin || m_mode == Mode::PlaceText || m_mode == Mode::PlaceWindow) {
        const QPoint s = snap(m_mouseWorld);
        p.setPen(QPen(st.selection, lw));
        p.drawRect(QRectF(s.x() - 4, s.y() - 4, 8, 8));
        if (m_mode == Mode::PlaceText)
            SchematicPainter::drawText(p, s, Justify(), 2, m_pendingText, st.selection, st.fontFamily);
    }
}

SymbolEditor::Item SymbolEditor::itemAt(QPointF w) const
{
    const double tol = 6.0 / m_scale;
    const QString fam = AppSettings::instance().fontFamily();
    for (int i = m_sym.pins.size() - 1; i >= 0; --i)
        if (QLineF(m_sym.pins[i].pos, w).length() < tol + 4)
            return {Item::Pin, i};
    for (int i = m_sym.windows.size() - 1; i >= 0; --i) {
        const AttrWindow &aw = m_sym.windows[i];
        if (SchematicPainter::textRect(aw.pos, aw.just, std::max(1, aw.fontSize), windowLabel(aw), fam).adjusted(-2, -2, 2, 2).contains(w))
            return {Item::Window, i};
    }
    for (int i = m_sym.texts.size() - 1; i >= 0; --i) {
        const SymText &t = m_sym.texts[i];
        if (SchematicPainter::textRect(t.pos, t.just, t.fontSize, t.text, fam).adjusted(-2, -2, 2, 2).contains(w))
            return {Item::Text, i};
    }
    for (int i = m_sym.shapes.size() - 1; i >= 0; --i) {
        const Shape &s = m_sym.shapes[i];
        if (s.kind == Shape::Line) {
            if (distToSegment(w, s.p[0], s.p[1]) < tol)
                return {Item::Shape, i};
        } else {
            const QRectF r = QRectF(s.p[0], s.p[1]).normalized();
            if (r.adjusted(-tol, -tol, tol, tol).contains(w) && !r.adjusted(tol, tol, -tol, -tol).contains(w))
                return {Item::Shape, i};
        }
    }
    return {};
}

void SymbolEditor::setMode(Mode m)
{
    m_mode = m;
    m_shapeActive = false;
    m_shapeStage = 0;
    setCursor(m == Mode::Normal ? Qt::ArrowCursor : (m == Mode::Delete ? Qt::ForbiddenCursor : Qt::CrossCursor));
    if (m == Mode::PlaceText) {
        bool ok = false;
        m_pendingText = QInputDialog::getText(this, tr("Text"), tr("Text to place in the symbol:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || m_pendingText.isEmpty())
            m_mode = Mode::Normal;
    }
    update();
}

void SymbolEditor::startPlaceWindow()
{
    QStringList names{QStringLiteral("InstName"), QStringLiteral("Value"), QStringLiteral("Value2"), QStringLiteral("SpiceModel"),
                      QStringLiteral("SpiceLine"), QStringLiteral("SpiceLine2"), QStringLiteral("Type")};
    bool ok = false;
    const QString n = QInputDialog::getItem(this, tr("Attribute Window"), tr("Attribute to display:"), names, 0, false, &ok);
    if (!ok)
        return;
    m_windowId = n == QLatin1String("InstName") ? AttrId::InstName : attrIdForName(n);
    setMode(Mode::PlaceWindow);
}

void SymbolEditor::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    const QPointF w = toWorld(e->position());
    m_lastWidget = e->position();
    if (e->button() == Qt::MiddleButton) {
        m_panning = true;
        return;
    }
    if (e->button() == Qt::RightButton) {
        if (m_mode != Mode::Normal) {
            setMode(Mode::Normal);
            return;
        }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const bool fine = e->modifiers() & Qt::ControlModifier;
    const QPoint s = snap(w, fine);
    switch (m_mode) {
    case Mode::DrawLine:
    case Mode::DrawRect:
    case Mode::DrawCircle:
    case Mode::DrawArc:
        if (!m_shapeActive) {
            m_shapeActive = true;
            m_shapeStage = 1;
            m_shape = Shape();
            m_shape.kind = m_mode == Mode::DrawLine ? Shape::Line : m_mode == Mode::DrawRect ? Shape::Rect : m_mode == Mode::DrawCircle ? Shape::Circle : Shape::Arc;
            for (QPoint &p : m_shape.p)
                p = s;
            return;
        }
        if (m_shape.kind == Shape::Arc && m_shapeStage < 3) {
            ++m_shapeStage;
            return;
        }
        pushUndo();
        m_sym.shapes << m_shape;
        m_shapeActive = false;
        changed();
        return;
    case Mode::AddPin: {
        SymPin pin;
        pin.pos = s;
        pin.spiceOrder = int(m_sym.pins.size()) + 1;
        pin.name = QStringLiteral("p%1").arg(pin.spiceOrder);
        pin.labelNone = true;
        pushUndo();
        m_sym.pins << pin;
        changed();
        editPin(int(m_sym.pins.size()) - 1);
        return;
    }
    case Mode::PlaceText: {
        pushUndo();
        SymText t;
        t.pos = s;
        t.text = m_pendingText;
        t.just = Justify::fromString(QStringLiteral("Left"));
        m_sym.texts << t;
        setMode(Mode::Normal);
        changed();
        return;
    }
    case Mode::PlaceWindow: {
        pushUndo();
        AttrWindow aw;
        aw.id = m_windowId;
        aw.pos = s;
        aw.just = Justify::fromString(QStringLiteral("Left"));
        aw.fontSize = 2;
        for (int i = m_sym.windows.size() - 1; i >= 0; --i)
            if (m_sym.windows[i].id == aw.id)
                m_sym.windows.removeAt(i);
        m_sym.windows << aw;
        setMode(Mode::Normal);
        changed();
        return;
    }
    case Mode::Delete: {
        const Item it = itemAt(w);
        if (it.type != Item::None) {
            m_selection = {it};
            deleteSelection();
        }
        return;
    }
    case Mode::Normal:
    default: {
        const Item it = itemAt(w);
        if (it.type == Item::None) {
            m_selection.clear();
            m_panning = true;
            update();
            return;
        }
        if (e->modifiers() & Qt::ShiftModifier) {
            if (m_selection.contains(it))
                m_selection.removeAll(it);
            else
                m_selection << it;
        } else if (!m_selection.contains(it)) {
            m_selection = {it};
        }
        m_dragging = true;
        m_dragLast = s;
        m_dragBackup = m_sym.serialize();
        update();
        return;
    }
    }
}

void SymbolEditor::moveItems(QPoint d)
{
    for (const Item &it : std::as_const(m_selection)) {
        switch (it.type) {
        case Item::Shape:
            for (QPoint &p : m_sym.shapes[it.index].p)
                p += d;
            break;
        case Item::Pin: m_sym.pins[it.index].pos += d; break;
        case Item::Text: m_sym.texts[it.index].pos += d; break;
        case Item::Window: m_sym.windows[it.index].pos += d; break;
        default: break;
        }
    }
}

void SymbolEditor::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF w = toWorld(e->position());
    m_mouseWorld = w;
    const QPointF dw = e->position() - m_lastWidget;
    m_lastWidget = e->position();
    const QPoint s = snap(w, e->modifiers() & Qt::ControlModifier);
    emit statusMessage(QStringLiteral("x=%1 y=%2").arg(s.x()).arg(s.y()));
    if (m_panning) {
        m_origin -= dw / m_scale;
        update();
        return;
    }
    if (m_dragging) {
        const QPoint d = s - m_dragLast;
        if (!d.isNull()) {
            moveItems(d);
            m_dragLast = s;
            update();
        }
        return;
    }
    if (m_shapeActive) {
        if (m_shapeStage == 1)
            m_shape.p[1] = s;
        else if (m_shapeStage == 2)
            m_shape.p[2] = m_shape.p[3] = s;
        else
            m_shape.p[3] = s;
    }
    update();
}

void SymbolEditor::mouseReleaseEvent(QMouseEvent *)
{
    if (m_panning) {
        m_panning = false;
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        if (m_dragBackup != m_sym.serialize()) {
            m_undo << m_dragBackup;
            m_redo.clear();
            changed();
        }
    }
}

void SymbolEditor::mouseDoubleClickEvent(QMouseEvent *e)
{
    const Item it = itemAt(toWorld(e->position()));
    switch (it.type) {
    case Item::Pin: editPin(it.index); break;
    case Item::Text: editText(it.index); break;
    case Item::Window: editWindow(it.index); break;
    default:
        if (m_mode == Mode::Normal)
            editAttributes();
        break;
    }
}

void SymbolEditor::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_mode != Mode::Normal)
        return;
    const Item it = itemAt(toWorld(e->pos()));
    if (it.type == Item::Pin) {
        editPin(it.index);
        return;
    }
    if (it.type == Item::Window) {
        editWindow(it.index);
        return;
    }
    if (it.type == Item::Text) {
        editText(it.index);
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Edit Attributes..."), this, &SymbolEditor::editAttributes);
    menu.addAction(tr("Attribute Window..."), this, &SymbolEditor::startPlaceWindow);
    menu.addAction(tr("Add Pin/Port"), this, [this]() { setMode(Mode::AddPin); });
    menu.addSeparator();
    menu.addAction(tr("Line"), this, [this]() { setMode(Mode::DrawLine); });
    menu.addAction(tr("Rectangle"), this, [this]() { setMode(Mode::DrawRect); });
    menu.addAction(tr("Circle"), this, [this]() { setMode(Mode::DrawCircle); });
    menu.addAction(tr("Arc"), this, [this]() { setMode(Mode::DrawArc); });
    menu.addAction(tr("Text"), this, [this]() { setMode(Mode::PlaceText); });
    menu.exec(e->globalPos());
}

void SymbolEditor::wheelEvent(QWheelEvent *e)
{
    const QPointF anchor = toWorld(e->position());
    m_scale = std::clamp(m_scale * std::pow(1.2, e->angleDelta().y() / 120.0), 0.1, 40.0);
    m_origin = anchor - e->position() / m_scale;
    update();
}

void SymbolEditor::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        setMode(Mode::Normal);
        m_selection.clear();
        update();
        return;
    }
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        deleteSelection();
        return;
    }
    DocumentWidget::keyPressEvent(e);
}

void SymbolEditor::deleteSelection()
{
    if (m_selection.isEmpty()) {
        setMode(Mode::Delete);
        return;
    }
    pushUndo();
    QList<int> shapes, pins, texts, windows;
    for (const Item &it : std::as_const(m_selection)) {
        if (it.type == Item::Shape) shapes << it.index;
        if (it.type == Item::Pin) pins << it.index;
        if (it.type == Item::Text) texts << it.index;
        if (it.type == Item::Window) windows << it.index;
    }
    auto rm = [](auto &list, QList<int> idx) {
        std::sort(idx.begin(), idx.end(), std::greater<int>());
        for (int i : idx)
            list.removeAt(i);
    };
    rm(m_sym.shapes, shapes);
    rm(m_sym.pins, pins);
    rm(m_sym.texts, texts);
    rm(m_sym.windows, windows);
    m_selection.clear();
    changed();
}

void SymbolEditor::rotateSelection()
{
    if (m_selection.isEmpty())
        return;
    pushUndo();
    QRectF bb;
    for (const Item &it : std::as_const(m_selection)) {
        QPointF p = it.type == Item::Pin ? QPointF(m_sym.pins[it.index].pos)
                  : it.type == Item::Text ? QPointF(m_sym.texts[it.index].pos)
                  : it.type == Item::Window ? QPointF(m_sym.windows[it.index].pos)
                  : QPointF(m_sym.shapes[it.index].p[0]);
        bb = bb.isNull() ? QRectF(p, QSizeF(1, 1)) : bb.united(QRectF(p, QSizeF(1, 1)));
    }
    const QPoint c = snap(bb.center());
    auto rot = [c](QPoint p) { const QPoint d = p - c; return c + QPoint(-d.y(), d.x()); };
    for (const Item &it : std::as_const(m_selection)) {
        switch (it.type) {
        case Item::Shape:
            for (QPoint &p : m_sym.shapes[it.index].p)
                p = rot(p);
            break;
        case Item::Pin: m_sym.pins[it.index].pos = rot(m_sym.pins[it.index].pos); break;
        case Item::Text: m_sym.texts[it.index].pos = rot(m_sym.texts[it.index].pos); break;
        case Item::Window: m_sym.windows[it.index].pos = rot(m_sym.windows[it.index].pos); break;
        default: break;
        }
    }
    changed();
}

void SymbolEditor::mirrorSelection()
{
    if (m_selection.isEmpty())
        return;
    pushUndo();
    int cx = 0, n = 0;
    for (const Item &it : std::as_const(m_selection)) {
        if (it.type == Item::Pin) { cx += m_sym.pins[it.index].pos.x(); ++n; }
        else if (it.type == Item::Shape) { cx += m_sym.shapes[it.index].p[0].x(); ++n; }
    }
    cx = n ? snap(QPointF(double(cx) / n, 0)).x() : 0;
    auto mir = [cx](QPoint p) { return QPoint(2 * cx - p.x(), p.y()); };
    for (const Item &it : std::as_const(m_selection)) {
        switch (it.type) {
        case Item::Shape: {
            Shape &s = m_sym.shapes[it.index];
            for (QPoint &p : s.p)
                p = mir(p);
            if (s.kind == Shape::Arc)
                std::swap(s.p[2], s.p[3]);
            break;
        }
        case Item::Pin: m_sym.pins[it.index].pos = mir(m_sym.pins[it.index].pos); break;
        case Item::Text: m_sym.texts[it.index].pos = mir(m_sym.texts[it.index].pos); break;
        case Item::Window: m_sym.windows[it.index].pos = mir(m_sym.windows[it.index].pos); break;
        default: break;
        }
    }
    changed();
}

void SymbolEditor::editPin(int index)
{
    SymPin &pin = m_sym.pins[index];
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Pin/Port Properties"));
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit(pin.name);
    auto *order = new QSpinBox;
    order->setRange(1, 9999);
    order->setValue(pin.spiceOrder);
    auto *just = new QComboBox;
    const QStringList justs = {QStringLiteral("NONE"), QStringLiteral("LEFT"), QStringLiteral("RIGHT"), QStringLiteral("TOP"),
                               QStringLiteral("BOTTOM"), QStringLiteral("VLEFT"), QStringLiteral("VRIGHT"), QStringLiteral("VTOP"),
                               QStringLiteral("VBOTTOM")};
    just->addItems(justs);
    just->setCurrentText(pin.labelNone ? QStringLiteral("NONE") : pin.labelJust.toPinString());
    auto *offset = new QSpinBox;
    offset->setRange(0, 200);
    offset->setValue(pin.labelOffset);
    form->addRow(tr("Pin name:"), name);
    form->addRow(tr("Netlist order:"), order);
    form->addRow(tr("Label justification:"), just);
    form->addRow(tr("Label offset:"), offset);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;
    pushUndo();
    pin.name = name->text().trimmed();
    pin.spiceOrder = order->value();
    pin.labelJust = Justify::fromPinString(just->currentText(), &pin.labelNone);
    pin.labelOffset = offset->value();
    changed();
}

void SymbolEditor::editText(int index)
{
    SymText &t = m_sym.texts[index];
    bool ok = false;
    const QString v = QInputDialog::getText(this, tr("Edit Text"), tr("Text:"), QLineEdit::Normal, t.text, &ok);
    if (!ok)
        return;
    pushUndo();
    t.text = v;
    changed();
}

void SymbolEditor::editWindow(int index)
{
    AttrWindow &w = m_sym.windows[index];
    QStringList justs = {QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Center"), QStringLiteral("Top"),
                         QStringLiteral("Bottom"), QStringLiteral("VLeft"), QStringLiteral("VRight"), QStringLiteral("VCenter"),
                         QStringLiteral("VTop"), QStringLiteral("VBottom"), QStringLiteral("Invisible")};
    bool ok = false;
    const QString j = QInputDialog::getItem(this, tr("Attribute Window"), tr("Justification:"), justs, std::max(0, int(justs.indexOf(w.just.toString()))), false, &ok);
    if (!ok)
        return;
    pushUndo();
    w.just = Justify::fromString(j);
    if (w.just.invisible)
        w.fontSize = 0;
    else if (w.fontSize == 0)
        w.fontSize = 2;
    changed();
}

void SymbolEditor::editAttributes()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Symbol Attribute Editor"));
    dlg.resize(520, 360);
    auto *form = new QFormLayout(&dlg);
    auto *type = new QComboBox;
    type->addItems({QStringLiteral("Cell"), QStringLiteral("Block")});
    type->setCurrentIndex(m_sym.type == SymbolDef::Type::Block ? 1 : 0);
    form->addRow(tr("Symbol type:"), type);
    QHash<QString, QLineEdit *> edits;
    for (const char *k : {"Prefix", "SpiceModel", "Value", "Value2", "SpiceLine", "SpiceLine2", "Description", "ModelFile", "InstName"}) {
        auto *e = new QLineEdit(m_sym.attrs.value(QLatin1String(k)));
        form->addRow(QLatin1String(k) + QStringLiteral(":"), e);
        edits.insert(QLatin1String(k), e);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;
    pushUndo();
    m_sym.type = type->currentIndex() == 1 ? SymbolDef::Type::Block : SymbolDef::Type::Cell;
    for (auto it = edits.constBegin(); it != edits.constEnd(); ++it) {
        const QString v = it.value()->text().trimmed();
        if (v.isEmpty())
            m_sym.attrs.remove(it.key());
        else
            m_sym.attrs.set(it.key(), v);
    }
    changed();
}

SymbolDef SymbolEditor::generateFromSubckt(const QString &name, const QStringList &ports, const QString &modelFile)
{
    SymbolDef d;
    d.type = SymbolDef::Type::Cell;
    const int n = ports.size();
    const int leftCount = (n + 1) / 2;
    const int rightCount = n - leftCount;
    const int rows = std::max(leftCount, rightCount);
    const int h = std::max(2, rows + 1) * 32;
    const int w = 128;
    Shape r;
    r.kind = Shape::Rect;
    r.p[0] = QPoint(-w / 2, -h / 2);
    r.p[1] = QPoint(w / 2, h / 2);
    d.shapes << r;
    for (int i = 0; i < n; ++i) {
        SymPin p;
        const bool left = i < leftCount;
        const int row = left ? i : i - leftCount;
        p.pos = QPoint(left ? -w / 2 - 32 : w / 2 + 32, -h / 2 + 32 + row * 32);
        Shape lead;
        lead.kind = Shape::Line;
        lead.p[0] = p.pos;
        lead.p[1] = QPoint(left ? -w / 2 : w / 2, p.pos.y());
        d.shapes << lead;
        p.name = ports[i];
        p.spiceOrder = i + 1;
        p.labelNone = false;
        p.labelJust = Justify::fromString(left ? QStringLiteral("Left") : QStringLiteral("Right"));
        p.labelOffset = 40;
        d.pins << p;
    }
    AttrWindow wn;
    wn.id = AttrId::InstName;
    wn.pos = QPoint(0, -h / 2 - 8);
    wn.just = Justify::fromString(QStringLiteral("Bottom"));
    d.windows << wn;
    AttrWindow wv;
    wv.id = AttrId::Value;
    wv.pos = QPoint(0, h / 2 + 8);
    wv.just = Justify::fromString(QStringLiteral("Top"));
    d.windows << wv;
    d.attrs.set(QStringLiteral("Prefix"), QStringLiteral("X"));
    if (!modelFile.isEmpty())
        d.attrs.set(QStringLiteral("SpiceModel"), QFileInfo(modelFile).fileName());
    d.attrs.set(QStringLiteral("Value"), name);
    d.attrs.set(QStringLiteral("Value2"), name);
    d.attrs.set(QStringLiteral("Description"), QStringLiteral("Auto-generated symbol for subcircuit %1").arg(name));
    return d;
}

SymbolDef SymbolEditor::generateBlock(const QStringList &inputs, const QStringList &outputs, const QStringList &bidirs)
{
    SymbolDef d;
    d.type = SymbolDef::Type::Block;
    d.typeText = QStringLiteral("BLOCK");
    const QStringList left = inputs + bidirs;
    const QStringList right = outputs;
    const int rows = std::max(std::max(left.size(), right.size()), qsizetype(1));
    const int h = (rows + 1) * 32;
    const int w = 160;
    Shape r;
    r.kind = Shape::Rect;
    r.p[0] = QPoint(-w / 2, -h / 2);
    r.p[1] = QPoint(w / 2, h / 2);
    d.shapes << r;
    int order = 1;
    for (int i = 0; i < left.size(); ++i) {
        SymPin p;
        p.pos = QPoint(-w / 2, -h / 2 + 32 + i * 32);
        p.name = left[i];
        p.spiceOrder = order++;
        p.labelNone = false;
        p.labelJust = Justify::fromString(QStringLiteral("Left"));
        p.labelOffset = 8;
        d.pins << p;
    }
    for (int i = 0; i < right.size(); ++i) {
        SymPin p;
        p.pos = QPoint(w / 2, -h / 2 + 32 + i * 32);
        p.name = right[i];
        p.spiceOrder = order++;
        p.labelNone = false;
        p.labelJust = Justify::fromString(QStringLiteral("Right"));
        p.labelOffset = 8;
        d.pins << p;
    }
    AttrWindow wn;
    wn.id = AttrId::InstName;
    wn.pos = QPoint(0, -h / 2 - 8);
    wn.just = Justify::fromString(QStringLiteral("Bottom"));
    d.windows << wn;
    return d;
}

bool SymbolEditor::exportImage(const QString &path)
{
    QRectF b = m_sym.bounds().adjusted(-24, -24, 24, 24);
    const double s = 4;
    QImage img((b.size() * s).toSize(), QImage::Format_ARGB32);
    const SchematicStyle st = SchematicStyle::fromSettings();
    img.fill(st.background);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(s, s);
    p.translate(-b.topLeft());
    p.setPen(QPen(st.component, 1));
    SchematicPainter::drawSymbolBody(p, m_sym, QPoint(), Orient::R0, st.component, st.pin, true, st.fontFamily);
    p.end();
    return img.save(path);
}
