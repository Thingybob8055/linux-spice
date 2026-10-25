#include "Measure.h"

#include "Units.h"

#include <QRegularExpression>

#include <cmath>

// ---------------------------------------------------------------- DatasetContext

DatasetContext::DatasetContext(const Dataset &ds, int run)
    : m_ds(ds)
    , m_run(run)
{
}

std::optional<Value> DatasetContext::vector(int var, int run) const
{
    if (var < 0 || run < 0 || run >= m_ds.runs.size())
        return std::nullopt;
    const Dataset::Run &r = m_ds.runs[run];
    Value v;
    v.re = r.re[var];
    if (m_ds.complex && var > 0 && !r.im.isEmpty()) {
        v.im = r.im[var];
        v.complex = true;
    }
    v.unit = Unit::fromSymbol(m_ds.unitFor(var));
    if (m_ds.unitFor(var).isEmpty() || !v.unit.known)
        v.unit = Unit::unknown();
    if (m_ds.unitFor(var).isEmpty())
        v.unit = Unit();
    return v;
}

std::optional<Value> DatasetContext::nodeVoltage(const QString &node, int run) const
{
    const QString n = node.trimmed();
    if (n == QLatin1String("0") || n.compare(QLatin1String("gnd"), Qt::CaseInsensitive) == 0) {
        Value z;
        z.re = QVector<double>(m_ds.pointCount(run), 0.0);
        if (m_ds.complex) {
            z.im = z.re;
            z.complex = true;
        }
        z.unit = Unit::volt();
        return z;
    }
    const int idx = m_ds.varIndex(QStringLiteral("V(%1)").arg(n));
    if (idx >= 0)
        return vector(idx, run);
    const int raw = m_ds.varIndex(n);
    if (raw >= 0)
        return vector(raw, run);
    return std::nullopt;
}

std::optional<Value> DatasetContext::signal(const QString &name, int step)
{
    const int run = step >= 0 ? step : m_run;
    if (run >= m_ds.runs.size())
        return std::nullopt;
    const int direct = m_ds.varIndex(name);
    if (direct >= 0)
        return vector(direct, run);

    static const QRegularExpression re(QStringLiteral("^(\\w+)\\((.*)\\)$"));
    const auto m = re.match(name.trimmed());
    if (!m.hasMatch())
        return std::nullopt;
    const QString fn = m.captured(1).toLower();
    const QString inner = m.captured(2);
    if (fn == QLatin1String("v") || fn == QLatin1String("vm") || fn == QLatin1String("vdb") || fn == QLatin1String("vp")
        || fn == QLatin1String("vr") || fn == QLatin1String("vi")) {
        const QStringList nodes = inner.split(',');
        std::optional<Value> a = nodeVoltage(nodes.value(0), run);
        if (!a)
            return std::nullopt;
        if (nodes.size() > 1) {
            std::optional<Value> b = nodeVoltage(nodes.value(1), run);
            if (!b)
                return std::nullopt;
            for (int k = 0; k < a->size(); ++k) {
                a->re[k] -= b->re[k];
                if (a->complex)
                    a->im[k] -= b->i(k);
            }
        }
        if (fn == QLatin1String("v"))
            return a;
        Value r;
        r.re.resize(a->size());
        r.unit = fn == QLatin1String("vdb") || fn == QLatin1String("vp") ? Unit() : Unit::volt();
        for (int k = 0; k < a->size(); ++k) {
            const std::complex<double> z = a->c(k);
            if (fn == QLatin1String("vm")) r.re[k] = std::abs(z);
            else if (fn == QLatin1String("vdb")) r.re[k] = 20 * std::log10(std::abs(z));
            else if (fn == QLatin1String("vp")) r.re[k] = std::arg(z) * 180 / M_PI;
            else if (fn == QLatin1String("vr")) r.re[k] = z.real();
            else r.re[k] = z.imag();
        }
        return r;
    }
    // Currents: try exact spellings with different case of the device part.
    for (int i = 0; i < m_ds.vars.size(); ++i)
        if (m_ds.vars[i].name.compare(name, Qt::CaseInsensitive) == 0)
            return vector(i, run);
    // "I(Q1)" falls back to the collector current, "I(M1)" to the drain current.
    if (fn == QLatin1String("i")) {
        for (const char *alt : {"Ic", "Id", "Ix"}) {
            const int k = m_ds.varIndex(QStringLiteral("%1(%2)").arg(QLatin1String(alt), inner));
            if (k >= 0)
                return vector(k, run);
        }
    }
    return std::nullopt;
}

