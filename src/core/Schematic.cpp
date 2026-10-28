#include "Schematic.h"

#include "SymbolLibrary.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace {

int toInt(const QString &s) { return s.toInt(); }

struct PointHash {
    QHash<qint64, int> counts;
    static qint64 key(QPoint p) { return (qint64(p.x()) << 32) ^ (quint32)p.y(); }
    void add(QPoint p, int n = 1) { counts[key(p)] += n; }
    int get(QPoint p) const { return counts.value(key(p)); }
};

} // namespace

// LTspice writes a newline in schematic text as "\n" and a backslash as "\\"
// (".lib ..\\models\\x.lib" in the file is ".lib ..\models\x.lib").
QString unescapeText(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == '\\')) {
            out += s[i + 1] == 'n' ? QChar('\n') : QChar('\\');
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

QString escapeText(const QString &s)
{
    QString out = s;
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    out.replace('\\', QLatin1String("\\\\"));
    out.replace('\n', QLatin1String("\\n"));
    return out;
}

// ---------------------------------------------------------------- SymbolInstance

QString SymbolInstance::attr(const QString &key) const
{
    if (attrs.contains(key))
        return attrs.value(key);
    if (def)
        return def->attrs.value(key);
    return QString();
}

const AttrWindow *SymbolInstance::window(int id) const
{
    for (const AttrWindow &w : windows)
        if (w.id == id)
            return &w;
    return def ? def->window(id) : nullptr;
}

QList<QPoint> SymbolInstance::pinPositions() const
{
    QList<QPoint> out;
    if (!def)
        return out;
    for (const SymPin &p : def->pins)
        out << pinWorld(p);
    return out;
}

QRect SymbolInstance::worldBounds() const
{
    const QRect local = def ? def->bounds() : QRect(-16, -16, 32, 32);
    const QPoint a = pos + orientApply(orient, local.topLeft());
    const QPoint b = pos + orientApply(orient, local.bottomRight());
    return rectFromPoints(a, b);
}

// ---------------------------------------------------------------- Schematic

bool Schematic::parse(const QString &text, QString *error)
{
    wires.clear();
    flags.clear();
    symbols.clear();
    texts.clear();
    shapes.clear();
    dataFlags.clear();
    busTaps.clear();

    const QStringList lines = textio::splitLines(text);
    SymbolInstance *cur = nullptr;
    int lineNo = 0;
    for (const QString &rawLine : lines) {
        ++lineNo;
        QString line = rawLine;
        if (line.startsWith(QChar(0xFEFF)))
            line.remove(0, 1);
        if (line.trimmed().isEmpty())
            continue;
        const QStringList tok = textio::tokenize(line);
        const QString kw = tok[0].toUpper();

        if (kw == QLatin1String("VERSION")) {
            versionLine = line.trimmed();
        } else if (kw == QLatin1String("SHEET")) {
            sheetNumber = tok.value(1).toInt();
            sheetSize = QSize(tok.value(2).toInt(), tok.value(3).toInt());
        } else if (kw == QLatin1String("WIRE")) {
            if (tok.size() >= 5)
                wires.append({QPoint(toInt(tok[1]), toInt(tok[2])), QPoint(toInt(tok[3]), toInt(tok[4]))});
            cur = nullptr;
        } else if (kw == QLatin1String("FLAG")) {
            const QStringList t = textio::tokenize(line, 3);
            if (t.size() >= 3) {
                NetFlag f;
                f.pos = QPoint(toInt(t[1]), toInt(t[2]));
                f.name = t.value(3).trimmed();
                flags.append(f);
            }
            cur = nullptr;
        } else if (kw == QLatin1String("IOPIN")) {
            if (tok.size() >= 4) {
                const QPoint p(toInt(tok[1]), toInt(tok[2]));
                bool attached = false;
                for (int i = flags.size() - 1; i >= 0; --i) {
                    if (flags[i].pos == p) {
                        flags[i].ioDirection = tok[3];
                        attached = true;
                        break;
                    }
                }
                if (!attached) {
                    NetFlag f;
                    f.pos = p;
                    f.ioDirection = tok[3];
                    flags.append(f);
                }
            }
        } else if (kw == QLatin1String("DATAFLAG")) {
            const QStringList t = textio::tokenize(line, 3);
            if (t.size() >= 3) {
                DataFlag d;
                d.pos = QPoint(toInt(t[1]), toInt(t[2]));
                QString e = t.value(3).trimmed();
                if (e.startsWith('"') && e.endsWith('"') && e.size() >= 2)
                    e = e.mid(1, e.size() - 2);
                d.expr = e;
                dataFlags.append(d);
            }
            cur = nullptr;
        } else if (kw == QLatin1String("BUSTAP")) {
            if (tok.size() >= 5)
                busTaps.append({QPoint(toInt(tok[1]), toInt(tok[2])), QPoint(toInt(tok[3]), toInt(tok[4]))});
        } else if (kw == QLatin1String("SYMBOL")) {
            if (tok.size() >= 4) {
                SymbolInstance s;
                s.symbolName = tok[1];
                s.pos = QPoint(toInt(tok[2]), toInt(tok[3]));
                s.orient = orientFromString(tok.value(4, QStringLiteral("R0")));
                symbols.append(s);
                cur = &symbols.last();
            }
        } else if (kw == QLatin1String("WINDOW")) {
            if (cur && tok.size() >= 5) {
                AttrWindow w;
                w.id = toInt(tok[1]);
                w.pos = QPoint(toInt(tok[2]), toInt(tok[3]));
                w.just = Justify::fromString(tok[4]);
                w.fontSize = tok.size() > 5 ? toInt(tok[5]) : 2;
                cur->windows.append(w);
            }
        } else if (kw == QLatin1String("SYMATTR")) {
            if (cur) {
                const QStringList t = textio::tokenize(line, 2);
                if (t.size() >= 2)
                    cur->attrs.items().append({t[1], t.value(2)});
            }
        } else if (kw == QLatin1String("TEXT")) {
            const QStringList t = textio::tokenize(line, 5);
            if (t.size() >= 5) {
                SchText st;
                st.pos = QPoint(toInt(t[1]), toInt(t[2]));
                st.just = Justify::fromString(t[3]);
                st.fontSize = toInt(t[4]);
                QString content = t.value(5);
                if (content.startsWith('!')) {
                    st.directive = true;
                    content.remove(0, 1);
                } else if (content.startsWith(';')) {
                    content.remove(0, 1);
                }
                st.text = unescapeText(content);
                texts.append(st);
            }
            cur = nullptr;
        } else if (kw == QLatin1String("LINE") || kw == QLatin1String("RECTANGLE") || kw == QLatin1String("CIRCLE")
                   || kw == QLatin1String("ARC")) {
            Shape s;
            if (Shape::parse(tok, s))
                shapes.append(s);
            cur = nullptr;
        } else {
            // Unknown keyword: ignore but keep going (forward compatible).
        }
    }
    if (error)
        error->clear();
    return true;
}

QString Schematic::serialize() const
{
    QStringList out;
    out << versionLine;
    out << QStringLiteral("SHEET %1 %2 %3").arg(sheetNumber).arg(sheetSize.width()).arg(sheetSize.height());
    for (const Wire &w : wires)
        out << QStringLiteral("WIRE %1 %2 %3 %4").arg(w.a.x()).arg(w.a.y()).arg(w.b.x()).arg(w.b.y());
    for (const NetFlag &f : flags) {
        if (!f.name.isEmpty() || f.ioDirection.isEmpty())
            out << QStringLiteral("FLAG %1 %2 %3").arg(f.pos.x()).arg(f.pos.y()).arg(f.name);
        if (!f.ioDirection.isEmpty())
            out << QStringLiteral("IOPIN %1 %2 %3").arg(f.pos.x()).arg(f.pos.y()).arg(f.ioDirection);
    }
    for (const BusTap &b : busTaps)
        out << QStringLiteral("BUSTAP %1 %2 %3 %4").arg(b.a.x()).arg(b.a.y()).arg(b.b.x()).arg(b.b.y());
    for (const SymbolInstance &s : symbols) {
        out << QStringLiteral("SYMBOL %1 %2 %3 %4").arg(s.symbolName).arg(s.pos.x()).arg(s.pos.y()).arg(orientToString(s.orient));
        for (const AttrWindow &w : s.windows)
            out << QStringLiteral("WINDOW %1 %2 %3 %4 %5").arg(w.id).arg(w.pos.x()).arg(w.pos.y()).arg(w.just.toString()).arg(w.fontSize);
        for (const auto &kv : s.attrs.items())
            out << QStringLiteral("SYMATTR %1 %2").arg(kv.first, kv.second);
    }
    for (const DataFlag &d : dataFlags)
        out << QStringLiteral("DATAFLAG %1 %2 \"%3\"").arg(d.pos.x()).arg(d.pos.y()).arg(d.expr);
    for (const SchText &t : texts)
        out << QStringLiteral("TEXT %1 %2 %3 %4 %5%6")
                   .arg(t.pos.x())
                   .arg(t.pos.y())
                   .arg(t.just.toString())
                   .arg(t.fontSize)
                   .arg(t.directive ? QStringLiteral("!") : QStringLiteral(";"))
                   .arg(escapeText(t.text));
    for (const Shape &s : shapes)
        out << s.serialize();
    return out.join('\n') + '\n';
}

bool Schematic::load(const QString &path, QString *error)
{
    textio::TextFile tf;
    if (!textio::readFile(path, tf, error))
        return false;
    encoding = tf.encoding;
    filePath = QFileInfo(path).absoluteFilePath();
    if (!parse(tf.text, error))
        return false;
    resolveSymbols();
    return true;
}

bool Schematic::save(const QString &path, QString *error)
{
    textio::TextFile tf;
    tf.text = serialize();
    tf.encoding = encoding == textio::Encoding::Utf8 ? textio::Encoding::Latin1 : encoding;
    // Fall back to UTF-16 when the content cannot be represented in Windows-1252.
    if (tf.encoding == textio::Encoding::Latin1) {
        for (QChar c : tf.text) {
            if (c.unicode() > 0xFF && c.unicode() != 0x03BC) {
                tf.encoding = textio::Encoding::Utf16LE;
                break;
            }
        }
    }
    tf.crlf = true;
    if (!textio::writeFile(path, tf, error))
        return false;
    filePath = QFileInfo(path).absoluteFilePath();
    encoding = tf.encoding;
    return true;
}

QString Schematic::directory() const
{
    return filePath.isEmpty() ? QString() : QFileInfo(filePath).absolutePath();
}

void Schematic::resolveSymbols()
{
    const QString dir = directory();
    for (SymbolInstance &s : symbols)
        s.def = SymbolLibrary::instance().find(s.symbolName, dir);
}

QStringList Schematic::unresolvedSymbols() const
{
    QStringList out;
    for (const SymbolInstance &s : symbols)
        if (!s.def && !out.contains(s.symbolName))
            out << s.symbolName;
    return out;
}

QRect Schematic::bounds() const
{
    QRect r;
    auto add = [&r](QRect q) { r = r.isNull() ? q : r.united(q); };
    for (const Wire &w : wires)
        add(rectFromPoints(w.a, w.b).adjusted(0, 0, 1, 1));
    for (const NetFlag &f : flags)
        add(QRect(f.pos - QPoint(16, 16), QSize(32, 32)));
    for (const SymbolInstance &s : symbols)
        add(s.worldBounds());
    for (const SchText &t : texts) {
        const QStringList lines = t.text.split('\n');
        int w = 0;
        for (const QString &l : lines)
            w = std::max<int>(w, l.size());
        const double sc = fontScale(t.fontSize);
        const int width = int(w * 9 * sc), height = int(lines.size() * 18 * sc);
        if (t.just.vertical)
            add(QRect(t.pos.x() - height, t.pos.y() - width, height * 2, width * 2));
        else
            add(QRect(t.pos.x() - width, t.pos.y() - height, width * 2, height * 2));
    }
    for (const Shape &s : shapes)
        add(s.bounds().normalized());
    return r;
}

QStringList Schematic::directives() const
{
    QStringList out;
    for (const SchText &t : texts) {
        if (!t.directive)
            continue;
        for (const QString &l : t.text.split('\n'))
            out << l;
    }
    return out;
}

QString Schematic::nextInstName(const QString &prefix) const
{
    const QString p = prefix.isEmpty() ? QStringLiteral("X") : prefix;
    QSet<int> used;
    const QRegularExpression re(QStringLiteral("^%1(\\d+)$").arg(QRegularExpression::escape(p)),
                                QRegularExpression::CaseInsensitiveOption);
    for (const SymbolInstance &s : symbols) {
        const auto m = re.match(s.instName());
        if (m.hasMatch())
            used.insert(m.captured(1).toInt());
    }
    int n = 1;
    while (used.contains(n))
        ++n;
    return p + QString::number(n);
}

void Schematic::normalizeWires()
{
    // Remove zero-length wires and exact duplicates.
    QList<Wire> ws;
    QSet<QString> seen;
    for (const Wire &w : wires) {
        if (w.a == w.b)
            continue;
        const QPoint a = (w.a.x() < w.b.x() || (w.a.x() == w.b.x() && w.a.y() < w.b.y())) ? w.a : w.b;
        const QPoint b = a == w.a ? w.b : w.a;
        const QString key = QStringLiteral("%1,%2,%3,%4").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y());
        if (seen.contains(key))
            continue;
        seen.insert(key);
        ws.append(w);
    }

    // Collect all connection points (wire ends, pins, flags).
    QList<QPoint> points;
    for (const Wire &w : ws)
        points << w.a << w.b;
    for (const SymbolInstance &s : symbols)
        points << s.pinPositions();
    for (const NetFlag &f : flags)
        points << f.pos;

    // Split wires at any connection point that lies strictly inside them.
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 8) {
        changed = false;
        QList<Wire> out;
        for (const Wire &w : ws) {
            QList<QPoint> cuts;
            for (const QPoint &p : points) {
                if (p != w.a && p != w.b && pointOnSegment(p, w.a, w.b))
                    cuts << p;
            }
            if (cuts.isEmpty()) {
                out << w;
                continue;
            }
            changed = true;
            std::sort(cuts.begin(), cuts.end(), [&](QPoint p, QPoint q) {
                return (p - w.a).manhattanLength() < (q - w.a).manhattanLength();
            });
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            QPoint prev = w.a;
            for (const QPoint &c : cuts) {
                out << Wire{prev, c};
                prev = c;
            }
            out << Wire{prev, w.b};
        }
        ws = out;
    }
    wires = ws;
}

