#include "NetParams.h"

#include "NetlistTranslator.h"
#include "Units.h"

#include <QRegularExpression>

#include <cmath>

namespace netparams {

namespace {

using C = std::complex<double>;

QString firstToken(const QString &line)
{
    return line.trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
}

double number(const QString &v, double def)
{
    QString t = v.trimmed();
    if (t.startsWith('{') && t.endsWith('}'))
        t = t.mid(1, t.size() - 2);
    return units::parseSpiceNumber(t).value_or(def);
}

// Joins "+" continuation lines.
QStringList logical(const QStringList &lines)
{
    QStringList out;
    for (const QString &l : lines) {
        if (l.startsWith('+') && !out.isEmpty())
            out.last() += ' ' + l.mid(1);
        else
            out << l;
    }
    return out;
}

struct M2 {
    C a, b, c, d; // [[a b], [c d]]
};
M2 mul(const M2 &x, const M2 &y)
{
    return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d};
}
M2 inv(const M2 &m)
{
    const C det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-300) {
        const C nan(std::nan(""), std::nan(""));
        return {nan, nan, nan, nan};
    }
    return {m.d / det, -m.b / det, -m.c / det, m.a / det};
}

} // namespace

QList<Spec> parse(const QStringList &ltLines, QStringList *errors)
{
    const QStringList lines = logical(ltLines);
    QList<Spec> specs;
    auto findElement = [&lines](const QString &name) -> QStringList {
        for (const QString &l : lines)
            if (firstToken(l).compare(name, Qt::CaseInsensitive) == 0)
                return NetlistTranslator::tokenize(l.section(';', 0, 0));
        return {};
    };
    static const QRegularExpression outRe(QStringLiteral("^([VI])\\(([^,)]+)(?:,([^)]+))?\\)$"), QRegularExpression::CaseInsensitiveOption);
    for (const QString &l : lines) {
        const QString t = l.section(';', 0, 0).trimmed();
        if (!t.startsWith(QLatin1String(".net "), Qt::CaseInsensitive) && !t.startsWith(QLatin1String(".net\t"), Qt::CaseInsensitive))
            continue;
        const QStringList tok = NetlistTranslator::tokenize(t);
        if (tok.size() < 3) {
            if (errors)
                *errors << QStringLiteral("Bad .net statement: %1").arg(t);
            continue;
        }
        Spec s;
        s.source = tok[2];
        const QStringList src = findElement(s.source);
        if (src.size() < 3 || !(src[0][0].toUpper() == 'V' || src[0][0].toUpper() == 'I')) {
            if (errors)
                *errors << QStringLiteral(".net: input source %1 not found").arg(s.source);
            continue;
        }
        // A current source "I n+ n- ..." drives current out of n- into the circuit, so that is
        // the port's positive terminal; a voltage source's is n+.
        const bool currentSource = src[0][0].toUpper() == 'I';
        s.inP = currentSource ? src[2] : src[1];
        s.inM = currentSource ? src[1] : src[2];
        std::optional<double> rin, rout;
        for (const QString &p : src)
            if (p.startsWith(QLatin1String("rser="), Qt::CaseInsensitive))
                rin = number(p.mid(5), 1);
        const auto m = outRe.match(tok[1]);
        if (!m.hasMatch()) {
            if (errors)
                *errors << QStringLiteral(".net: cannot read output %1").arg(tok[1]);
            continue;
        }
        if (m.captured(1).toUpper() == QLatin1String("V")) {
            s.outP = m.captured(2).trimmed();
            s.outM = m.captured(3).trimmed().isEmpty() ? QStringLiteral("0") : m.captured(3).trimmed();
        } else {
            s.outResistor = m.captured(2).trimmed();
            const QStringList r = findElement(s.outResistor);
            if (r.size() < 4) {
                if (errors)
                    *errors << QStringLiteral(".net: output element %1 not found").arg(s.outResistor);
                continue;
            }
            s.outP = r[1];
            s.outM = r[2];
            rout = number(r[3], 1);
        }
        for (int i = 3; i < tok.size(); ++i) {
            const QString lt = tok[i].toLower();
            if (lt.startsWith(QLatin1String("rin=")))
                rin = number(tok[i].mid(4), 1);
            else if (lt.startsWith(QLatin1String("rout=")))
                rout = number(tok[i].mid(5), 1);
        }
        s.rin = rin.value_or(1.0);
        s.rout = rout.value_or(1.0);
        specs << s;
    }
    return specs;
}