std::optional<Value> DatasetContext::variable(const QString &name, int step)
{
    const QString l = name.toLower();
    auto it = params.constFind(l);
    if (it != params.constEnd())
        return it.value();
    const int run = step >= 0 ? step : m_run;
    if (m_ds.isEmpty() || run >= m_ds.runs.size())
        return std::nullopt;
    if (l == m_ds.xName().toLower() || l == QLatin1String("time") || l == QLatin1String("freq")
        || l == QLatin1String("frequency")) {
        if (l == QLatin1String("time") && m_ds.kind != Dataset::Kind::Transient)
            return std::nullopt;
        if ((l == QLatin1String("freq") || l == QLatin1String("frequency")) && m_ds.kind != Dataset::Kind::AC
            && m_ds.kind != Dataset::Kind::Noise && m_ds.kind != Dataset::Kind::FFT)
            return std::nullopt;
        Value v;
        v.re = m_ds.runs[run].re[0];
        v.unit = Unit::fromSymbol(m_ds.xUnit());
        return v;
    }
    if (l == QLatin1String("omega") || l == QLatin1String("w")) {
        if (m_ds.kind != Dataset::Kind::AC)
            return std::nullopt;
        Value v;
        v.re = m_ds.runs[run].re[0];
        for (double &x : v.re)
            x *= 2 * M_PI;
        v.unit = Unit::hertz();
        return v;
    }
    const int idx = m_ds.varIndex(name);
    if (idx >= 0)
        return vector(idx, run);
    return std::nullopt;
}

std::optional<Value> DatasetContext::xAxis(int step)
{
    const int run = step >= 0 ? step : m_run;
    if (m_ds.isEmpty() || run >= m_ds.runs.size())
        return std::nullopt;
    Value v;
    v.re = m_ds.runs[run].re[0];
    v.unit = Unit::fromSymbol(m_ds.xUnit());
    return v;
}

// ---------------------------------------------------------------- statement parsing

