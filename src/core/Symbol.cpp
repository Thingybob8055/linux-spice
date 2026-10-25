#include "Symbol.h"

#include "TextIO.h"

#include <QHash>

namespace {

const QList<QPair<int, QString>> &attrIdTable()
{
    static const QList<QPair<int, QString>> table = {
        {AttrId::InstName, QStringLiteral("InstName")},
        {AttrId::Type, QStringLiteral("Type")},
        {AttrId::Prefix, QStringLiteral("Prefix")},
        {AttrId::Value, QStringLiteral("Value")},
        {AttrId::Description, QStringLiteral("Description")},
        {AttrId::SpiceModel, QStringLiteral("SpiceModel")},
        {AttrId::SpiceLine, QStringLiteral("SpiceLine")},
        {AttrId::SpiceLine2, QStringLiteral("SpiceLine2")},
        {AttrId::ModelFile, QStringLiteral("ModelFile")},
        {AttrId::Value2, QStringLiteral("Value2")},
    };
    return table;
}

int toInt(const QString &s)
{
    return s.toInt();
}

} // namespace

int attrIdForName(const QString &name)
{
    for (const auto &e : attrIdTable())
        if (e.second.compare(name, Qt::CaseInsensitive) == 0)
            return e.first;
    return -1;
}

QString attrNameForId(int id)
{
    for (const auto &e : attrIdTable())
        if (e.first == id)
            return e.second;
    return QString();
}

double fontScale(int fontSize)
{
    static const double scales[] = {0.625, 1.0, 1.5, 2.0, 2.5, 3.5, 5.0, 7.0};
    if (fontSize < 0)
        fontSize = 2;
    if (fontSize > 7)
        fontSize = 7;
    return scales[fontSize] / 1.5;
}

// ---------------------------------------------------------------- Shape

QString Shape::keyword() const
{
    switch (kind) {
    case Line: return QStringLiteral("LINE");
    case Rect: return QStringLiteral("RECTANGLE");
    case Circle: return QStringLiteral("CIRCLE");
    case Arc: return QStringLiteral("ARC");
    }
    return QString();
}

bool Shape::parse(const QStringList &tok, Shape &out)
{
    if (tok.isEmpty())
        return false;
    const QString kw = tok[0].toUpper();
    int npts = 2;
    if (kw == QLatin1String("LINE")) out.kind = Line;
    else if (kw == QLatin1String("RECTANGLE")) out.kind = Rect;
    else if (kw == QLatin1String("CIRCLE")) out.kind = Circle;
    else if (kw == QLatin1String("ARC")) { out.kind = Arc; npts = 4; }
    else return false;
    if (tok.size() < 2 + npts * 2)
        return false;
    out.width = tok[1];
    for (int i = 0; i < npts; ++i)
        out.p[i] = QPoint(toInt(tok[2 + 2 * i]), toInt(tok[3 + 2 * i]));
    const int styleIdx = 2 + npts * 2;
    if (tok.size() > styleIdx) {
        out.hasStyle = true;
        out.style = LineStyle(std::clamp(toInt(tok[styleIdx]), 0, 4));
    }
    return true;
}

QString Shape::serialize() const
{
    QString s = keyword() + ' ' + width;
    const int npts = kind == Arc ? 4 : 2;
    for (int i = 0; i < npts; ++i)
        s += QStringLiteral(" %1 %2").arg(p[i].x()).arg(p[i].y());
    if (hasStyle || style != LineStyle::Solid)
        s += QStringLiteral(" %1").arg(int(style));
    return s;
}

Shape Shape::transformed(Orient o, QPoint offset) const
{
    Shape r = *this;
    const int npts = kind == Arc ? 4 : 2;
    for (int i = 0; i < npts; ++i)
        r.p[i] = orientApply(o, p[i]) + offset;
    if (kind == Arc && isMirrored(o))
        std::swap(r.p[2], r.p[3]); // mirroring reverses the sweep direction
    return r;
}

QRect Shape::bounds() const
{
    return rectFromPoints(p[0], p[1]);
}

// ---------------------------------------------------------------- AttrList

QString AttrList::value(const QString &key, const QString &def) const
{
    for (const auto &kv : m_items)
        if (kv.first.compare(key, Qt::CaseInsensitive) == 0)
            return kv.second;
    return def;
}

bool AttrList::contains(const QString &key) const
{
    for (const auto &kv : m_items)
        if (kv.first.compare(key, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

void AttrList::set(const QString &key, const QString &value)
{
    for (auto &kv : m_items) {
        if (kv.first.compare(key, Qt::CaseInsensitive) == 0) {
            kv.second = value;
            return;
        }
    }
    m_items.append({key, value});
}

void AttrList::remove(const QString &key)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].first.compare(key, Qt::CaseInsensitive) == 0) {
            m_items.removeAt(i);
            return;
        }
    }
}

// ---------------------------------------------------------------- SymbolDef