QList<QPoint> Schematic::junctions() const
{
    PointHash ends;
    for (const Wire &w : wires) {
        ends.add(w.a);
        ends.add(w.b);
    }
    PointHash pins;
    for (const SymbolInstance &s : symbols)
        for (const QPoint &p : s.pinPositions())
            pins.add(p);

    QList<QPoint> out;
    QSet<qint64> done;
    auto consider = [&](QPoint p) {
        const qint64 k = PointHash::key(p);
        if (done.contains(k))
            return;
        done.insert(k);
        int n = ends.get(p) + pins.get(p);
        for (const Wire &w : wires)
            if (p != w.a && p != w.b && pointOnSegment(p, w.a, w.b))
                n += 2;
        if (n >= 3 && ends.get(p) > 0)
            out << p;
    };
    for (const Wire &w : wires) {
        consider(w.a);
        consider(w.b);
    }
    return out;
}

QList<QPoint> Schematic::unconnectedPins() const
{
    PointHash conn;
    for (const Wire &w : wires) {
        conn.add(w.a);
        conn.add(w.b);
    }
    for (const NetFlag &f : flags)
        conn.add(f.pos);
    PointHash pins;
    for (const SymbolInstance &s : symbols)
        for (const QPoint &p : s.pinPositions())
            pins.add(p);

    QList<QPoint> out;
    for (const SymbolInstance &s : symbols) {
        for (const QPoint &p : s.pinPositions()) {
            if (conn.get(p) > 0 || pins.get(p) > 1)
                continue;
            bool onWire = false;
            for (const Wire &w : wires) {
                if (pointOnSegment(p, w.a, w.b)) {
                    onWire = true;
                    break;
                }
            }
            if (!onWire)
                out << p;
        }
    }
    return out;
}