namespace {

struct Segment {
    QString kw;   // upper case keyword ("" for the leading part)
    QString text; // argument text (after optional '=')
};

const QStringList &measKeywords()
{
    static const QStringList kws = {QStringLiteral("FIND"), QStringLiteral("WHEN"), QStringLiteral("AT"), QStringLiteral("DERIV"),
                                    QStringLiteral("AVG"), QStringLiteral("MAX"), QStringLiteral("MIN"), QStringLiteral("PP"),
                                    QStringLiteral("RMS"), QStringLiteral("INTEG"), QStringLiteral("FROM"), QStringLiteral("TO"),
                                    QStringLiteral("TRIG"), QStringLiteral("TARG"), QStringLiteral("TD"), QStringLiteral("RISE"),
                                    QStringLiteral("FALL"), QStringLiteral("CROSS"), QStringLiteral("PARAM"), QStringLiteral("VAL"),
                                    QStringLiteral("DERIVATIVE"), QStringLiteral("INTEGRAL"), QStringLiteral("AVERAGE")};
    return kws;
}

QList<Segment> splitSegments(const QString &s)
{
    QList<Segment> out;
    out.append({QString(), QString()});
    int depth = 0;
    int i = 0;
    const int n = s.size();
    int segStart = 0;
    auto closeSeg = [&](int end) { out.last().text = s.mid(segStart, end - segStart).trimmed(); };
    while (i < n) {
        const QChar c = s[i];
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;
        if (depth == 0 && c.isLetter() && (i == 0 || s[i - 1].isSpace() || s[i - 1] == '=' || s[i - 1] == ')')) {
            int j = i;
            while (j < n && s[j].isLetter())
                ++j;
            const QString word = s.mid(i, j - i).toUpper();
            int k = j;
            while (k < n && s[k].isSpace())
                ++k;
            const bool boundary = j >= n || s[j].isSpace() || s[j] == '=';
            if (boundary && measKeywords().contains(word)) {
                // "AT" etc must not be part of an expression like "V(at)"; we are at depth 0 so ok.
                closeSeg(i);
                if (k < n && s[k] == '=' && (k + 1 >= n || s[k + 1] != '='))
                    ++k;
                out.append({word, QString()});
                segStart = k;
                i = k;
                continue;
            }
            i = j;
            continue;
        }
        ++i;
    }
    closeSeg(n);
    return out;
}

// Split "lhs=rhs" at a top-level single '='. Returns false if none.
bool splitEquation(const QString &s, QString *lhs, QString *rhs)
{
    int depth = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '(' || c == '{')
            ++depth;
        else if (c == ')' || c == '}')
            --depth;
        else if (c == '=' && depth == 0) {
            const QChar prev = i > 0 ? s[i - 1] : QChar();
            const QChar next = i + 1 < s.size() ? s[i + 1] : QChar();
            if (next == '=' || prev == '=' || prev == '<' || prev == '>' || prev == '!')
                continue;
            *lhs = s.left(i).trimmed();
            *rhs = s.mid(i + 1).trimmed();
            return true;
        }
    }
    return false;
}

double realOf(const Value &v, int k)
{
    return v.complex ? std::abs(v.c(k)) : v.r(k);
}

// Linear interpolation of a value at x.
std::complex<double> interpAt(const QVector<double> &xs, const Value &v, double x, bool *ok)
{
    *ok = false;
    const int n = xs.size();
    if (n == 0 || v.size() == 0)
        return {};
    if (v.isScalar()) {
        *ok = true;
        return v.c(0);
    }
    const bool ascending = xs.last() >= xs.first();
    if ((ascending && (x < xs.first() || x > xs.last())) || (!ascending && (x > xs.first() || x < xs.last())))
        return {};
    for (int k = 1; k < n; ++k) {
        const double x0 = xs[k - 1], x1 = xs[k];
        if ((x >= std::min(x0, x1)) && (x <= std::max(x0, x1))) {
            *ok = true;
            if (x1 == x0)
                return v.c(k);
            const double t = (x - x0) / (x1 - x0);
            return v.c(k - 1) + t * (v.c(k) - v.c(k - 1));
        }
    }
    *ok = true;
    return v.c(n - 1);
}

struct CrossSpec {
    enum Mode { Cross, Rise, Fall };
    Mode mode = Cross;
    int count = 1;
    bool last = false;
    double td = 0.0;
};

// Find the x where f (=lhs-rhs) crosses zero per spec.
std::optional<double> findCrossing(const QVector<double> &xs, const Value &f, const CrossSpec &spec)
{
    QList<double> hits;
    const int n = xs.size();
    for (int k = 1; k < n; ++k) {
        if (xs[k] < spec.td)
            continue;
        const double a = realOf(f, k - 1), b = realOf(f, k);
        bool rise = a < 0 && b >= 0;
        bool fall = a > 0 && b <= 0;
        if (!(rise || fall))
            continue;
        if (spec.mode == CrossSpec::Rise && !rise)
            continue;
        if (spec.mode == CrossSpec::Fall && !fall)
            continue;
        const double t = (a == b) ? 0.0 : a / (a - b);
        const double x = xs[k - 1] + t * (xs[k] - xs[k - 1]);
        if (x < spec.td)
            continue;
        hits << x;
        if (!spec.last && hits.size() >= spec.count)
            return hits.last();
    }
    if (spec.last && !hits.isEmpty())
        return hits.last();
    // A waveform exactly equal at the first point counts as the first crossing.
    if (!spec.last && spec.count == 1 && hits.isEmpty() && n > 0 && realOf(f, 0) == 0.0 && xs[0] >= spec.td)
        return xs[0];
    return std::nullopt;
}