QSharedPointer<SymbolDef> SymbolDef::parse(const QString &text, QString *error)
{
    auto sym = QSharedPointer<SymbolDef>::create();
    SymPin *currentPin = nullptr;
    const QStringList lines = textio::splitLines(text);
    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.startsWith(QChar(0xFEFF)))
            line.remove(0, 1);
        if (line.trimmed().isEmpty())
            continue;
        const QStringList tok = textio::tokenize(line);
        const QString kw = tok[0].toUpper();
        if (kw == QLatin1String("VERSION")) {
            sym->versionLine = line.trimmed();
        } else if (kw == QLatin1String("SYMBOLTYPE")) {
            sym->typeText = tok.value(1, QStringLiteral("CELL"));
            sym->type = sym->typeText.compare(QLatin1String("BLOCK"), Qt::CaseInsensitive) == 0 ? Type::Block : Type::Cell;
        } else if (kw == QLatin1String("LINE") || kw == QLatin1String("RECTANGLE") || kw == QLatin1String("CIRCLE")
                   || kw == QLatin1String("ARC")) {
            Shape s;
            if (Shape::parse(tok, s))
                sym->shapes.append(s);
        } else if (kw == QLatin1String("TEXT")) {
            const QStringList t = textio::tokenize(line, 5);
            if (t.size() >= 5) {
                SymText st;
                st.pos = QPoint(toInt(t[1]), toInt(t[2]));
                st.just = Justify::fromString(t[3]);
                st.fontSize = toInt(t[4]);
                st.text = t.value(5);
                sym->texts.append(st);
            }
        } else if (kw == QLatin1String("WINDOW")) {
            if (tok.size() >= 5) {
                AttrWindow w;
                w.id = toInt(tok[1]);
                w.pos = QPoint(toInt(tok[2]), toInt(tok[3]));
                w.just = Justify::fromString(tok[4]);
                w.fontSize = tok.size() > 5 ? toInt(tok[5]) : 2;
                sym->windows.append(w);
            }
        } else if (kw == QLatin1String("SYMATTR")) {
            const QStringList t = textio::tokenize(line, 2);
            if (t.size() >= 2)
                sym->attrs.items().append({t[1], t.value(2)});
        } else if (kw == QLatin1String("PIN")) {
            if (tok.size() >= 3) {
                SymPin p;
                p.pos = QPoint(toInt(tok[1]), toInt(tok[2]));
                p.labelJust = Justify::fromPinString(tok.value(3, QStringLiteral("NONE")), &p.labelNone);
                p.labelOffset = tok.size() > 4 ? toInt(tok[4]) : 8;
                sym->pins.append(p);
                currentPin = &sym->pins.last();
            }
        } else if (kw == QLatin1String("PINATTR")) {
            if (currentPin) {
                const QStringList t = textio::tokenize(line, 2);
                const QString key = t.value(1);
                const QString val = t.value(2);
                if (key.compare(QLatin1String("PinName"), Qt::CaseInsensitive) == 0)
                    currentPin->name = val;
                else if (key.compare(QLatin1String("SpiceOrder"), Qt::CaseInsensitive) == 0)
                    currentPin->spiceOrder = val.toInt();
            }
        }
    }
    // Pins without explicit SpiceOrder get their file order.
    for (int i = 0; i < sym->pins.size(); ++i)
        if (sym->pins[i].spiceOrder == 0)
            sym->pins[i].spiceOrder = i + 1;
    if (error)
        error->clear();
    return sym;
}

QSharedPointer<SymbolDef> SymbolDef::load(const QString &path, QString *error)
{
    bool ok = false;
    const QString text = textio::readText(path, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(path);
        return {};
    }
    auto sym = parse(text, error);
    if (sym)
        sym->filePath = path;
    return sym;
}

QString SymbolDef::serialize() const
{
    QStringList out;
    out << versionLine;
    out << QStringLiteral("SymbolType %1").arg(type == Type::Block ? QStringLiteral("BLOCK") : QStringLiteral("CELL"));
    for (const Shape &s : shapes)
        out << s.serialize();
    for (const SymText &t : texts)
        out << QStringLiteral("TEXT %1 %2 %3 %4 %5").arg(t.pos.x()).arg(t.pos.y()).arg(t.just.toString()).arg(t.fontSize).arg(t.text);
    for (const AttrWindow &w : windows)
        out << QStringLiteral("WINDOW %1 %2 %3 %4 %5").arg(w.id).arg(w.pos.x()).arg(w.pos.y()).arg(w.just.toString()).arg(w.fontSize);
    for (const auto &kv : attrs.items())
        out << QStringLiteral("SYMATTR %1 %2").arg(kv.first, kv.second);
    for (const SymPin &p : pins) {
        out << QStringLiteral("PIN %1 %2 %3 %4")
                   .arg(p.pos.x())
                   .arg(p.pos.y())
                   .arg(p.labelNone ? QStringLiteral("NONE") : p.labelJust.toPinString())
                   .arg(p.labelOffset);
        out << QStringLiteral("PINATTR PinName %1").arg(p.name);
        out << QStringLiteral("PINATTR SpiceOrder %1").arg(p.spiceOrder);
    }
    return out.join('\n') + '\n';
}

bool SymbolDef::save(const QString &path, QString *error) const
{
    textio::TextFile tf;
    tf.text = serialize();
    tf.encoding = textio::Encoding::Latin1;
    tf.crlf = true;
    return textio::writeFile(path, tf, error);
}

const AttrWindow *SymbolDef::window(int id) const
{
    for (const AttrWindow &w : windows)
        if (w.id == id)
            return &w;
    return nullptr;
}

QRect SymbolDef::bounds() const
{
    QRect r;
    auto add = [&r](QRect q) { r = r.isNull() ? q : r.united(q); };
    for (const Shape &s : shapes) {
        add(s.bounds().normalized());
    }
    for (const SymPin &p : pins)
        add(QRect(p.pos, QSize(1, 1)));
    for (const SymText &t : texts)
        add(QRect(t.pos, QSize(1, 1)));
    if (r.isNull())
        r = QRect(-16, -16, 32, 32);
    return r;
}

QList<SymPin> SymbolDef::pinsBySpiceOrder() const
{
    QList<SymPin> sorted = pins;
    std::stable_sort(sorted.begin(), sorted.end(), [](const SymPin &a, const SymPin &b) { return a.spiceOrder < b.spiceOrder; });
    return sorted;
}
