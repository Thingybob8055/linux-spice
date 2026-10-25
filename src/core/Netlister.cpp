#include "Netlister.h"

#include "DirCache.h"
#include "SymbolLibrary.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace {

class UnionFind
{
public:
    int add()
    {
        m_parent.append(m_parent.size());
        return m_parent.size() - 1;
    }
    int find(int x)
    {
        while (m_parent[x] != x) {
            m_parent[x] = m_parent[m_parent[x]];
            x = m_parent[x];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            m_parent[std::max(a, b)] = std::min(a, b);
    }

private:
    QList<int> m_parent;
};

} // namespace

bool looksLikeModelFile(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("\\.(sub|lib|mod|cir|inc|txt|sp|spi|spice|ckt|net|bjt|dio|mos|jft|subckt)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(s.trimmed()).hasMatch();
}

Netlister::Netlister(const Schematic &sch)
    : m_sch(sch)
{
}

QString Netlister::spiceNameFor(const SymbolInstance &s)
{
    const QString prefix = s.attr(QStringLiteral("Prefix")).trimmed();
    QString inst = s.instName().trimmed();
    if (prefix.isEmpty())
        return inst;
    const QChar letter = prefix[0].toUpper();
    if (inst.isEmpty())
        return QString(letter) + QStringLiteral("§?");
    if (inst[0].toUpper() == letter)
        return inst;
    if (letter == 'X')
        return QStringLiteral("X") + inst;
    return QString(letter) + QStringLiteral("§") + inst;
}

bool Netlister::isHierarchicalBlock(const SymbolInstance &s, const QString &schDir, QString *ascPath)
{
    if (!s.def)
        return false;
    // Blocks without a SPICE prefix that have a same-named schematic beside them
    // (or beside the parent schematic) are hierarchical.
    if (!s.attr(QStringLiteral("Prefix")).isEmpty() && s.attr(QStringLiteral("Prefix")).compare(QLatin1String("X"), Qt::CaseInsensitive) != 0)
        return false;
    if (!s.attr(QStringLiteral("SpiceModel")).isEmpty() || !s.attr(QStringLiteral("ModelFile")).isEmpty())
        return false;
    const QString base = QFileInfo(normalizeSymbolName(s.symbolName)).fileName();
    QStringList dirs;
    if (!s.def->filePath.startsWith(':'))
        dirs << QFileInfo(s.def->filePath).absolutePath();
    if (!schDir.isEmpty())
        dirs << schDir;
    for (const QString &d : dirs) {
        const QString f = DirCache::match(d, base + QStringLiteral(".asc"));
        if (!f.isEmpty()) {
            if (ascPath)
                *ascPath = d + '/' + f;
            return true;
        }
    }
    return false;
}

Connectivity Netlister::connectivity(const Schematic &sch, QStringList *warnings)
{
    Connectivity c;
    UnionFind uf;
    QHash<qint64, int> pointId;
    auto idFor = [&](QPoint p) {
        const qint64 k = Connectivity::key(p);
        auto it = pointId.constFind(k);
        if (it != pointId.constEnd())
            return it.value();
        const int id = uf.add();
        pointId.insert(k, id);
        return id;
    };

    for (const Wire &w : sch.wires)
        uf.unite(idFor(w.a), idFor(w.b));
    QList<QList<QPoint>> pinPos;
    for (const SymbolInstance &s : sch.symbols) {
        pinPos << s.pinPositions();
        for (const QPoint &p : pinPos.last())
            idFor(p);
    }
    for (const NetFlag &f : sch.flags)
        idFor(f.pos);
    for (const DataFlag &d : sch.dataFlags)
        idFor(d.pos);

    // T-junctions: any point that lies in the interior of a wire joins that wire.
    const QList<qint64> keys = pointId.keys();
    for (const Wire &w : sch.wires) {
        const QRect bb = rectFromPoints(w.a, w.b);
        const int wid = pointId.value(Connectivity::key(w.a));
        for (qint64 k : keys) {
            const QPoint p(int(k >> 32), int(qint32(k & 0xffffffff)));
            if (!bb.contains(p))
                continue;
            if (p != w.a && p != w.b && pointOnSegment(p, w.a, w.b))
                uf.unite(pointId.value(k), wid);
        }
    }

    // Flags with the same name join their nets.
    QHash<QString, int> byName;
    for (const NetFlag &f : sch.flags) {
        if (f.name.isEmpty())
            continue;
        const QString n = f.name.toLower();
        const int id = pointId.value(Connectivity::key(f.pos));
        if (byName.contains(n))
            uf.unite(byName.value(n), id);
        else
            byName.insert(n, id);
    }

    // Name each root.
    QHash<int, QString> rootName;
    for (const NetFlag &f : sch.flags) {
        if (f.name.isEmpty())
            continue;
        const int r = uf.find(pointId.value(Connectivity::key(f.pos)));
        auto it = rootName.find(r);
        if (it == rootName.end()) {
            rootName.insert(r, f.name);
        } else if (it.value().compare(f.name, Qt::CaseInsensitive) != 0) {
            // Ground wins; otherwise keep the first and warn.
            if (f.isGround()) {
                if (warnings)
                    warnings->append(QStringLiteral("Net \"%1\" is shorted to ground").arg(it.value()));
                it.value() = f.name;
            } else if (warnings && it.value() != QLatin1String("0")) {
                warnings->append(QStringLiteral("Nets \"%1\" and \"%2\" are shorted together").arg(it.value(), f.name));
            }
        }
    }
    int autoIndex = 0;
    QSet<QString> used;
    for (const QString &n : rootName)
        used.insert(n.toLower());
    auto autoName = [&]() {
        QString n;
        do {
            n = QStringLiteral("N%1").arg(++autoIndex, 3, 10, QChar('0'));
        } while (used.contains(n.toLower()));
        used.insert(n.toLower());
        return n;
    };

    // Size of each net (distinct points) and number of pins at each point,
    // used to detect pins that touch nothing at all.
    QHash<int, int> rootSize;
    for (auto it = pointId.constBegin(); it != pointId.constEnd(); ++it)
        rootSize[uf.find(it.value())]++;
    QHash<qint64, int> pinsAtPoint;
    for (const auto &pins : pinPos)
        for (const QPoint &p : pins)
            pinsAtPoint[Connectivity::key(p)]++;

    // Auto names are assigned in symbol order so that netlists are stable.
    c.symbolPinNets.reserve(sch.symbols.size());
    int ncIndex = 0;
    for (int i = 0; i < sch.symbols.size(); ++i) {
        QStringList nets;
        for (const QPoint &p : pinPos[i]) {
            const qint64 k = Connectivity::key(p);
            const int r = uf.find(pointId.value(k));
            if (!rootName.contains(r)) {
                // A pin touching nothing else gets a unique "NC_xx" name.
                if (rootSize.value(r) <= 1 && pinsAtPoint.value(k) <= 1)
                    rootName.insert(r, QStringLiteral("NC_%1").arg(++ncIndex, 2, 10, QChar('0')));
                else
                    rootName.insert(r, autoName());
            }
            nets << rootName.value(r);
        }
        c.symbolPinNets << nets;
    }
    for (const Wire &w : sch.wires) {
        const int r = uf.find(pointId.value(Connectivity::key(w.a)));
        if (!rootName.contains(r))
            rootName.insert(r, autoName());
        c.wireNet << rootName.value(r);
    }
    for (const NetFlag &f : sch.flags) {
        const int r = uf.find(pointId.value(Connectivity::key(f.pos)));
        if (!rootName.contains(r))
            rootName.insert(r, autoName());
        c.flagNet << rootName.value(r);
    }
    for (auto it = pointId.constBegin(); it != pointId.constEnd(); ++it) {
        const int r = uf.find(it.value());
        if (rootName.contains(r))
            c.pointNet.insert(it.key(), rootName.value(r));
    }
    QSet<QString> seen;
    for (const QString &n : rootName) {
        if (!seen.contains(n)) {
            seen.insert(n);
            c.netNames << n;
        }
    }
    return c;
}

QStringList Netlister::sheetBody(const Schematic &sch, const Connectivity &conn, bool topLevel, QList<QString> *names)
{
    QStringList lines;
    const QString schDir = sch.directory();
    QSet<QString> usedNames;

    for (int i = 0; i < sch.symbols.size(); ++i) {
        const SymbolInstance &s = sch.symbols[i];
        if (names)
            names->append(QString());
        if (!s.def) {
            m_result.errors << QStringLiteral("Could not find symbol \"%1\"").arg(s.symbolName);
            continue;
        }
        QString ascPath;
        const bool hier = isHierarchicalBlock(s, schDir, &ascPath);
        QString prefix = s.attr(QStringLiteral("Prefix")).trimmed();
        if (prefix.isEmpty() && !hier)
            continue; // purely graphical symbol

        QString name;
        if (hier) {
            const QString inst = s.instName().isEmpty() ? QStringLiteral("X?") : s.instName();
            name = inst.startsWith('X', Qt::CaseInsensitive) ? inst : QStringLiteral("X§") + inst;
        } else {
            name = spiceNameFor(s);
        }
        if (usedNames.contains(name.toLower()))
            m_result.errors << QStringLiteral("Duplicate instance name \"%1\"").arg(name);
        usedNames.insert(name.toLower());
        if (names)
            (*names)[i] = name;

        // Pins in SPICE order.
        QStringList nodes;
        QList<int> order;
        for (int k = 0; k < s.def->pins.size(); ++k)
            order << k;
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return s.def->pins[a].spiceOrder < s.def->pins[b].spiceOrder; });
        const bool special = prefix.startsWith('A', Qt::CaseInsensitive);
        if (special) {
            // Special-function (A) devices always have 8 terminals addressed by
            // SpiceOrder; absent or unconnected terminals are tied to ground,
            // which those devices treat as "unused".
            QStringList pinSlots(8, QStringLiteral("0"));
            for (int k : order) {
                const int so = s.def->pins[k].spiceOrder;
                QString net = conn.symbolPinNets.value(i).value(k, QStringLiteral("0"));
                if (net.startsWith(QLatin1String("NC_")))
                    net = QStringLiteral("0");
                if (so >= 1 && so <= 8)
                    pinSlots[so - 1] = net;
            }
            nodes = pinSlots;
        } else {
            // Gaps in SpiceOrder (e.g. 1,2,3,5,...) become unique unconnected
            // nodes so the port count still matches the subcircuit.
            int expected = 1;
            for (int k : order) {
                const int so = s.def->pins[k].spiceOrder;
                while (so > expected && expected < 256) {
                    nodes << QStringLiteral("NC_%1_%2").arg(name.section(QChar(0xA7), -1)).arg(expected);
                    ++expected;
                }
                nodes << conn.symbolPinNets.value(i).value(k, QStringLiteral("NC"));
                expected = so + 1;
            }
        }

        QStringList tokens;
        tokens << name << nodes;
        if (hier) {
            const QString block = QFileInfo(ascPath).completeBaseName();
            tokens << block;
            const QString params = s.attr(QStringLiteral("SpiceLine")).trimmed();
            if (!params.isEmpty())
                tokens << params;
            QStringList ports;
            for (int k : order)
                ports << s.def->pins[k].name;
            if (!m_doneSubckts.contains(block.toLower())) {
                m_doneSubckts.insert(block.toLower());
                m_pendingSubckts.append({block, ascPath, ports});
            }
            lines << tokens.join(' ');
            continue;
        }

        const QString spiceModel = s.attr(QStringLiteral("SpiceModel")).trimmed();
        const QString value = s.attr(QStringLiteral("Value")).trimmed();
        const QString value2 = s.attr(QStringLiteral("Value2")).trimmed();
        const QString spiceLine = s.attr(QStringLiteral("SpiceLine")).trimmed();
        const QString spiceLine2 = s.attr(QStringLiteral("SpiceLine2")).trimmed();
        const QString modelFile = s.attr(QStringLiteral("ModelFile")).trimmed();

        const bool modelIsFile = looksLikeModelFile(spiceModel);
        if (modelIsFile && !m_libLines.contains(spiceModel))
            m_libLines << spiceModel;
        if (!modelFile.isEmpty() && !m_libLines.contains(modelFile))
            m_libLines << modelFile;

        const bool isSub = prefix.startsWith('X', Qt::CaseInsensitive);
        if (!spiceModel.isEmpty() && !modelIsFile)
            tokens << spiceModel;
        if (isSub && !value2.isEmpty() && (modelIsFile || value == value2)) {
            tokens << value2;
        } else {
            if (!value.isEmpty())
                tokens << value;
            if (!value2.isEmpty())
                tokens << value2;
        }
        if (!spiceLine.isEmpty())
            tokens << spiceLine;
        if (!spiceLine2.isEmpty())
            tokens << spiceLine2;
        lines << tokens.join(' ');
    }

    for (const QString &n : conn.netNames)
        if (n.startsWith(QLatin1String("$G_"), Qt::CaseInsensitive))
            m_globals.insert(n);

    // Directives from TEXT elements.
    // Continuation lines ("+ ...") stay attached to the line they continue.
    auto appendDirective = [&lines](const QString &l, QStringList &block) {
        if (l.trimmed().startsWith('+') && !block.isEmpty())
            block.last() += ' ' + l.trimmed().mid(1).trimmed();
        else
            block << l;
        Q_UNUSED(lines);
    };
    if (topLevel) {
        // Directive blocks keep their exact line order (they may contain
        // complete .subckt definitions with element lines).
        for (const SchText &t : sch.texts) {
            if (!t.directive)
                continue;
            QStringList block;
            for (const QString &l : t.text.split('\n'))
                appendDirective(l, block);
            m_topDirectives << block;
        }
    } else {
        for (const SchText &t : sch.texts) {
            if (!t.directive)
                continue;
            QStringList block;
            for (const QString &l : t.text.split('\n'))
                appendDirective(l, block);
            for (const QString &l : block) {
                const QString lt = l.trimmed().toLower();
                // Analysis commands do not belong inside a subcircuit.
                if (lt.startsWith(QLatin1String(".tran")) || lt.startsWith(QLatin1String(".ac")) || lt.startsWith(QLatin1String(".dc"))
                    || lt.startsWith(QLatin1String(".op")) || lt.startsWith(QLatin1String(".noise"))
                    || lt.startsWith(QLatin1String(".tf")) || lt.startsWith(QLatin1String(".step")))
                    continue;
                lines << l;
            }
        }
    }
    return lines;
}