QString formatComplex(std::complex<double> z, MeasureEngine::ComplexFormat fmt)
{
    switch (fmt) {
    case MeasureEngine::ComplexFormat::Cartesian:
        return QStringLiteral("(%1,%2)").arg(formatMeasNumber(z.real()), formatMeasNumber(z.imag()));
    case MeasureEngine::ComplexFormat::Polar:
        return QStringLiteral("(%1,%2°)").arg(formatMeasNumber(std::abs(z)), formatMeasNumber(std::arg(z) * 180 / M_PI));
    case MeasureEngine::ComplexFormat::DbDegrees:
    default:
        return QStringLiteral("(%1dB,%2°)").arg(formatMeasNumber(20 * std::log10(std::abs(z))), formatMeasNumber(std::arg(z) * 180 / M_PI));
    }
}

} // namespace

QString formatMeasNumber(double v)
{
    if (std::isnan(v))
        return QStringLiteral("NaN");
    return QString::number(v, 'g', 12);
}

QStringList MeasureEngine::extractMeasures(const QStringList &lines)
{
    QStringList out;
    QString cur;
    bool inMeas = false;
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        if (l.startsWith('+') && inMeas) {
            cur += ' ' + l.mid(1).trimmed();
            continue;
        }
        if (inMeas) {
            out << cur;
            inMeas = false;
        }
        if (l.startsWith(QLatin1String(".meas"), Qt::CaseInsensitive)) {
            cur = l;
            inMeas = true;
        }
    }
    if (inMeas)
        out << cur;
    return out;
}

QString MeasureEngine::measAnalysis(const QString &stmt)
{
    const QStringList t = stmt.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (t.size() < 2)
        return QString();
    const QString a = t[1].toUpper();
    static const QStringList analyses = {QStringLiteral("TRAN"), QStringLiteral("AC"), QStringLiteral("DC"), QStringLiteral("OP"),
                                         QStringLiteral("TF"), QStringLiteral("NOISE")};
    return analyses.contains(a) ? a : QString();
}

bool MeasureEngine::appliesTo(const QString &stmt, Dataset::Kind kind)
{
    const QString a = measAnalysis(stmt);
    if (a.isEmpty())
        return true;
    switch (kind) {
    case Dataset::Kind::Transient: return a == QLatin1String("TRAN");
    case Dataset::Kind::AC: return a == QLatin1String("AC");
    case Dataset::Kind::DCSweep: return a == QLatin1String("DC");
    case Dataset::Kind::OperatingPoint: return a == QLatin1String("OP");
    case Dataset::Kind::Noise: return a == QLatin1String("NOISE");
    case Dataset::Kind::TransferFunction: return a == QLatin1String("TF");
    default: return true;
    }
}

QList<MeasResult> MeasureEngine::evaluate(const Dataset &ds, int run, const QStringList &statements,
                                          const QHash<QString, double> &params)
{
    QList<MeasResult> results;
    DatasetContext ctx(ds, run);
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        ctx.params.insert(it.key().toLower(), Value::scalar(it.value()));
    for (const QString &stmt : statements) {
        if (!appliesTo(stmt, ds.kind))
            continue;
        MeasResult r = evaluateOne(ds, ctx, stmt);
        if (r.ok)
            ctx.params.insert(r.name, r.complex ? Value::cscalar(r.value) : Value::scalar(r.value.real()));
        results << r;
    }
    return results;
}