QStringList excitationNetlist(const QStringList &ltLines, const Spec &spec, int port)
{
    static const QRegularExpression acRe(QStringLiteral("(\\bac\\s+)(\\{[^}]*\\}|\\S+)"), QRegularExpression::CaseInsensitiveOption);
    auto num = [](double v) { return QString::number(v, 'g', 15); };
    QStringList out;
    for (const QString &l : logical(ltLines)) {
        const QString t = l.trimmed();
        const QString lt = t.toLower();
        if (lt.startsWith(QLatin1String(".net")) || lt.startsWith(QLatin1String(".meas")) || lt.startsWith(QLatin1String(".step"))
            || lt.startsWith(QLatin1String(".four")) || lt.startsWith(QLatin1String(".wave")))
            continue;
        const QString name = firstToken(t);
        if (name.compare(spec.source, Qt::CaseInsensitive) == 0) {
            if (port == 1) {
                out << QStringLiteral("V__netd n__netd %1 AC 1").arg(spec.inM);
                out << QStringLiteral("R__netd n__netd %1 %2").arg(spec.inP, num(spec.rin));
            } else {
                out << QStringLiteral("R__netsrc %1 %2 %3").arg(spec.inP, spec.inM, num(spec.rin));
            }
            continue;
        }
        if (!spec.outResistor.isEmpty() && name.compare(spec.outResistor, Qt::CaseInsensitive) == 0)
            continue; // replaced below
        if (!name.isEmpty() && (name[0].toUpper() == 'V' || name[0].toUpper() == 'I')) {
            // Other sources keep their DC/transient values; their AC excitation is removed.
            QString zeroed = l;
            zeroed.replace(acRe, QStringLiteral("\\1 0"));
            out << zeroed;
            continue;
        }
        out << l;
    }
    // Port 2: the termination (port 1 driven) or the drive behind the termination.
    QStringList extra;
    if (port == 1) {
        extra << QStringLiteral("R__netterm %1 %2 %3").arg(spec.outP, spec.outM, num(spec.rout));
    } else {
        extra << QStringLiteral("V__netd2 n__netd2 %1 AC 1").arg(spec.outM);
        extra << QStringLiteral("R__netd2 n__netd2 %1 %2").arg(spec.outP, num(spec.rout));
    }
    int insertAt = int(out.size());
    for (int i = 0; i < out.size(); ++i)
        if (out[i].trimmed().compare(QLatin1String(".end"), Qt::CaseInsensitive) == 0)
            insertAt = i;
    for (int i = 0; i < extra.size(); ++i)
        out.insert(insertAt + i, extra[i]);
    return out;
}

QStringList measuredSignals(const Spec &spec)
{
    auto v = [](const QString &p, const QString &m) {
        return m == QLatin1String("0") ? QStringLiteral("V(%1)").arg(p) : QStringLiteral("V(%1,%2)").arg(p, m);
    };
    return {v(spec.inP, spec.inM), v(spec.outP, spec.outM)};
}

PortData portData(const Spec &spec, int port, const QVector<C> &v1, const QVector<C> &v2)
{
    PortData d;
    d.v1 = v1;
    d.v2 = v2;
    const int n = std::min(v1.size(), v2.size());
    for (int k = 0; k < n; ++k) {
        // Currents into the network: through the 1V drive's resistance, or out into a termination.
        d.i1 << (port == 1 ? (C(1) - v1[k]) / spec.rin : -v1[k] / spec.rin);
        d.i2 << (port == 2 ? (C(1) - v2[k]) / spec.rout : -v2[k] / spec.rout);
    }
    return d;
}

Result compute(const Spec &spec, const PortData &a, const PortData &b)
{
    Result r;
    const QString s = QStringLiteral("(%1)").arg(spec.source);
    const char *names[] = {"S11", "S21", "S12", "S22", "Z11", "Z21", "Z12", "Z22", "Y11", "Y21", "Y12", "Y22",
                           "H11", "H21", "H12", "H22", "Zout", "Yout", "Zin", "Yin"};
    const char *types[] = {"S-parameter", "S-parameter", "S-parameter", "S-parameter", "impedance", "impedance", "impedance",
                           "impedance", "conductance", "conductance", "conductance", "conductance", "impedance", "H-parameter",
                           "H-parameter", "conductance", "impedance", "conductance", "impedance", "conductance"};
    for (int i = 0; i < 20; ++i) {
        r.names << QLatin1String(names[i]) + s;
        r.types << QLatin1String(types[i]);
        r.values << QVector<C>();
    }
    const int n = std::min({a.v1.size(), a.v2.size(), b.v1.size(), b.v2.size()});
    const double z1 = spec.rin, z2 = spec.rout;
    for (int k = 0; k < n; ++k) {
        const M2 V{a.v1[k], b.v1[k], a.v2[k], b.v2[k]};
        const M2 I{a.i1[k], b.i1[k], a.i2[k], b.i2[k]};
        const M2 Z = mul(V, inv(I));
        const M2 Y = mul(I, inv(V));
        auto wave = [](C v, C i, double z, bool incident) { return (v + (incident ? 1.0 : -1.0) * z * i) / (2 * std::sqrt(z)); };
        const M2 A{wave(a.v1[k], a.i1[k], z1, true), wave(b.v1[k], b.i1[k], z1, true), wave(a.v2[k], a.i2[k], z2, true),
                   wave(b.v2[k], b.i2[k], z2, true)};
        const M2 B{wave(a.v1[k], a.i1[k], z1, false), wave(b.v1[k], b.i1[k], z1, false), wave(a.v2[k], a.i2[k], z2, false),
                   wave(b.v2[k], b.i2[k], z2, false)};
        const M2 S = mul(B, inv(A));
        const M2 H = mul(M2{a.v1[k], b.v1[k], a.i2[k], b.i2[k]}, inv(M2{a.i1[k], b.i1[k], a.v2[k], b.v2[k]}));
        const C zin = a.v1[k] / a.i1[k], zout = b.v2[k] / b.i2[k];
        const C vals[] = {S.a, S.c, S.b, S.d, Z.a, Z.c, Z.b, Z.d, Y.a, Y.c, Y.b, Y.d, H.a, H.c, H.b, H.d, zout, C(1) / zout, zin, C(1) / zin};
        for (int i = 0; i < 20; ++i)
            r.values[i] << vals[i];
    }
    return r;
}

} // namespace netparams