void Netlister::emitSubckts()
{
    while (!m_pendingSubckts.isEmpty()) {
        const SubcktJob job = m_pendingSubckts.takeFirst();
        Schematic sub;
        QString err;
        if (!sub.load(job.ascPath, &err)) {
            m_result.errors << QStringLiteral("Cannot load hierarchical block %1: %2").arg(job.ascPath, err);
            continue;
        }
        QStringList warn;
        const Connectivity conn = connectivity(sub, &warn);
        // Ports: the nets carrying the IOPIN/flag names.
        QStringList ports;
        for (const QString &p : job.ports) {
            QString net = p;
            for (const QString &n : conn.netNames)
                if (n.compare(p, Qt::CaseInsensitive) == 0)
                    net = n;
            ports << net;
        }
        m_subcktText << QStringLiteral(".subckt %1 %2").arg(job.name, ports.join(' '));
        m_subcktText << sheetBody(sub, conn, false, nullptr);
        m_subcktText << QStringLiteral(".ends %1").arg(job.name);
    }
}

NetlistResult Netlister::build()
{
    m_result = NetlistResult();
    m_topDirectives.clear();
    m_result.conn = connectivity(m_sch, &m_result.warnings);

    QStringList out;
    out << QStringLiteral("* %1").arg(m_sch.filePath.isEmpty() ? QStringLiteral("untitled.asc") : QDir::toNativeSeparators(m_sch.filePath));
    QStringList body = sheetBody(m_sch, m_result.conn, true, &m_result.symbolSpiceName);
    emitSubckts();

    // Element lines first, then subcircuits, then directives in schematic order.
    out << body;
    out << m_subcktText;
    out << m_topDirectives;
    for (const QString &lib : m_libLines)
        out << QStringLiteral(".lib %1").arg(lib);
    if (!m_globals.isEmpty())
        out << QStringLiteral(".global %1").arg(QStringList(m_globals.values()).join(' '));
    out << QStringLiteral(".backanno");
    out << QStringLiteral(".end");
    m_result.text = out.join('\n') + '\n';
    m_result.libFiles = m_libLines;
    return m_result;
}