MeasResult MeasureEngine::evaluateOne(const Dataset &ds, DatasetContext &ctx, const QString &stmtIn)
{
    MeasResult res;
    QString stmt = stmtIn.trimmed();
    // Strip ".meas[ure]" and optional analysis keyword.
    stmt = stmt.section(QRegularExpression(QStringLiteral("\\s+")), 1);
    QString analysis = measAnalysis(stmtIn);
    res.analysis = analysis;
    if (!analysis.isEmpty())
        stmt = stmt.section(QRegularExpression(QStringLiteral("\\s+")), 1);
    // Name may be followed by '=' or space.
    static const QRegularExpression nameRe(QStringLiteral("^([^\\s=]+)\\s*(.*)$"));
    const auto nm = nameRe.match(stmt);
    if (!nm.hasMatch()) {
        res.logLine = QStringLiteral("Syntax error in: %1").arg(stmtIn);
        return res;
    }
    res.name = nm.captured(1).toLower();
    const QString body = nm.captured(2).trimmed();
    const QList<Segment> segs = splitSegments(body);

    auto segText = [&](const QString &kw) -> QString {
        for (const Segment &s : segs)
            if (s.kw == kw)
                return s.text;
        return QString();
    };
    auto hasSeg = [&](const QString &kw) {
        for (const Segment &s : segs)
            if (s.kw == kw)
                return true;
        return false;
    };
    auto evalExpr = [&](const QString &text, QString *err) -> std::optional<Value> {
        Expression e(text);
        if (!e.isValid()) {
            *err = e.error();
            return std::nullopt;
        }
        auto v = e.evaluate(ctx);
        if (!v)
            *err = e.error();
        return v;
    };
    auto evalNumber = [&](const QString &text, bool *ok) -> double {
        QString err;
        auto v = evalExpr(text, &err);
        *ok = v.has_value() && v->size() >= 1;
        return *ok ? v->r(0) : 0.0;
    };
    auto fail = [&](const QString &why) {
        res.ok = false;
        res.logLine = QStringLiteral("Measurement \"%1\" FAIL'ed%2").arg(res.name, why.isEmpty() ? QString() : QStringLiteral(": ") + why);
        return res;
    };

    const QVector<double> &xs = ds.runs[ctx.run()].re[0];

    // Parses crossing qualifiers that follow a WHEN/TRIG/TARG clause.
    auto crossSpecFrom = [&](const QList<Segment> &list, int startIdx) {
        CrossSpec cs;
        for (int i = startIdx; i < list.size(); ++i) {
            const Segment &s = list[i];
            if (s.kw == QLatin1String("TRIG") || s.kw == QLatin1String("TARG"))
                break;
            if (s.kw == QLatin1String("TD")) {
                bool ok;
                cs.td = evalNumber(s.text, &ok);
            } else if (s.kw == QLatin1String("RISE") || s.kw == QLatin1String("FALL") || s.kw == QLatin1String("CROSS")) {
                cs.mode = s.kw == QLatin1String("RISE") ? CrossSpec::Rise : (s.kw == QLatin1String("FALL") ? CrossSpec::Fall : CrossSpec::Cross);
                if (s.text.compare(QLatin1String("LAST"), Qt::CaseInsensitive) == 0)
                    cs.last = true;
                else
                    cs.count = std::max(1, s.text.toInt());
            }
        }
        return cs;
    };

    // Computes the x location described by a condition "lhs=rhs" (or "expr VAL=x").
    auto locateWhen = [&](const QString &cond, const QString &valText, const CrossSpec &cs, QString *err) -> std::optional<double> {
        QString lhs, rhs;
        if (!splitEquation(cond, &lhs, &rhs)) {
            if (valText.isEmpty()) {
                *err = QStringLiteral("missing '=' in condition");
                return std::nullopt;
            }
            lhs = cond;
            rhs = valText;
        }
        auto a = evalExpr(lhs, err);
        if (!a)
            return std::nullopt;
        auto b = evalExpr(rhs, err);
        if (!b)
            return std::nullopt;
        Value f;
        const int n = xs.size();
        f.re.resize(n);
        for (int k = 0; k < n; ++k)
            f.re[k] = realOf(*a, a->isScalar() ? 0 : k) - realOf(*b, b->isScalar() ? 0 : k);
        auto x = findCrossing(xs, f, cs);
        if (!x)
            *err = QStringLiteral("condition never met");
        return x;
    };

    QString err;
    const QString firstKw = segs.size() > 1 ? segs[1].kw : QString();

    // PARAM expression.
    if (hasSeg(QStringLiteral("PARAM"))) {
        const QString e = segText(QStringLiteral("PARAM"));
        auto v = evalExpr(e, &err);
        if (!v)
            return fail(err);
        res.ok = true;
        res.complex = v->complex;
        res.value = v->c(0);
        res.exprText = e;
        res.logLine = QStringLiteral("%1: %2=%3").arg(res.name, e, v->complex ? formatComplex(res.value, complexFormat) : formatMeasNumber(res.value.real()));
        return res;
    }

    // FIND / DERIV with AT or WHEN.
    if (hasSeg(QStringLiteral("FIND")) || hasSeg(QStringLiteral("DERIV")) || hasSeg(QStringLiteral("DERIVATIVE"))) {
        const bool deriv = !hasSeg(QStringLiteral("FIND"));
        const QString e = deriv ? (hasSeg(QStringLiteral("DERIV")) ? segText(QStringLiteral("DERIV")) : segText(QStringLiteral("DERIVATIVE")))
                                : segText(QStringLiteral("FIND"));
        auto v = evalExpr(deriv ? QStringLiteral("d(%1)").arg(e) : e, &err);
        if (!v)
            return fail(err);
        double x;
        if (hasSeg(QStringLiteral("AT"))) {
            bool ok;
            x = evalNumber(segText(QStringLiteral("AT")), &ok);
            if (!ok)
                return fail(QStringLiteral("bad AT value"));
        } else if (hasSeg(QStringLiteral("WHEN"))) {
            int idx = 0;
            for (int i = 0; i < segs.size(); ++i)
                if (segs[i].kw == QLatin1String("WHEN"))
                    idx = i;
            auto xw = locateWhen(segText(QStringLiteral("WHEN")), segText(QStringLiteral("VAL")), crossSpecFrom(segs, idx + 1), &err);
            if (!xw)
                return fail(err);
            x = *xw;
        } else {
            return fail(QStringLiteral("FIND needs AT or WHEN"));
        }
        bool ok;
        const std::complex<double> val = interpAt(xs, *v, x, &ok);
        if (!ok)
            return fail(QStringLiteral("AT value out of range"));
        res.ok = true;
        res.complex = v->complex;
        res.value = val;
        res.at = x;
        res.exprText = e;
        res.logLine = QStringLiteral("%1: %2=%3 at %4")
                          .arg(res.name, deriv ? QStringLiteral("d(%1)").arg(e) : e,
                               v->complex ? formatComplex(val, complexFormat) : formatMeasNumber(val.real()), formatMeasNumber(x));
        return res;
    }

    // WHEN alone -> x location.
    if (firstKw == QLatin1String("WHEN")) {
        const QString cond = segText(QStringLiteral("WHEN"));
        auto xw = locateWhen(cond, segText(QStringLiteral("VAL")), crossSpecFrom(segs, 2), &err);
        if (!xw)
            return fail(err);
        res.ok = true;
        res.value = *xw;
        res.at = *xw;
        res.exprText = cond;
        res.logLine = QStringLiteral("%1: %2 AT %3").arg(res.name, cond, formatMeasNumber(*xw));
        return res;
    }

    // TRIG ... TARG ... -> delay measurement (unless combined with AVG etc).
    auto rangeFromTrigTarg = [&](double *from, double *to, QString *e) -> bool {
        int trigIdx = -1, targIdx = -1;
        for (int i = 0; i < segs.size(); ++i) {
            if (segs[i].kw == QLatin1String("TRIG")) trigIdx = i;
            if (segs[i].kw == QLatin1String("TARG")) targIdx = i;
        }
        auto locate = [&](int idx, double *out) -> bool {
            const Segment &s = segs[idx];
            // "TRIG AT=x"
            QString valText;
            for (int i = idx + 1; i < segs.size() && segs[i].kw != QLatin1String("TRIG") && segs[i].kw != QLatin1String("TARG"); ++i) {
                if (segs[i].kw == QLatin1String("AT")) {
                    bool ok;
                    *out = evalNumber(segs[i].text, &ok);
                    return ok;
                }
                if (segs[i].kw == QLatin1String("VAL"))
                    valText = segs[i].text;
            }
            auto x = locateWhen(s.text, valText, crossSpecFrom(segs, idx + 1), e);
            if (!x)
                return false;
            *out = *x;
            return true;
        };
        if (trigIdx < 0 || targIdx < 0) {
            *e = QStringLiteral("TRIG and TARG are both required");
            return false;
        }
        return locate(trigIdx, from) && locate(targIdx, to);
    };

    static const QStringList aggregates = {QStringLiteral("AVG"), QStringLiteral("MAX"), QStringLiteral("MIN"), QStringLiteral("PP"),
                                           QStringLiteral("RMS"), QStringLiteral("INTEG"), QStringLiteral("INTEGRAL"), QStringLiteral("AVERAGE")};
    QString agg;
    for (const QString &a : aggregates)
        if (hasSeg(a)) {
            agg = a;
            break;
        }

    if (agg.isEmpty() && hasSeg(QStringLiteral("TRIG"))) {
        double from, to;
        if (!rangeFromTrigTarg(&from, &to, &err))
            return fail(err);
        res.ok = true;
        res.value = to - from;
        res.from = from;
        res.to = to;
        res.exprText = QStringLiteral("TRIG/TARG");
        res.logLine = QStringLiteral("%1=%2 FROM %3 TO %4").arg(res.name, formatMeasNumber(to - from), formatMeasNumber(from), formatMeasNumber(to));
        return res;
    }

    if (!agg.isEmpty()) {
        const QString e = segText(agg);
        auto v = evalExpr(e, &err);
        if (!v)
            return fail(err);
        double from = xs.isEmpty() ? 0 : xs.first(), to = xs.isEmpty() ? 0 : xs.last();
        if (hasSeg(QStringLiteral("TRIG"))) {
            if (!rangeFromTrigTarg(&from, &to, &err))
                return fail(err);
        } else {
            bool ok;
            if (hasSeg(QStringLiteral("FROM"))) {
                from = evalNumber(segText(QStringLiteral("FROM")), &ok);
                if (!ok)
                    return fail(QStringLiteral("bad FROM"));
            }
            if (hasSeg(QStringLiteral("TO"))) {
                to = evalNumber(segText(QStringLiteral("TO")), &ok);
                if (!ok)
                    return fail(QStringLiteral("bad TO"));
            }
        }
        if (from > to)
            std::swap(from, to);
        // Build the sample list inside [from, to] with interpolated endpoints.
        QVector<double> px, py;
        bool okA, okB;
        const std::complex<double> ya = interpAt(xs, *v, from, &okA);
        const std::complex<double> yb = interpAt(xs, *v, to, &okB);
        auto mag = [&](std::complex<double> z) { return v->complex ? std::abs(z) : z.real(); };
        if (okA) {
            px << from;
            py << mag(ya);
        }
        for (int k = 0; k < xs.size(); ++k) {
            if (xs[k] > from && xs[k] < to) {
                px << xs[k];
                py << realOf(*v, v->isScalar() ? 0 : k);
            }
        }
        if (okB && to > from) {
            px << to;
            py << mag(yb);
        }
        if (py.isEmpty())
            return fail(QStringLiteral("empty interval"));
        double result = 0;
        const QString a = agg == QLatin1String("AVERAGE") ? QStringLiteral("AVG") : (agg == QLatin1String("INTEGRAL") ? QStringLiteral("INTEG") : agg);
        if (a == QLatin1String("MAX")) {
            result = *std::max_element(py.begin(), py.end());
        } else if (a == QLatin1String("MIN")) {
            result = *std::min_element(py.begin(), py.end());
        } else if (a == QLatin1String("PP")) {
            result = *std::max_element(py.begin(), py.end()) - *std::min_element(py.begin(), py.end());
        } else {
            double integ = 0, integSq = 0;
            for (int k = 1; k < px.size(); ++k) {
                const double dx = px[k] - px[k - 1];
                integ += 0.5 * (py[k] + py[k - 1]) * dx;
                integSq += 0.5 * (py[k] * py[k] + py[k - 1] * py[k - 1]) * dx;
            }
            const double span = to - from;
            if (a == QLatin1String("INTEG"))
                result = integ;
            else if (a == QLatin1String("AVG"))
                result = span > 0 ? integ / span : py.first();
            else
                result = span > 0 ? std::sqrt(integSq / span) : std::fabs(py.first());
        }
        res.ok = true;
        res.value = result;
        res.from = from;
        res.to = to;
        res.exprText = QStringLiteral("%1(%2)").arg(a, e);
        res.logLine = QStringLiteral("%1: %2(%3)=%4 FROM %5 TO %6")
                          .arg(res.name, a, e, formatMeasNumber(result), formatMeasNumber(from), formatMeasNumber(to));
        return res;
    }

    // Bare expression (".meas tran x V(out)" is treated as FIND at the end).
    if (!segs.isEmpty() && !segs[0].text.isEmpty()) {
        auto v = evalExpr(segs[0].text, &err);
        if (!v)
            return fail(err);
        res.ok = true;
        res.complex = v->complex;
        res.value = v->c(v->size() - 1);
        res.exprText = segs[0].text;
        res.logLine = QStringLiteral("%1: %2=%3").arg(res.name, segs[0].text,
                                                       v->complex ? formatComplex(res.value, complexFormat) : formatMeasNumber(res.value.real()));
        return res;
    }
    return fail(QStringLiteral("unrecognized measurement"));
}

QString MeasureEngine::formatLog(const QList<QList<MeasResult>> &perRun, const QStringList &stepLabels)
{
    QStringList out;
    if (perRun.isEmpty())
        return QString();
    if (perRun.size() == 1) {
        for (const MeasResult &r : perRun[0])
            out << r.logLine;
        return out.join('\n');
    }
    // Stepped: one table per measurement name.
    QStringList names;
    for (const auto &run : perRun)
        for (const MeasResult &r : run)
            if (!names.contains(r.name))
                names << r.name;
    for (const QString &name : names) {
        out << QString();
        out << QStringLiteral("Measurement: %1").arg(name);
        QString header = QStringLiteral("  step\t");
        bool hasRange = false, hasAt = false;
        QString exprText;
        for (const auto &run : perRun)
            for (const MeasResult &r : run)
                if (r.name == name) {
                    hasRange |= !std::isnan(r.from);
                    hasAt |= !std::isnan(r.at);
                    if (exprText.isEmpty())
                        exprText = r.exprText;
                }
        header += exprText.isEmpty() ? name : exprText;
        if (hasAt)
            header += QStringLiteral("\tAT");
        if (hasRange)
            header += QStringLiteral("\tFROM\tTO");
        out << header;
        for (int i = 0; i < perRun.size(); ++i) {
            for (const MeasResult &r : perRun[i]) {
                if (r.name != name)
                    continue;
                QString row = QStringLiteral("%1\t").arg(i + 1, 6);
                row += r.ok ? (r.complex ? formatComplex(r.value, ComplexFormat::DbDegrees) : formatMeasNumber(r.value.real()))
                            : QStringLiteral("FAIL");
                if (hasAt)
                    row += QStringLiteral("\t") + formatMeasNumber(r.at);
                if (hasRange)
                    row += QStringLiteral("\t%1\t%2").arg(formatMeasNumber(r.from), formatMeasNumber(r.to));
                out << row;
            }
        }
    }
    Q_UNUSED(stepLabels);
    return out.join('\n');
}
