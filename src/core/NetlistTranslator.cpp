#include "NetlistTranslator.h"

#include "Expression.h"
#include "Laplace.h"
#include "ModelLibrary.h"
#include "TextIO.h"
#include "Units.h"

#include <cstring>
#include <functional>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <cmath>

namespace {

QString lower(const QString &s) { return s.toLower(); }

bool startsWithCI(const QString &s, const char *prefix)
{
    return s.startsWith(QLatin1String(prefix), Qt::CaseInsensitive);
}

// "Rser = 1" -> "Rser=1" (outside of quotes), keeps "==", "<=", ">=", "!=".
QString normalizeEquals(const QString &line)
{
    QString out;
    out.reserve(line.size());
    bool quote = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == '"')
            quote = !quote;
        if (!quote && c == '=') {
            const QChar prev = i > 0 ? line[i - 1] : QChar();
            const QChar next = i + 1 < line.size() ? line[i + 1] : QChar();
            if (next == '=' || prev == '=' || prev == '<' || prev == '>' || prev == '!') {
                out += c;
                continue;
            }
            while (!out.isEmpty() && out.endsWith(' '))
                out.chop(1);
            out += '=';
            int j = i + 1;
            while (j < line.size() && line[j] == ' ')
                ++j;
            i = j - 1;
            continue;
        }
        out += c;
    }
    return out;
}

// LTspice accepts ".param X table(...)" (no '='); ngspice needs "X=...".
// Braces nested inside parameter expressions are also removed.
QString normalizeParamLine(const QString &line)
{
    static const QRegularExpression noEq(QStringLiteral("^(\\.params?\\s+)([A-Za-z_][\\w]*)\\s+(?!=)(\\S.*)$"), QRegularExpression::CaseInsensitiveOption);
    QString l = line;
    const auto m = noEq.match(l);
    if (m.hasMatch() && !m.captured(3).startsWith('='))
        l = m.captured(1) + m.captured(2) + '=' + m.captured(3);
    // Split into "name = value" assignments at top level; each value gets
    // exactly one pair of braces (inner braces are not allowed by ngspice and
    // spaces inside unbraced expressions would split the value).
    const int kwEnd = l.indexOf(QRegularExpression(QStringLiteral("\\s")));
    if (kwEnd < 0)
        return l;
    const QString body = l.mid(kwEnd);
    struct Assign {
        int nameStart, eqPos;
    };
    QList<Assign> assigns;
    int depth = 0;
    for (int i = 0; i < body.size(); ++i) {
        const QChar c = body[i];
        if (c == '(' || c == '{')
            ++depth;
        else if (c == ')' || c == '}')
            --depth;
        else if (c == '=' && depth == 0) {
            const QChar next = i + 1 < body.size() ? body[i + 1] : QChar();
            const QChar prev = i > 0 ? body[i - 1] : QChar();
            if (next == '=' || prev == '=' || prev == '!' || prev == '<' || prev == '>')
                continue;
            int j = i - 1;
            while (j >= 0 && body[j].isSpace())
                --j;
            int k = j;
            while (k >= 0 && (body[k].isLetterOrNumber() || body[k] == '_' || body[k] == '$' || body[k] == '.'))
                --k;
            if (k == j)
                continue;
            assigns.append({k + 1, i});
        }
    }
    if (assigns.isEmpty())
        return l;
    QStringList parts;
    for (int a = 0; a < assigns.size(); ++a) {
        const QString name = body.mid(assigns[a].nameStart, assigns[a].eqPos - assigns[a].nameStart).trimmed();
        const int vEnd = a + 1 < assigns.size() ? assigns[a + 1].nameStart : int(body.size());
        QString value = body.mid(assigns[a].eqPos + 1, vEnd - assigns[a].eqPos - 1).trimmed();
        value.remove('{').remove('}');
        if (value.startsWith('"'))
            parts << QStringLiteral("%1=%2").arg(name, value); // string parameter
        else
            parts << QStringLiteral("%1={%2}").arg(name, value.trimmed());
    }
    return l.left(kwEnd) + ' ' + parts.join(' ');
}

bool isNumeric(const QString &tok)
{
    static const QRegularExpression re(QStringLiteral("^[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?[A-Za-zµ]*$"));
    return re.match(tok).hasMatch();
}

bool isValueToken(const QString &tok)
{
    return isNumeric(tok) || (units::isBraced(tok));
}

// Parameter values known to the translator, used to evaluate {expr} in analysis commands.
thread_local const QHash<QString, QString> *g_params = nullptr;

double evalParamExpr(const QString &text, int depth = 0);

double numberOr(const QString &tok, double def)
{
    if (units::isBraced(tok) && g_params) {
        const double v = evalParamExpr(tok.mid(1, tok.size() - 2));
        return std::isnan(v) ? def : v;
    }
    QString t = tok;
    if (units::isBraced(t))
        t = t.mid(1, t.size() - 2);
    auto v = units::parseSpiceNumber(t);
    return v ? *v : def;
}

double evalParamExpr(const QString &text, int depth)
{
    if (depth > 20)
        return qQNaN();
    Expression e(text);
    if (!e.isValid())
        return qQNaN();
    ParamContext ctx;
    for (const QString &id : e.identifiers()) {
        const QString key = id.toLower();
        if (g_params && g_params->contains(key)) {
            const double v = evalParamExpr(g_params->value(key), depth + 1);
            if (!std::isnan(v))
                ctx.vars.insert(key, Value::scalar(v));
        }
    }
    bool ok = false;
    const double v = e.evaluateScalar(ctx, &ok);
    return ok ? v : qQNaN();
}

// Splits "key=value" tokens into a map (keys lower-case) and returns the positional tokens.
QStringList splitParams(const QStringList &tokens, QHash<QString, QString> *named, QStringList *namedOrder = nullptr)
{
    QStringList positional;
    static const QRegularExpression namedRe(QStringLiteral("^[A-Za-z_][\\w.]*="));
    for (const QString &t : tokens) {
        const int eq = t.indexOf('=');
        if (namedRe.match(t).hasMatch()) {
            const QString k = t.left(eq).toLower();
            named->insert(k, t.mid(eq + 1));
            if (namedOrder)
                namedOrder->append(k);
        } else {
            positional << t;
        }
    }
    return positional;
}

const QSet<QString> &builtinModelNames()
{
    static const QSet<QString> s = {QStringLiteral("npn"),  QStringLiteral("pnp"),  QStringLiteral("nmos"), QStringLiteral("pmos"),
                                    QStringLiteral("njf"),  QStringLiteral("pjf"),  QStringLiteral("d"),    QStringLiteral("vdmos"),
                                    QStringLiteral("nmf"),  QStringLiteral("pmf"),  QStringLiteral("sw"),   QStringLiteral("csw")};
    return s;
}

QString builtinModelLine(const QString &name)
{
    const QString l = name.toLower();
    if (l == QLatin1String("npn")) return QStringLiteral(".model NPN NPN");
    if (l == QLatin1String("pnp")) return QStringLiteral(".model PNP PNP");
    if (l == QLatin1String("nmos")) return QStringLiteral(".model NMOS NMOS");
    if (l == QLatin1String("pmos")) return QStringLiteral(".model PMOS PMOS");
    if (l == QLatin1String("njf")) return QStringLiteral(".model NJF NJF");
    if (l == QLatin1String("pjf")) return QStringLiteral(".model PJF PJF");
    if (l == QLatin1String("d")) return QStringLiteral(".model D D");
    if (l == QLatin1String("vdmos")) return QStringLiteral(".model VDMOS VDMOS");
    if (l == QLatin1String("nmf")) return QStringLiteral(".model NMF NMF");
    if (l == QLatin1String("pmf")) return QStringLiteral(".model PMF PMF");
    if (l == QLatin1String("sw")) return QStringLiteral(".model SW SW");
    if (l == QLatin1String("csw")) return QStringLiteral(".model CSW CSW");
    return QString();
}

// Finds the matching ')' for the '(' at index `open`.
int matchParen(const QString &s, int open)
{
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        if (s[i] == '(')
            ++depth;
        else if (s[i] == ')' && --depth == 0)
            return i;
    }
    return -1;
}

// Splits the argument list of a call at top-level commas.
QStringList splitArgs(const QString &s)
{
    QStringList out;
    int depth = 0, start = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '(' || c == '{')
            ++depth;
        else if (c == ')' || c == '}')
            --depth;
        else if (c == ',' && depth == 0) {
            out << s.mid(start, i - start).trimmed();
            start = i + 1;
        }
    }
    out << s.mid(start).trimmed();
    return out;
}

} // namespace

QStringList NetlistTranslator::tokenize(const QString &line)
{
    QStringList out;
    QString cur;
    int depth = 0;
    bool quote = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == '"')
            quote = !quote;
        if (!quote) {
            if (c == '(' || c == '{' || c == '[')
                ++depth;
            else if (c == ')' || c == '}' || c == ']')
                depth = std::max(0, depth - 1);
        }
        if (!quote && depth == 0 && (c == ' ' || c == '\t' || c == ',')) {
            if (c == ',' && cur.isEmpty())
                continue;
            if (!cur.isEmpty()) {
                out << cur;
                cur.clear();
            }
            continue;
        }
        if (!quote && c == '(' && depth == 1 && cur.isEmpty() && !out.isEmpty()) {
            // "PULSE (0 1 ...)" -> attach to the previous word.
            static const QRegularExpression word(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
            if (word.match(out.last()).hasMatch()) {
                cur = out.takeLast();
            }
        }
        cur += c;
    }
    if (!cur.isEmpty())
        out << cur;
    return out;
}

QString NetlistTranslator::rewriteExpression(const QString &exprIn)
{
    QString expr = exprIn;
    expr.replace(QChar(0xB5), 'u');
    // Function rewrites, innermost first by repeated scanning.
    static const QRegularExpression fnRe(QStringLiteral("\\b(if|limit|table|buf|inv|mc|gauss|flat|u|rand|random|white)\\s*\\("),
                                         QRegularExpression::CaseInsensitiveOption);
    for (int guard = 0; guard < 200; ++guard) {
        // Find the last match so nested calls are rewritten inside-out.
        QRegularExpressionMatch last;
        auto it = fnRe.globalMatch(expr);
        while (it.hasNext())
            last = it.next();
        if (!last.hasMatch())
            break;
        const QString fn = last.captured(1).toLower();
        const int open = last.capturedEnd() - 1;
        const int close = matchParen(expr, open);
        if (close < 0)
            break;
        const QStringList a = splitArgs(expr.mid(open + 1, close - open - 1));
        QString rep;
        if (fn == QLatin1String("if") && a.size() == 3)
            rep = QStringLiteral("((%1) ? (%2) : (%3))").arg(a[0], a[1], a[2]);
        else if (fn == QLatin1String("limit") && a.size() == 3)
            rep = QStringLiteral("min(max((%1),min((%2),(%3))),max((%2),(%3)))").arg(a[0], a[1], a[2]);
        else if (fn == QLatin1String("table") && a.size() >= 3) {
            // Piecewise-linear lookup as nested conditionals (works in .param and B sources).
            QStringList xs, ys;
            for (int i = 1; i + 1 < a.size(); i += 2) {
                xs << a[i];
                ys << a[i + 1];
            }
            const QString x = QStringLiteral("(%1)").arg(a[0]);
            QString chain = QStringLiteral("(%1)").arg(ys.last());
            for (int i = xs.size() - 1; i >= 1; --i) {
                const QString seg = QStringLiteral("((%1)+((%2)-(%1))*(%3-(%4))/((%5)-(%4)))").arg(ys[i - 1], ys[i], x, xs[i - 1], xs[i]);
                chain = QStringLiteral("(%1<=(%2) ? %3 : %4)").arg(x, xs[i], seg, chain);
            }
            rep = QStringLiteral("(%1<=(%2) ? (%3) : %4)").arg(x, xs[0], ys[0], chain);
        }
        else if (fn == QLatin1String("mc") && a.size() == 2)
            rep = QStringLiteral("unif(%1,%2)").arg(a[0], a[1]); // uniform within ±tol
        else if (fn == QLatin1String("gauss") && a.size() == 1)
            rep = QStringLiteral("agauss(0,%1,1)").arg(a[0]);
        else if (fn == QLatin1String("flat") && a.size() == 1)
            rep = QStringLiteral("aunif(0,%1)").arg(a[0]);
        else if (fn == QLatin1String("buf") && a.size() == 1)
            rep = QStringLiteral("((%1) > 0.5 ? 1 : 0)").arg(a[0]);
        else if (fn == QLatin1String("inv") && a.size() == 1)
            rep = QStringLiteral("((%1) > 0.5 ? 0 : 1)").arg(a[0]);
        else if ((fn == QLatin1String("rand") || fn == QLatin1String("random") || fn == QLatin1String("white")) && a.size() == 1) {
            // Deterministic pseudo-random values per integer step of the argument (sine hash);
            // random()/white() interpolate smoothly between successive values.
            auto h = [](const QString &n) { return QStringLiteral("(abs(sin((%1)*12.9898+78.233)*43758.5453) - floor(abs(sin((%1)*12.9898+78.233)*43758.5453)))").arg(n); };
            const QString x = QStringLiteral("(%1)").arg(a[0]);
            const QString n = QStringLiteral("floor%1").arg(x);
            if (fn == QLatin1String("rand")) {
                rep = h(n);
            } else {
                const QString f = QStringLiteral("(%1-%2)").arg(x, n);
                const QString w = QStringLiteral("(%1*%1*(3-2*%1))").arg(f); // smoothstep
                rep = QStringLiteral("(%1+(%2-%1)*%3%4)").arg(h(n), h(n + QStringLiteral("+1")), w, fn == QLatin1String("white") ? QStringLiteral("-0.5") : QString());
            }
        }
        else if (fn == QLatin1String("u") && a.size() == 1)
            rep = QStringLiteral("((%1) > 0 ? 1 : 0)").arg(a[0]); // unit step; not available in ngspice .param expressions
        else
            rep = QStringLiteral("%1__(%2)").arg(fn, a.join(',')); // leave as-is (marked to avoid loops)
        expr.replace(last.capturedStart(), close - last.capturedStart() + 1, rep);
    }
    expr.replace(QRegularExpression(QStringLiteral("\\b(if|limit|table|buf|inv|mc|gauss|flat|u|rand|random|white)__\\(")), QStringLiteral("\\1("));
    return expr;
}

QString NetlistTranslator::nodeToken(const QString &n)
{
    QString s = n;
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9_+\\-.#:$/\\[\\]<>!&|~@%^]"));
    if (s.contains(bad))
        s.replace(bad, QStringLiteral("_"));
    const QString key = s.toLower();
    if (!m_res.nodeNames.contains(key))
        m_res.nodeNames.insert(key, n);
    return s;
}

QString NetlistTranslator::elementName(const QString &n)
{
    QString s = n;
    s.replace(QChar(0xA7), '_'); // '§'
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9_+\\-.#:$]"));
    s.replace(bad, QStringLiteral("_"));
    m_res.elementNames.insert(s.toLower(), n);
    return s;
}

QString NetlistTranslator::internalNode(const QString &elem, int k)
{
    return QStringLiteral("ls_%1_%2").arg(QString(elem).replace(QChar(0xA7), '_').toLower()).arg(k);
}

bool NetlistTranslator::isKnownModel(const QString &name) const
{
    const QString l = name.toLower();
    if (m_modelTypes.contains(l) || builtinModelNames().contains(l))
        return true;
    for (const QString &path : m_libFiles) {
        const LibraryFilePtr lib = ModelLibrary::instance().load(path);
        if (lib && lib->defs.contains(l) && lib->defs.value(l).kind == ModelDef::Model)
            return true;
    }
    return ModelLibrary::instance().standardModel(l).has_value();
}

// ---------------------------------------------------------------- top level

TranslationResult NetlistTranslator::translate(const QStringList &ltLines, const Options &opts)
{
    m_opts = opts;
    m_res = TranslationResult();
    m_topElements.clear();
    m_subckts.clear();
    m_models.clear();
    m_params.clear();
    m_directives.clear();
    m_analysis.clear();
    m_optionLines.clear();
    m_libFiles.clear();
    m_definedModels.clear();
    m_definedSubckts.clear();
    m_modelTypes.clear();
    m_pulledDefs.clear();
    m_inductorIc.clear();
    m_stepModelOverrides.clear();
    m_tranMaxStep = 0;
    m_deviceMaxStep = 0;
    m_thevInduc = false;
    m_startup = false;
    m_inMachine = false;
    m_modelLines.clear();
    m_senses.clear();

    // Pick up step-model markers before comments are stripped.
    for (const QString &l : ltLines) {
        if (l.startsWith(QLatin1String("*@stepmodel "))) {
            const QStringList t = l.mid(12).split(' ', Qt::SkipEmptyParts);
            if (t.size() == 3)
                m_stepModelOverrides[t[0].toLower()] << QStringLiteral("%1=%2").arg(t[1], t[2]);
        }
    }

    QStringList body = ltLines;
    if (!body.isEmpty() && body.first().trimmed().startsWith('*'))
        body.removeFirst(); // title
    processLines(ModelLibrary::logicalLines(body.join('\n')), 0);
    if (!m_analysis.isEmpty()) {
        const QString a = translateAnalysis(m_res.analysisLine);
        if (a.isEmpty()) {
            m_analysis.clear();
            m_res.analysis.clear();
        } else {
            m_analysis = {a};
        }
    }
    resolveDefinitions();
    {
        QStringList all = m_topElements + m_pulledDefs + m_params;
        for (const Subckt &sc : m_subckts)
            all << sc.body;
        collectCurrentSenses(all);
        for (const QString &l : all + m_models) {
            if (!l.trimmed().startsWith(QLatin1String(".model"), Qt::CaseInsensitive))
                continue;
            const QStringList mt = l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (mt.size() > 2 && !m_modelLines.contains(mt[1].toLower()))
                m_modelLines.insert(mt[1].toLower(), l);
        }
    }

    // Translate element lines.
    QStringList out;
    out << QStringLiteral("* LinSpice translated netlist");
    QStringList elements;
    for (const QString &l : m_topElements)
        elements << translateElement(l, true);
    elements << floatingNodeShunts(elements);

    QStringList subText;
    for (const Subckt &s : m_subckts) {
        subText << normalizeEquals(s.header);
        for (const QString &l : s.body) {
            const QString low = l.trimmed().toLower();
            if (low.startsWith(QLatin1String(".model")))
                subText << translateModel(l);
            else if (low.startsWith(QLatin1String(".param")))
                subText << rewriteExpression(normalizeEquals(normalizeParamLine(l)));
            else if (low.startsWith('.'))
                subText << normalizeEquals(l);
            else
                subText << translateElement(l, false);
        }
        subText << QStringLiteral(".ends %1").arg(s.name);
    }
    QStringList pulled;
    for (const QString &l : m_pulledDefs) {
        const QString low = l.trimmed().toLower();
        if (low.startsWith(QLatin1String(".model")))
            pulled << translateModel(l);
        else if (low.startsWith(QLatin1String(".param")))
            pulled << rewriteExpression(normalizeEquals(normalizeParamLine(l)));
        else if (low.startsWith(QLatin1String(".subckt")) || low.startsWith(QLatin1String(".ends")) || low.startsWith('.'))
            pulled << normalizeEquals(rewriteExpression(l));
        else
            pulled << translateElement(l, false);
    }

    out << QStringLiteral(".param pi=3.141592653589793");
    for (const QString &p : m_params)
        out << rewriteCurrents(rewriteExpression(normalizeEquals(p)));
    out << elements;
    out << subText;
    out << pulled;
    for (const QString &m : m_models)
        out << translateModel(m);
    out << m_directives;
    if (m_opts.saveCurrents)
        out << QStringLiteral(".options savecurrents");
    out << m_optionLines;
    // LTspice's documented defaults: trapezoidal integration and trtol=2 (ngspice uses 7, a much
    // looser truncation-error bound). Gear's numerical damping and the looser bound make
    // free-running oscillators run slow or die out (measured against LTspice: Colpitts -2.8%,
    // Clapp/Pierce not oscillating); FRA runs tighten trtol further for small-signal accuracy.
    bool methodSet = m_opts.extraOptions.contains(QLatin1String("method"), Qt::CaseInsensitive);
    bool trtolSet = m_opts.extraOptions.contains(QLatin1String("trtol"), Qt::CaseInsensitive);
    for (const QString &o : m_optionLines) {
        if (o.contains(QLatin1String("method="), Qt::CaseInsensitive))
            methodSet = true;
        if (o.contains(QLatin1String("trtol"), Qt::CaseInsensitive))
            trtolSet = true;
    }
    if (!methodSet)
        out << QStringLiteral(".options method=trap");
    if (!trtolSet)
        out << (m_res.fra ? QStringLiteral(".options trtol=1") : QStringLiteral(".options trtol=2"));
    if (!m_opts.extraOptions.trimmed().isEmpty())
        out << QStringLiteral(".options %1").arg(m_opts.extraOptions.trimmed());
    if (!m_analysis.isEmpty() && (m_res.fra || (m_deviceMaxStep > 0 && m_analysis.first().startsWith(QLatin1String(".tran"), Qt::CaseInsensitive)))) {
        const qsizetype nw = m_res.warnings.size();
        const QString a = translateAnalysis(m_res.analysisLine);
        m_res.warnings.resize(nw);
        if (!a.isEmpty())
            m_analysis = {a};
    }
    out << m_analysis;
    out << QStringLiteral(".end");

    // LTspice lets instances pass parameters a .subckt does not declare; ngspice
    // needs them declared, so add "name=0" defaults to the header.
    {
        QHash<QString, QSet<QString>> passed; // subckt -> param names
        static const QRegularExpression kv(QStringLiteral("([A-Za-z_][\\w.]*)\\s*="));
        for (const QString &l : out) {
            if (!l.startsWith('X', Qt::CaseInsensitive))
                continue;
            const QStringList t = tokenize(l);
            QString sub;
            QSet<QString> names;
            for (int i = 1; i < t.size(); ++i) {
                if (t[i].compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0)
                    continue;
                const auto m = kv.match(t[i]);
                if (m.hasMatch() && m.capturedStart() == 0)
                    names.insert(m.captured(1));
                else if (names.isEmpty())
                    sub = t[i];
            }
            if (!sub.isEmpty() && !names.isEmpty())
                passed[sub.toLower()].unite(names);
        }
        for (int li = 0; li < out.size(); ++li) {
            QString &l = out[li];
            if (!l.startsWith(QLatin1String(".subckt"), Qt::CaseInsensitive))
                continue;
            const QStringList t = tokenize(l);
            if (t.size() < 2 || !passed.contains(t[1].toLower()))
                continue;
            QSet<QString> declared;
            for (const QString &tok : t) {
                const auto m = kv.match(tok);
                if (m.hasMatch() && m.capturedStart() == 0)
                    declared.insert(m.captured(1).toLower());
            }
            // Body ".param" definitions of passed parameters become header
            // defaults, so that instance values override them (LTspice semantics).
            QHash<QString, QString> bodyDefaults;
            QSet<QString> passedLower;
            for (const QString &n : passed.value(t[1].toLower()))
                passedLower.insert(n.toLower());
            int depth = 0;
            for (int bi = li + 1; bi < out.size(); ++bi) {
                const QString bl = out[bi].trimmed().toLower();
                if (bl.startsWith(QLatin1String(".subckt"))) {
                    ++depth;
                    continue;
                }
                if (bl.startsWith(QLatin1String(".ends"))) {
                    if (depth-- == 0)
                        break;
                    continue;
                }
                if (depth > 0 || !bl.startsWith(QLatin1String(".param")))
                    continue;
                QStringList toks = tokenize(out[bi].trimmed().mid(6).trimmed());
                QStringList keepToks;
                for (const QString &tok : toks) {
                    const auto m = kv.match(tok);
                    if (m.hasMatch() && m.capturedStart() == 0 && passedLower.contains(m.captured(1).toLower()) && !declared.contains(m.captured(1).toLower())) {
                        bodyDefaults.insert(m.captured(1).toLower(), tok.mid(tok.indexOf('=') + 1));
                        continue;
                    }
                    keepToks << tok;
                }
                out[bi] = keepToks.isEmpty() ? QStringLiteral("* (parameter defaults moved to .subckt line)")
                                             : QStringLiteral(".param ") + keepToks.join(' ');
            }
            QStringList add;
            for (const QString &n : passed.value(t[1].toLower()))
                if (!declared.contains(n.toLower()))
                    add << QStringLiteral("%1=%2").arg(n, bodyDefaults.value(n.toLower(), QStringLiteral("0")));
            if (add.isEmpty())
                continue;
            if (!l.contains(QLatin1String("params:"), Qt::CaseInsensitive))
                l += QStringLiteral(" params:");
            l += ' ' + add.join(' ');
        }
    }
    // ngspice's parameter substitution mis-evaluates some "{expr}" forms inside
    // behavioral expressions (e.g. "x/{a*b}"); behavioral sources can reference
    // parameters directly, so plain parentheses are used there instead.
    for (QString &l : out) {
        if (!l.startsWith('B', Qt::CaseInsensitive))
            continue;
        const int eq = l.indexOf('=');
        if (eq < 0)
            continue;
        QString expr = l.mid(eq + 1);
        expr.replace('{', '(').replace('}', ')');
        l = l.left(eq + 1) + expr;
    }
    disambiguateModels(out);
    m_res.netlist = out.join('\n') + '\n';
    m_res.hasAnalysis = !m_res.analysis.isEmpty();
    if (!m_res.hasAnalysis)
        m_res.errors << QStringLiteral("No simulation command found. Add a .tran, .ac, .dc, .op, .noise or .tf directive.");
    return m_res;
}

void NetlistTranslator::processLines(const QStringList &lines, int depth)
{
    Subckt *open = nullptr;
    int nest = 0;
    for (QString l : lines) {
        l.replace(QChar(0xB5), 'u');
        l.replace(QChar(0x03BC), 'u');
        const QString t = l.trimmed();
        if (t.isEmpty() || t == QLatin1String("*<binary>"))
            continue;
        const QString low = t.toLower();

        if (open) {
            if (low.startsWith(QLatin1String(".subckt")))
                ++nest;
            if (low.startsWith(QLatin1String(".ends"))) {
                if (nest == 0) {
                    open = nullptr;
                    continue;
                }
                --nest;
            }
            if (low.startsWith(QLatin1String(".lib ")) || low.startsWith(QLatin1String(".inc"))) {
                // Library references inside sub-sheets apply globally.
                Subckt *keep = open;
                open = nullptr;
                processLines({t}, depth + 1);
                open = keep;
                continue;
            }
            open->body << t;
            if (low.startsWith(QLatin1String(".model"))) {
                static const QRegularExpression mre(QStringLiteral("^\\.model\\s+(\\S+)\\s+(?:ako:\\s*\\S+\\s+)?([A-Za-z]+)"),
                                                    QRegularExpression::CaseInsensitiveOption);
                const auto m = mre.match(t);
                if (m.hasMatch())
                    m_modelTypes.insert(m.captured(1).toLower(), m.captured(2).toUpper());
            }
            continue;
        }

        if (m_inMachine) {
            if (low.startsWith(QLatin1String(".endmachine")))
                m_inMachine = false;
            continue;
        }
        if (low.startsWith(QLatin1String(".machine"))) {
            m_inMachine = true;
            m_res.warnings << QStringLiteral(".machine state machines are not supported by the ngspice engine and were ignored");
            continue;
        }
        if (!low.startsWith('.')) {
            m_topElements << t;
            continue;
        }

        const QStringList tok = t.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString kw = tok[0].toLower();
        if (kw == QLatin1String(".subckt")) {
            Subckt s;
            s.name = tok.value(1);
            s.header = t;
            m_subckts.append(s);
            open = &m_subckts.last();
            nest = 0;
            m_definedSubckts.insert(s.name.toLower());
        } else if (kw == QLatin1String(".model")) {
            m_models << t;
            static const QRegularExpression mre(QStringLiteral("^\\.model\\s+(\\S+)\\s+(?:ako:\\s*\\S+\\s+)?([A-Za-z]+)"),
                                                QRegularExpression::CaseInsensitiveOption);
            const auto m = mre.match(t);
            if (m.hasMatch()) {
                m_definedModels.insert(m.captured(1).toLower());
                m_modelTypes.insert(m.captured(1).toLower(), m.captured(2).toUpper());
            }
        } else if (kw == QLatin1String(".param") || kw == QLatin1String(".params")) {
            QString p = normalizeParamLine(t);
            if (kw == QLatin1String(".params"))
                p = QStringLiteral(".param") + p.mid(7);
            m_params << p;
            // Record name=expr pairs for .meas evaluation.
            static const QRegularExpression pr(QStringLiteral("([A-Za-z_][\\w$]*)\\s*=\\s*(\\{[^}]*\\}|[^\\s]+)"));
            auto it = pr.globalMatch(p.mid(6));
            while (it.hasNext()) {
                const auto m = it.next();
                QString v = m.captured(2);
                if (units::isBraced(v))
                    v = v.mid(1, v.size() - 2);
                m_res.params.insert(m.captured(1).toLower(), v);
            }
        } else if (kw == QLatin1String(".func")) {
            m_params << t;
            m_res.funcLines << t;
        } else if (kw == QLatin1String(".include") || kw == QLatin1String(".inc")) {
            const QString file = t.section(QRegularExpression(QStringLiteral("\\s+")), 1).trimmed();
            const QString path = ModelLibrary::instance().resolveFile(file, m_opts.baseDir);
            if (path.isEmpty()) {
                m_res.errors << QStringLiteral("Could not open include file \"%1\"").arg(file);
                continue;
            }
            if (ModelLibrary::isEncryptedFile(path)) {
                m_res.errors << QStringLiteral("\"%1\" is an encrypted (proprietary) model file and cannot be simulated with ngspice.").arg(QFileInfo(path).fileName());
                continue;
            }
            if (depth > 8) {
                m_res.errors << QStringLiteral("Include nesting too deep at %1").arg(file);
                continue;
            }
            bool ok = false;
            const QString text = textio::readText(path, &ok);
            const QString saved = m_opts.baseDir;
            processLines(ModelLibrary::logicalLines(text), depth + 1);
            m_opts.baseDir = saved;
        } else if (kw == QLatin1String(".lib")) {
            const QStringList parts = tok.mid(1);
            if (parts.isEmpty())
                continue;
            addLibrary(parts[0], parts.value(1));
        } else if (kw == QLatin1String(".tran") || kw == QLatin1String(".ac") || kw == QLatin1String(".dc") || kw == QLatin1String(".op")
                   || kw == QLatin1String(".noise") || kw == QLatin1String(".tf")) {
            if (!m_analysis.isEmpty()) {
                m_res.warnings << QStringLiteral("Only one analysis is supported per run; ignoring \"%1\"").arg(t);
                continue;
            }
            // Translated after all lines are read so .param values are known.
            m_analysis << QString();
            m_res.analysis = kw.mid(1);
            m_res.analysisLine = t;
        } else if (kw == QLatin1String(".meas") || kw == QLatin1String(".measure")) {
            m_res.measStatements << t;
        } else if (kw == QLatin1String(".options") || kw == QLatin1String(".option") || kw == QLatin1String(".opt")) {
            handleOptions(t);
        } else if (kw == QLatin1String(".step")) {
            // Handled by the step planner before translation.
        } else if (kw == QLatin1String(".wave")) {
            m_res.waveStatements << t;
        } else if (kw == QLatin1String(".four") || kw == QLatin1String(".fourier")) {
            // LTspice: .four <freq> [Nharmonics] [Nperiods] <data...>
            QStringList parts = tok.mid(1);
            QStringList outParts;
            if (!parts.isEmpty())
                outParts << parts.takeFirst();
            while (!parts.isEmpty() && isNumeric(parts.first()))
                parts.removeFirst();
            outParts << parts;
            m_directives << QStringLiteral(".four %1").arg(outParts.join(' '));
            m_res.fourStatements << t;
        } else if (kw == QLatin1String(".ic") || kw == QLatin1String(".nodeset")) {
            // Move I(Lx)=val to the inductor's ic= parameter.
            static const QRegularExpression ire(QStringLiteral("\\bI\\(\\s*([^)\\s]+)\\s*\\)\\s*=\\s*(\\S+)"), QRegularExpression::CaseInsensitiveOption);
            QString rest = t;
            auto it = ire.globalMatch(t);
            while (it.hasNext()) {
                const auto m = it.next();
                m_inductorIc.insert(m.captured(1).toLower(), m.captured(2));
                rest.remove(m.captured(0));
            }
            if (rest.trimmed().size() > kw.size())
                m_directives << normalizeEquals(rest);
        } else if (kw == QLatin1String(".temp") || kw == QLatin1String(".global")) {
            m_directives << t;
        } else if (kw == QLatin1String(".save")) {
            m_res.warnings << QStringLiteral(".save is ignored; all node voltages and device currents are saved.");
        } else if (kw == QLatin1String(".end") || kw == QLatin1String(".backanno") || kw == QLatin1String(".endl")) {
            // nothing
        } else if (kw == QLatin1String(".fra")) {
            if (!m_analysis.isEmpty()) {
                m_res.warnings << QStringLiteral("Only one analysis is supported per run; ignoring \"%1\"").arg(t);
                continue;
            }
            // A transient run; its length comes from the FRA devices' sweep (known after elements).
            m_analysis << QString();
            m_res.analysis = QStringLiteral("tran");
            m_res.analysisLine = t;
            m_res.fra = true;
        } else if (kw == QLatin1String(".net")) {
            // Two-port parameters: computed from extra AC runs by the simulation controller.
        } else if (kw == QLatin1String(".machine")
                   || kw == QLatin1String(".savebias") || kw == QLatin1String(".loadbias") || kw == QLatin1String(".savestate")
                   || kw == QLatin1String(".loadstate") || kw == QLatin1String(".mc")) {
            m_res.warnings << QStringLiteral("%1 is not supported by the ngspice engine and was ignored").arg(tok[0]);
        } else {
            m_directives << t;
        }
    }
}

void NetlistTranslator::addLibrary(const QString &file, const QString &section)
{
    const QString path = ModelLibrary::instance().resolveFile(file, m_opts.baseDir);
    if (path.isEmpty()) {
        m_res.errors << QStringLiteral("Could not open library file \"%1\"").arg(file);
        return;
    }
    if (m_libFiles.contains(path))
        return;
    const LibraryFilePtr lib = ModelLibrary::instance().load(path);
    if (!lib) {
        m_res.errors << QStringLiteral("Could not read library file \"%1\"").arg(path);
        return;
    }
    if (lib->encrypted) {
        m_res.errors << QStringLiteral("\"%1\" is an encrypted (proprietary) model file and cannot be simulated with ngspice.")
                            .arg(QFileInfo(path).fileName());
        return;
    }
    m_libFiles << path;
    if (!section.isEmpty()) {
        m_libSections.insert(path, section);
        const QStringList secLines = lib->sections.value(section.toLower());
        if (secLines.isEmpty())
            m_res.warnings << QStringLiteral("Section \"%1\" not found in %2").arg(section, QFileInfo(path).fileName());
        // Treat the section content as included text.
        processLines(secLines, 1);
    }
    for (const QString &g : lib->globalLines)
        if (!m_params.contains(g))
            m_params << g;
    for (auto it = lib->defs.constBegin(); it != lib->defs.constEnd(); ++it)
        if (it->kind == ModelDef::Model && !m_modelTypes.contains(it.key()))
            m_modelTypes.insert(it.key(), it->type);
}

void NetlistTranslator::collectNeeds(const QStringList &elementLines, QSet<QString> &models, QSet<QString> &subckts) const
{
    for (const QString &raw : elementLines) {
        const QString line = normalizeEquals(raw.trimmed());
        if (line.startsWith('.') || line.isEmpty())
            continue;
        const QStringList t = tokenize(line);
        if (t.isEmpty())
            continue;
        const QChar k = t[0][0].toUpper();
        auto addModel = [&](int idx) {
            if (idx < t.size() && !t[idx].contains('=') && !isValueToken(t[idx]))
                models.insert(t[idx].toLower());
        };
        switch (k.unicode()) {
        case 'D': addModel(3); break;
        case 'J': addModel(4); break;
        case 'Z': addModel(4); break;
        case 'U': addModel(4); break;
        case 'S': addModel(5); break;
        case 'W': addModel(4); break;
        case 'O': addModel(5); break;
        case 'Q':
        case 'M':
            if (t.size() > 4 && isKnownModel(t[4]))
                models.insert(t[4].toLower());
            else
                addModel(5);
            break;
        case 'X': {
            // Subcircuit name: last token without '=' (before "params:").
            QString name;
            for (int i = 1; i < t.size(); ++i) {
                if (t[i].compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0)
                    break;
                if (!t[i].contains('='))
                    name = t[i];
            }
            if (!name.isEmpty())
                subckts.insert(name.toLower());
            break;
        }
        default:
            break;
        }
    }
}

void NetlistTranslator::resolveDefinitions()
{
    QSet<QString> doneModels = m_definedModels;
    QSet<QString> doneSubckts = m_definedSubckts;
    QStringList scan = m_topElements;
    for (const Subckt &s : m_subckts) {
        scan << s.body;
        // Nested definitions and local models inside netlist subcircuits.
        for (const QString &l : s.body) {
            const QStringList tt = l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (tt.size() > 1 && tt[0].compare(QLatin1String(".subckt"), Qt::CaseInsensitive) == 0)
                doneSubckts.insert(tt[1].toLower());
            if (tt.size() > 2 && tt[0].compare(QLatin1String(".model"), Qt::CaseInsensitive) == 0)
                doneModels.insert(tt[1].toLower());
        }
    }

    // Adds a definition pulled from a library: registers local models and nested
    // subcircuits, and follows .lib/.include lines found inside it.
    auto absorb = [&](const QStringList &lines, const QString &fromFile) {
        for (const QString &l : lines) {
            const QStringList tt = l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            const QString kw = tt.value(0).toLower();
            if ((kw == QLatin1String(".lib") || kw == QLatin1String(".include") || kw == QLatin1String(".inc")) && tt.size() > 1) {
                const QString saved = m_opts.baseDir;
                m_opts.baseDir = QFileInfo(fromFile).absolutePath();
                addLibrary(tt[1], kw == QLatin1String(".lib") ? tt.value(2) : QString());
                m_opts.baseDir = saved;
                continue;
            }
            if (kw == QLatin1String(".model") && tt.size() > 2) {
                doneModels.insert(tt[1].toLower());
                m_modelTypes.insert(tt[1].toLower(), tt[2].section('(', 0, 0).toUpper());
            } else if (kw == QLatin1String(".subckt") && tt.size() > 1) {
                doneSubckts.insert(tt[1].toLower());
            }
            m_pulledDefs << l;
            scan << l;
        }
    };

    for (int round = 0; round < 50 && !scan.isEmpty(); ++round) {
        QSet<QString> models, subckts;
        collectNeeds(scan, models, subckts);
        static const QRegularExpression akoRe(QStringLiteral("\\bako:\\s*(\\S+)"), QRegularExpression::CaseInsensitiveOption);
        for (const QString &l : scan + m_models) {
            const auto m = akoRe.match(l);
            if (m.hasMatch())
                models.insert(m.captured(1).toLower());
        }
        scan.clear();

        for (const QString &name : subckts) {
            if (doneSubckts.contains(name))
                continue;
            doneSubckts.insert(name);
            bool found = false;
            for (int li = 0; li < m_libFiles.size() && !found; ++li) {
                const QString path = m_libFiles[li];
                const LibraryFilePtr lib = ModelLibrary::instance().load(path);
                auto it = lib->defs.constFind(name);
                if (it != lib->defs.constEnd() && it->kind == ModelDef::Subckt) {
                    if (it->encrypted)
                        m_res.errors << QStringLiteral("The model for \"%1\" in %2 is encrypted by its vendor and cannot be simulated with ngspice.")
                                            .arg(it->name, QFileInfo(path).fileName());
                    else
                        absorb(it->lines, path);
                    found = true;
                }
            }
            if (!found) {
                // Try to locate "<name>.sub" in the library tree (LTspice ships one file per part).
                const QString file = ModelLibrary::instance().findSubcktFile(name);
                if (!file.isEmpty()) {
                    if (ModelLibrary::isEncryptedFile(file)) {
                        m_res.errors << QStringLiteral("The model for \"%1\" (%2) is encrypted by its vendor and cannot be simulated with ngspice.")
                                            .arg(name, QFileInfo(file).fileName());
                        continue;
                    }
                    const LibraryFilePtr lib = ModelLibrary::instance().load(file);
                    if (lib && !m_libFiles.contains(file)) {
                        m_libFiles << file;
                        for (auto it2 = lib->defs.constBegin(); it2 != lib->defs.constEnd(); ++it2)
                            if (it2->kind == ModelDef::Model && !m_modelTypes.contains(it2.key()))
                                m_modelTypes.insert(it2.key(), it2->type);
                    }
                    if (lib && lib->defs.contains(name)) {
                        absorb(lib->defs[name].lines, file);
                        found = true;
                    }
                }
            }
            if (!found) {
                // Do not double-report parts whose library was already reported as encrypted.
                bool encryptedReported = false;
                for (const QString &e : m_res.errors)
                    if (e.contains(QLatin1String("encrypted")))
                        encryptedReported = true;
                if (!encryptedReported)
                    m_res.errors << QStringLiteral("Unknown subcircuit called in: \"%1\"").arg(name);
            }
        }

        for (const QString &name : models) {
            if (doneModels.contains(name))
                continue;
            doneModels.insert(name);
            bool found = false;
            for (const QString &path : m_libFiles) {
                const LibraryFilePtr lib = ModelLibrary::instance().load(path);
                auto it = lib->defs.constFind(name);
                if (it != lib->defs.constEnd() && it->kind == ModelDef::Model) {
                    absorb(it->lines, path);
                    m_modelTypes.insert(name, it->type);
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
            if (auto sm = ModelLibrary::instance().standardModel(name)) {
                absorb(sm->lines, sm->file);
                m_modelTypes.insert(name, sm->type);
                continue;
            }
            const QString builtin = builtinModelLine(name);
            if (!builtin.isEmpty()) {
                m_pulledDefs << builtin;
                m_modelTypes.insert(name, builtin.section(' ', 2, 2).toUpper());
                continue;
            }
            m_res.errors << QStringLiteral("Could not find model \"%1\"").arg(name);
        }
    }
}

// ---------------------------------------------------------------- elements

namespace {

// Terminal index for a device-current function (Ic, Id, ...) on a device kind.
int senseTerminal(const QString &fn, QChar kind)
{
    const QString f = fn.toLower();
    kind = kind.toUpper();
    if (f == QLatin1String("i"))
        return 1;
    if (kind == 'Q') {
        if (f == QLatin1String("ic")) return 1;
        if (f == QLatin1String("ib")) return 2;
        if (f == QLatin1String("ie")) return 3;
        if (f == QLatin1String("is")) return 4;
    } else if (kind == 'M' || kind == 'J' || kind == 'Z') {
        if (f == QLatin1String("id")) return 1;
        if (f == QLatin1String("ig")) return 2;
        if (f == QLatin1String("is")) return 3;
        if (f == QLatin1String("ib")) return 4;
    }
    return 1;
}

const QRegularExpression &currentRefRe()
{
    static const QRegularExpression re(QStringLiteral("\\b(I|Ic|Ib|Ie|Id|Ig|Is)\\s*\\(\\s*([A-Za-z][^()\\s,]*)\\s*\\)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString senseSourceName(const QString &dev, int term)
{
    return QStringLiteral("Vls_%1_%2").arg(QString(dev).replace(QChar(0xA7), '_').toLower()).arg(term);
}

} // namespace

void NetlistTranslator::collectCurrentSenses(const QStringList &lines)
{
    static const QRegularExpression exprLine(QStringLiteral("^\\s*([BEG]\\S*|\\.param|\\.func)\\b"), QRegularExpression::CaseInsensitiveOption);
    for (const QString &l : lines) {
        if (!exprLine.match(l).hasMatch())
            continue;
        auto it = currentRefRe().globalMatch(l);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString dev = m.captured(2);
            const QChar kind = dev[0].toUpper();
            // Branch currents of voltage-like sources are available directly.
            if (kind == 'V' || kind == 'E' || kind == 'H')
                continue;
            m_senses[dev.toLower()].insert(senseTerminal(m.captured(1), kind));
        }
    }
}

QString NetlistTranslator::rewriteCurrents(const QString &expr) const
{
    if (m_senses.isEmpty())
        return expr;
    QString out = expr;
    QList<QRegularExpressionMatch> matches;
    auto it = currentRefRe().globalMatch(expr);
    while (it.hasNext())
        matches << it.next();
    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &m = matches[i];
        const QString dev = m.captured(2);
        if (!m_senses.contains(dev.toLower()))
            continue;
        const int term = senseTerminal(m.captured(1), dev[0]);
        out.replace(m.capturedStart(), m.capturedLength(), QStringLiteral("I(%1)").arg(senseSourceName(dev, term)));
    }
    return out;
}

QStringList NetlistTranslator::translateElement(const QString &rawLine, bool topLevel)
{
    Q_UNUSED(topLevel);
    QString line = rawLine.trimmed();
    if (line.isEmpty())
        return {};
    const QChar k = line[0].toUpper();
    if (k == 'B') {
        // A behavioral current source whose current is referenced elsewhere gets a sense source.
        const QStringList bt = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (bt.size() > 2 && m_senses.contains(bt[0].toLower())) {
            const QString inner = internalNode(bt[0], 91);
            const QString sense = QStringLiteral("%1 %2 %3 0").arg(senseSourceName(bt[0], 1), nodeToken(bt[1]), inner);
            QString rebuilt = line;
            const int p1 = rebuilt.indexOf(bt[1], bt[0].size());
            rebuilt.replace(p1, bt[1].size(), inner);
            return QStringList{sense} + translateBehavioral(rebuilt);
        }
        return translateBehavioral(line);
    }
    line = normalizeEquals(line);
    QStringList t = tokenize(line);
    t.removeAll(QStringLiteral("\"\""));
    if (t.isEmpty())
        return {};
    // PSPICE writes node pairs as "(a,b)" on controlled sources and switches.
    if (QStringLiteral("EFGHSW").contains(k)) {
        QStringList expanded;
        for (const QString &tok : t) {
            static const QRegularExpression pair(QStringLiteral("^\\(\\s*([^,()\\s]+)\\s*[,\\s]\\s*([^,()\\s]+)\\s*\\)$"));
            const auto m = pair.match(tok);
            if (m.hasMatch())
                expanded << m.captured(1) << m.captured(2);
            else
                expanded << tok;
        }
        t = expanded;
        line = t.join(' ');
    }
    // 0V sense sources for device currents used in behavioral expressions.
    QStringList senseLines;
    const auto senseIt = m_senses.constFind(t[0].toLower());
    if (senseIt != m_senses.constEnd()) {
        for (int term : *senseIt) {
            if (term < 1 || term >= t.size())
                continue;
            const QString orig = nodeToken(t[term]);
            const QString inner = internalNode(t[0], 90 + term);
            senseLines << QStringLiteral("%1 %2 %3 0").arg(senseSourceName(t[0], term), orig, inner);
            t[term] = inner;
        }
        line = t.join(' ');
    }
    if (!senseLines.isEmpty()) {
        QStringList res = translateElementTokens(t, line, k);
        return senseLines + res;
    }
    return translateElementTokens(t, line, k);
}

QStringList NetlistTranslator::translateElementTokens(QStringList t, const QString &line, QChar k)
{
    switch (k.unicode()) {
    case 'R':
    case 'C':
    case 'L':
        return translatePassive(t);
    case 'V':
    case 'I':
        return translateSource(t);
    case 'E':
    case 'G':
    case 'F':
    case 'H':
        return translateControlled(t, line);
    case 'J':
        if (t.size() == 3) {
            // Wire jumper: a short that lets a net carry two names.
            return {QStringLiteral("V%1_jumper %2 %3 0").arg(elementName(t[0]), nodeToken(t[1]), nodeToken(t[2]))};
        }
        return translateSemiconductor(t);
    case 'D':
    case 'Q':
    case 'M':
    case 'Z':
        return translateSemiconductor(t);
    case 'A':
        return translateDigital(t);
    case 'K':
        return translateMutual(t);
    case 'S':
    case 'W': {
        QStringList sw = translateSwitch(t);
        if (!sw.isEmpty())
            return sw;
        break; // native switch
    }
    case '@':
        return translateFraAnalyzer(t);
    case '&': {
        QString err;
        QStringList nodes = t;
        if (const auto probe = fra::parseProbe(nodes, &err))
            m_res.fraProbes << *probe;
        else
            m_res.errors << err;
        return {};
    }
    case 'X': {
        QStringList out;
        out << elementName(t[0]);
        for (int i = 1; i < t.size(); ++i) {
            if (t[i].contains('=') || t[i].compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0)
                out << rewriteExpression(t[i]);
            else
                out << t[i];
        }
        // Node tokens: everything between name and the subckt name.
        int last = -1;
        for (int i = 1; i < out.size(); ++i) {
            if (out[i].compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0)
                break;
            if (!out[i].contains('='))
                last = i;
        }
        for (int i = 1; i < last; ++i)
            out[i] = nodeToken(out[i]);
        return {out.join(' ')};
    }
    default:
        break;
    }
    {
        // S, W, T, O, U and anything else pass through with node bookkeeping.
        // PSPICE writes control node pairs as "(a,b)".
        QStringList expanded;
        for (const QString &tok : t) {
            if (tok.startsWith('(') && tok.endsWith(')') && tok.contains(','))
                expanded << tok.mid(1, tok.size() - 2).split(',', Qt::SkipEmptyParts);
            else
                expanded << tok;
        }
        QStringList out = expanded;
        out[0] = elementName(t[0]);
        int nodes = 0;
        switch (k.unicode()) {
        case 'S': nodes = 4; break;
        case 'W': nodes = 2; break;
        case 'T':
        case 'O': nodes = 4; break;
        case 'U': nodes = 3; break;
        default: nodes = 0; break;
        }
        for (int i = 1; i <= nodes && i < out.size(); ++i)
            out[i] = nodeToken(out[i].trimmed());
        for (int i = nodes + 1; i < out.size(); ++i)
            out[i] = rewriteExpression(out[i]);
        return {out.join(' ')};
    }
}

QStringList NetlistTranslator::translatePassive(const QStringList &t)
{
    QStringList out;
    if (t.size() < 3)
        return {t.join(' ')};
    const QChar k = t[0][0].toUpper();
    const QString name = elementName(t[0]);
    const QString n1 = nodeToken(t[1]), n2 = nodeToken(t[2]);
    QHash<QString, QString> p;
    QStringList order;
    QStringList pos = splitParams(t.mid(3), &p, &order);

    // Nonlinear capacitor given by its charge: I = d(Q)/dt with x = V(n1,n2).
    if (k == 'C' && p.contains(QStringLiteral("q"))) {
        QString q = p.value(QStringLiteral("q"));
        if (units::isBraced(q))
            q = q.mid(1, q.size() - 2);
        q.replace(QRegularExpression(QStringLiteral("\\bx\\b")), QStringLiteral("V(%1,%2)").arg(n1, n2));
        return {QStringLiteral("B%1_q %2 %3 I=ddt(%4)").arg(name, n1, n2, rewriteExpression(q))};
    }
    QString value;
    if (!pos.isEmpty())
        value = pos.takeFirst();
    const QString keyName = QString(k).toLower();
    if (value.isEmpty() && p.contains(keyName))
        value = p.take(keyName);
    // Remaining positional tokens such as "x2" (parallel count) or model names.
    QString mult;
    for (const QString &x : pos) {
        static const QRegularExpression parallel(QStringLiteral("^x(\\d+)$"), QRegularExpression::CaseInsensitiveOption);
        const auto m = parallel.match(x);
        if (m.hasMatch())
            mult = m.captured(1);
    }
    if (p.contains(QStringLiteral("m")))
        mult = p.take(QStringLiteral("m"));
    if (value.isEmpty()) {
        m_res.errors << QStringLiteral("Missing value for %1").arg(t[0]);
        value = QStringLiteral("1");
    }
    value = rewriteExpression(value);
    static const QRegularExpression dynRe(QStringLiteral("\\b(v|i)\\s*\\(|\\btime\\b"), QRegularExpression::CaseInsensitiveOption);
    if (k == 'R' && dynRe.match(value).hasMatch()) {
        QString e = value;
        if (units::isBraced(e))
            e = e.mid(1, e.size() - 2);
        return {QStringLiteral("B%1_r %2 %3 I=V(%2,%3)/(%4)").arg(name, n1, n2, e)};
    }

    // Charge/flux forms (C: Q=..., L: Flux=...) are behavioral.
    if ((k == 'C' && p.contains(QStringLiteral("q"))) || (k == 'L' && p.contains(QStringLiteral("flux")))) {
        m_res.warnings << QStringLiteral("%1: nonlinear charge/flux definitions are not supported; using value %2").arg(t[0], value);
    }

    const double ser = numberOr(p.value(QStringLiteral("rser")), 0.0);
    QString rser = p.take(QStringLiteral("rser"));
    QString lser = p.take(QStringLiteral("lser"));
    QString rpar = p.take(QStringLiteral("rpar"));
    QString cpar = p.take(QStringLiteral("cpar"));
    QString rlshunt = p.take(QStringLiteral("rlshunt"));
    QString ic = p.take(QStringLiteral("ic"));
    if (k == 'L' && ic.isEmpty())
        ic = m_inductorIc.value(t[0].toLower());
    Q_UNUSED(ser);

    // Drop LTspice-only metadata.
    for (const char *drop : {"tol", "pwr", "mfg", "pn", "type", "v", "irms", "ipk", "nt", "vrms", "fcs", "rate", "noiseless_", "pdiss"})
        p.remove(QLatin1String(drop));
    if (k == 'L' && p.contains(QStringLiteral("hc")))
        m_res.warnings << QStringLiteral("%1: hysteretic core model is not supported by ngspice; treated as a linear inductor").arg(t[0]);
    for (const char *hyst : {"hc", "bs", "br", "a", "lm", "lg", "n"})
        if (k == 'L')
            p.remove(QLatin1String(hyst));

    if (k == 'L' && rser.isEmpty() && m_opts.inductorRser && !m_thevInduc)
        rser = QStringLiteral("1m");
    if (!rser.isEmpty() && numberOr(rser, 1.0) == 0.0)
        rser.clear();
    if (!lser.isEmpty() && numberOr(lser, 1.0) == 0.0)
        lser.clear();

    QString extra;
    static const QSet<QString> allowedR = {QStringLiteral("tc1"), QStringLiteral("tc2"), QStringLiteral("tc"), QStringLiteral("temp"),
                                           QStringLiteral("dtemp"), QStringLiteral("noisy"), QStringLiteral("scale"), QStringLiteral("ac"),
                                           QStringLiteral("noiseless")};
    for (const QString &key : order) {
        if (!p.contains(key))
            continue;
        if (allowedR.contains(key))
            extra += QStringLiteral(" %1=%2").arg(key, p.value(key));
        else if (key != QLatin1String("q") && key != QLatin1String("flux"))
            m_res.warnings << QStringLiteral("%1: parameter \"%2\" ignored").arg(t[0], key);
    }
    if (!mult.isEmpty())
        extra += QStringLiteral(" m=%1").arg(mult);

    // Series chain: n1 -[Rser]- a -[Lser]- b -[core]- n2
    QString a = n1;
    int idx = 1;
    if (!rser.isEmpty() && k != 'R') {
        const QString nx = internalNode(t[0], idx++);
        out << QStringLiteral("R%1_rser %2 %3 %4").arg(QString(name).replace(QChar(0xA7), '_'), a, nx, rser);
        a = nx;
    }
    if (!lser.isEmpty() && k == 'C') {
        const QString nx = internalNode(t[0], idx++);
        out << QStringLiteral("L%1_lser %2 %3 %4").arg(name, a, nx, lser);
        a = nx;
    }
    QString core = QStringLiteral("%1 %2 %3 %4%5").arg(name, a, n2, value, extra);
    if (!ic.isEmpty())
        core += QStringLiteral(" ic=%1").arg(ic);
    out << core;
    // LTspice applies a capacitor's ic= as a constraint on the initial operating point; ngspice
    // only honours it with "uic". A stiff conductance pulling the capacitor to its initial voltage
    // while time=0 (the operating point) reproduces that. Not for .ac/.noise/.tf: the constraint
    // would stay in their linearization.
    if (k == 'C' && !ic.isEmpty() && (m_res.analysis == QLatin1String("tran") || m_res.analysis == QLatin1String("op"))) {
        QString icv = ic;
        if (units::isBraced(icv))
            icv = icv.mid(1, icv.size() - 2);
        out << QStringLiteral("B%1_ic %2 %3 I=(time<=0 ? 1e4*(V(%2,%3)-(%4)) : 0)").arg(QString(name).replace(QChar(0xA7), '_'), a, n2, icv);
    }
    if (!rpar.isEmpty())
        out << QStringLiteral("R%1_rpar %2 %3 %4").arg(name, n1, n2, rpar);
    if (!cpar.isEmpty() && numberOr(cpar, 1.0) != 0.0)
        out << QStringLiteral("C%1_cpar %2 %3 %4").arg(name, n1, n2, cpar);
    if (!rlshunt.isEmpty() && !lser.isEmpty())
        m_res.warnings << QStringLiteral("%1: RLshunt ignored").arg(t[0]);
    if (k == 'C' && !ic.isEmpty() && !rser.isEmpty())
        m_res.warnings << QStringLiteral("%1: ic= with series resistance applies to the internal capacitor node").arg(t[0]);
    return out;
}

QString NetlistTranslator::pwlFromFile(const QString &fileIn, double timeScale, double valueScale)
{
    QString file = fileIn;
    if (file.startsWith('"') && file.endsWith('"'))
        file = file.mid(1, file.size() - 2);
    QString path = file;
    if (!QFileInfo(path).isAbsolute())
        path = QDir(m_opts.baseDir).filePath(file.replace('\\', '/'));
    bool ok = false;
    const QString text = textio::readText(path, &ok);
    if (!ok) {
        m_res.errors << QStringLiteral("Cannot open PWL file \"%1\"").arg(fileIn);
        return QStringLiteral("0 0");
    }
    QStringList pts;
    double lastT = 0;
    for (const QString &raw : textio::splitLines(text)) {
        const QString l = raw.trimmed();
        if (l.isEmpty() || l.startsWith('*') || l.startsWith('#') || l.startsWith(';'))
            continue;
        const QStringList c = l.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        if (c.size() < 2)
            continue;
        QString ts = c[0];
        double tv;
        if (ts.startsWith('+'))
            tv = lastT + numberOr(ts.mid(1), 0);
        else
            tv = numberOr(ts, qQNaN());
        const double vv = numberOr(c[1], qQNaN());
        if (std::isnan(tv) || std::isnan(vv))
            continue;
        lastT = tv;
        pts << QStringLiteral("%1 %2").arg(tv * timeScale, 0, 'g', 15).arg(vv * valueScale, 0, 'g', 15);
    }
    if (pts.isEmpty())
        return QStringLiteral("0 0");
    return pts.join(' ');
}

QStringList NetlistTranslator::translateSource(const QStringList &t)
{
    QStringList out;
    if (t.size() < 3)
        return {t.join(' ')};
    const QChar k = t[0][0].toUpper();
    const QString name = elementName(t[0]);
    const QString n1 = nodeToken(t[1]), n2 = nodeToken(t[2]);
    QString dc, acMag, acPhase, func, rser, cpar, rpar;
    bool load = false;
    QStringList rest = t.mid(3);
    for (QString &tok : rest)
        if (units::isBraced(tok))
            tok = rewriteExpression(tok);
    for (int i = 0; i < rest.size(); ++i) {
        const QString tok = rest[i];
        const QString lowt = tok.toLower();
        if (lowt == QLatin1String("dc")) {
            if (i + 1 < rest.size())
                dc = rest[++i];
        } else if (lowt == QLatin1String("ac")) {
            if (i + 1 < rest.size())
                acMag = rest[++i];
            if (i + 1 < rest.size() && isValueToken(rest[i + 1]))
                acPhase = rest[++i];
        } else if (lowt.startsWith(QLatin1String("rser="))) {
            rser = tok.mid(5);
        } else if (lowt.startsWith(QLatin1String("cpar="))) {
            cpar = tok.mid(5);
        } else if (lowt.startsWith(QLatin1String("rpar="))) {
            rpar = tok.mid(5);
        } else if (lowt == QLatin1String("load") || lowt == QLatin1String("load2")) {
            load = true;
        } else if (lowt.startsWith(QLatin1String("pulse(")) || lowt.startsWith(QLatin1String("sin(")) || lowt.startsWith(QLatin1String("sine("))
                   || lowt.startsWith(QLatin1String("exp(")) || lowt.startsWith(QLatin1String("sffm(")) || lowt.startsWith(QLatin1String("am("))) {
            const int open = tok.indexOf('(');
            const QString fn = tok.left(open).toLower();
            QStringList args = tokenize(tok.mid(open + 1, tok.size() - open - 2));
            for (QString &a : args)
                a = rewriteExpression(a);
            if (fn == QLatin1String("sine") || fn == QLatin1String("sin")) {
                if (args.size() > 6) {
                    m_res.warnings << QStringLiteral("%1: SINE Ncycles is not supported; the sine runs indefinitely").arg(t[0]);
                    args = args.mid(0, 6);
                }
                func = QStringLiteral("SIN(%1)").arg(args.join(' '));
                // ngspice's step control only watches reactive elements, so a sine driving a
                // resistive network would be sampled at the maximum step; resolve its period.
                g_params = &m_res.params;
                const double freq = args.size() > 2 ? numberOr(args[2], 0) : 0;
                if (freq > 0) {
                    const double limit = 1.0 / (20 * freq);
                    m_deviceMaxStep = m_deviceMaxStep > 0 ? std::min(m_deviceMaxStep, limit) : limit;
                }
            } else if (fn == QLatin1String("pulse")) {
                // A zero rise or fall time is 10% of min(Ton, Tperiod-Ton) in LTspice (measured);
                // ngspice would substitute its timestep. A zero width really means (almost) zero.
                g_params = &m_res.params;
                const double ton = args.size() > 5 ? numberOr(args[5], 0) : 0;
                const double tper = args.size() > 6 ? numberOr(args[6], 0) : 0;
                const double edge = ton > 0 ? 0.1 * std::min(ton, tper > ton ? tper - ton : ton) : 1e-12;
                for (int ai = 3; ai <= 5 && ai < args.size(); ++ai)
                    if (isNumeric(args[ai]) && numberOr(args[ai], 1) == 0)
                        args[ai] = ai == 5 ? QStringLiteral("1e-12") : QString::number(edge, 'g', 12);
                func = QStringLiteral("PULSE(%1)").arg(args.join(' '));
            } else {
                func = QStringLiteral("%1(%2)").arg(fn.toUpper(), args.join(' '));
            }
        } else if (lowt.startsWith(QLatin1String("pwl"))) {
            // PWL [REPEAT FOR n|FOREVER] (...) [ENDREPEAT] | PWL file=... | PWL TIME_SCALE_FACTOR=.. (...)
            double timeScale = 1, valueScale = 1;
            QString file;
            QStringList points;
            int repeat = 1;
            bool repeatForever = false;
            QString td;
            int j = i;
            QString first = tok;
            if (first.contains('(')) {
                // "PWL(...)" in one token
                const int open = first.indexOf('(');
                points << first.mid(open + 1, first.size() - open - 2);
            }
            for (j = i + 1; j < rest.size(); ++j) {
                const QString r = rest[j];
                const QString lr = r.toLower();
                if (lr.startsWith(QLatin1String("file="))) file = r.mid(5);
                else if (lr.startsWith(QLatin1String("time_scale_factor="))) timeScale = numberOr(r.mid(18), 1);
                else if (lr.startsWith(QLatin1String("value_scale_factor="))) valueScale = numberOr(r.mid(19), 1);
                else if (lr == QLatin1String("repeat")) continue;
                else if (lr == QLatin1String("for") && j + 1 < rest.size()) { repeat = int(numberOr(rest[++j], 1)); }
                else if (lr == QLatin1String("forever")) repeatForever = true;
                else if (lr == QLatin1String("endrepeat")) continue;
                else if (r.startsWith('(')) points << r.mid(1, r.size() - 2);
                else if (lr.startsWith(QLatin1String("td="))) td = r.mid(3);
                else break;
            }
            i = j - 1;
            QString list;
            if (!file.isEmpty()) {
                list = pwlFromFile(file, timeScale, valueScale);
            } else {
                // Expand relative "+dt" times and scale factors.
                QStringList vals = tokenize(points.join(' '));
                QStringList norm;
                double lastT = 0;
                for (int v = 0; v + 1 < vals.size(); v += 2) {
                    QString ts = vals[v];
                    double tv;
                    if (ts.startsWith('+'))
                        tv = lastT + numberOr(ts.mid(1), 0);
                    else
                        tv = numberOr(ts, qQNaN());
                    QString vs = vals[v + 1];
                    if (std::isnan(tv)) {
                        // Expression time value: keep as is (no scaling possible).
                        norm << rewriteExpression(ts) << rewriteExpression(vs);
                        continue;
                    }
                    lastT = tv;
                    norm << QString::number(tv * timeScale, 'g', 15);
                    if (valueScale != 1.0 && isNumeric(vs))
                        norm << QString::number(numberOr(vs, 0) * valueScale, 'g', 15);
                    else
                        norm << rewriteExpression(vs);
                }
                if (repeat > 1 && !repeatForever && norm.size() >= 2) {
                    // Unroll a finite repeat.
                    QStringList unrolled;
                    const double period = numberOr(norm[norm.size() - 2], 0);
                    for (int r = 0; r < repeat && r < 1000; ++r) {
                        for (int v = 0; v + 1 < norm.size(); v += 2) {
                            const double tv = numberOr(norm[v], 0) + r * period;
                            if (r > 0 && v == 0 && numberOr(norm[v], 0) == 0)
                                continue;
                            unrolled << QString::number(tv, 'g', 15) << norm[v + 1];
                        }
                    }
                    norm = unrolled;
                }
                list = norm.join(' ');
            }
            func = QStringLiteral("PWL(%1)").arg(list);
            if (repeatForever)
                func += QStringLiteral(" r=0");
            if (!td.isEmpty())
                func += QStringLiteral(" td=%1").arg(td);
        } else if (lowt.startsWith(QLatin1String("wavefile="))) {
            int chan = 0;
            for (const QString &o : rest)
                if (o.toLower().startsWith(QLatin1String("chan=")))
                    chan = int(numberOr(o.mid(5), 0));
            func = QStringLiteral("PWL(%1)").arg(pwlFromWav(tok.mid(9), chan));
        } else if (lowt.startsWith(QLatin1String("tbl=")) || lowt.startsWith(QLatin1String("trigger="))) {
            m_res.warnings << QStringLiteral("%1: \"%2\" sources are not supported; using 0").arg(t[0], tok.section('=', 0, 0));
            dc = QStringLiteral("0");
        } else if (lowt.startsWith(QLatin1String("chan="))) {
            // part of wavefile
        } else if (isValueToken(tok) || tok.startsWith('{')) {
            if (dc.isEmpty())
                dc = rewriteExpression(tok);
        } else {
            m_res.warnings << QStringLiteral("%1: unrecognized source parameter \"%2\" ignored").arg(t[0], tok);
        }
    }

    // ".tran ... startup": DC supply voltages ramp up from 0V over the first 20µs.
    if (m_startup && k == 'V' && func.isEmpty() && !dc.isEmpty())
        func = QStringLiteral("PWL(0 0 20u %1)").arg(dc);
    QString a = n1;
    if (!rser.isEmpty() && numberOr(rser, 1) != 0 && k == 'V') {
        // V n+ -> internal, R internal -> n-  (keeps I(V) equal to the terminal current)
        const QString nx = internalNode(t[0], 1);
        out << QStringLiteral("R%1_rser %2 %3 %4").arg(name, nx, n2, rser);
        QString spec;
        if (!dc.isEmpty()) spec += QStringLiteral(" DC %1").arg(dc);
        if (!acMag.isEmpty()) spec += QStringLiteral(" AC %1 %2").arg(acMag, acPhase);
        if (!func.isEmpty()) spec += ' ' + func;
        out.prepend(QStringLiteral("%1 %2 %3%4").arg(name, n1, nx, spec));
    } else if (load && k == 'I') {
        // Current load: only conducts when the voltage across it is positive.
        const QString val = dc.isEmpty() ? QStringLiteral("0") : dc;
        out << QStringLiteral("B%1_load %2 %3 I=(%4)*min(max(V(%2,%3)/1m,0),1)").arg(name, n1, n2, val);
        if (!func.isEmpty())
            m_res.warnings << QStringLiteral("%1: time-varying current loads use their DC value").arg(t[0]);
    } else {
        QString spec;
        if (!dc.isEmpty()) spec += QStringLiteral(" DC %1").arg(dc);
        if (!acMag.isEmpty()) spec += QStringLiteral(" AC %1 %2").arg(acMag, acPhase).trimmed().prepend(' ');
        if (!func.isEmpty()) spec += ' ' + func;
        if (spec.isEmpty())
            spec = QStringLiteral(" DC 0");
        out << QStringLiteral("%1 %2 %3%4").arg(name, a, n2, spec);
    }
    if (!cpar.isEmpty() && numberOr(cpar, 1) != 0)
        out << QStringLiteral("C%1_cpar %2 %3 %4").arg(name, n1, n2, cpar);
    if (!rpar.isEmpty())
        out << QStringLiteral("R%1_rpar %2 %3 %4").arg(name, n1, n2, rpar);
    return out;
}

QStringList NetlistTranslator::translateBehavioral(const QString &lineIn)
{
    QString line = lineIn;
    const QStringList head = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (head.size() < 4)
        return {line};
    const QString name = elementName(head[0]);
    const QString n1 = nodeToken(head[1]), n2 = nodeToken(head[2]);
    // Everything after the nodes.
    int pos = 0;
    for (int i = 0; i < 3; ++i) {
        pos = line.indexOf(head[i], pos) + head[i].size();
    }
    QString rest = line.mid(pos).trimmed();
    static const QRegularExpression kindRe(QStringLiteral("^([VIRP])\\s*=\\s*"), QRegularExpression::CaseInsensitiveOption);
    const auto km = kindRe.match(rest);
    if (!km.hasMatch()) {
        m_res.errors << QStringLiteral("%1: behavioral source needs V=, I=, R= or P=").arg(head[0]);
        return {};
    }
    const QChar kind = km.captured(1)[0].toUpper();
    rest = rest.mid(km.capturedEnd());
    // Strip trailing LTspice-only parameters.
    static const QRegularExpression tail(QStringLiteral("\\s+(ic|tripdv|tripdt|laplace|rpar|cpar|rser|window|nfft|mtol|delay|nojacob|vprxy|iprxy)\\b.*$"),
                                         QRegularExpression::CaseInsensitiveOption);
    QString params;
    const auto tm = tail.match(rest);
    if (tm.hasMatch()) {
        params = tm.captured(0).trimmed();
        rest = rest.left(tm.capturedStart());
    }
    QString expr = rewriteCurrents(rewriteExpression(rest.trimmed()));
    if (units::isBraced(expr))
        expr = expr.mid(1, expr.size() - 2);
    if (params.contains(QLatin1String("laplace"), Qt::CaseInsensitive)) {
        // The B expression is the input of the transfer function.
        const QString inNode = internalNode(head[0], 51);
        QStringList out;
        out << QStringLiteral("%1_lin %2 0 V=%3").arg(name, inNode, expr);
        out << laplaceSource(name, kind == 'V' ? QChar('E') : QChar('G'), n1, n2, QStringLiteral("V(%1)").arg(inNode), params);
        return out;
    }

    QStringList out;
    switch (kind.unicode()) {
    case 'V':
        out << QStringLiteral("%1 %2 %3 V=%4").arg(name, n1, n2, expr);
        break;
    case 'I':
        out << QStringLiteral("%1 %2 %3 I=%4").arg(name, n1, n2, expr);
        break;
    case 'R':
        out << QStringLiteral("%1 %2 %3 I=V(%2,%3)/(%4)").arg(name, n1, n2, expr);
        break;
    case 'P':
        out << QStringLiteral("%1 %2 %3 I=(%4)/(abs(V(%2,%3))<1e-9 ? 1e-9 : V(%2,%3))").arg(name, n1, n2, expr);
        break;
    }
    static const QRegularExpression rparRe(QStringLiteral("rpar\\s*=\\s*(\\S+)"), QRegularExpression::CaseInsensitiveOption);
    const auto rp = rparRe.match(params);
    if (rp.hasMatch())
        out << QStringLiteral("R%1_rpar %2 %3 %4").arg(name, n1, n2, rp.captured(1));
    return out;
}

QStringList NetlistTranslator::translateControlled(const QStringList &t, const QString &line)
{
    const QChar k = t[0][0].toUpper();
    if (t.size() < 4)
        return {line};
    const QString name = elementName(t[0]);
    const QString n1 = nodeToken(t[1]), n2 = nodeToken(t[2]);
    const QString low = line.toLower();

    const QString bkind = k == 'E' ? QStringLiteral("V") : QStringLiteral("I");
    // VALUE= / VOL= / CUR= -> behavioral source.
    static const QRegularExpression valRe(QStringLiteral("\\b(value|vol|cur)\\s*(?:=\\s*|(?=[{'\"]))(.*)$"), QRegularExpression::CaseInsensitiveOption);
    if (k == 'E' || k == 'G') {
        const auto m = valRe.match(line);
        if (m.hasMatch() && line.left(m.capturedStart()).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size() <= 3) {
            QString expr = m.captured(2).trimmed();
            if ((units::isBraced(expr)) || (expr.startsWith('\'') && expr.endsWith('\'')) || (expr.startsWith('"') && expr.endsWith('"')))
                expr = expr.mid(1, expr.size() - 2);
            return {QStringLiteral("B%1 %2 %3 %4=%5").arg(name, n1, n2, bkind, rewriteCurrents(rewriteExpression(expr)))};
        }
        // PSPICE: E1 n+ n- TABLE {expr} = (x1,y1) (x2,y2) ...
        static const QRegularExpression pspTable(QStringLiteral("\\btable\\s*\\{(.*)\\}\\s*=?\\s*(.*)$"), QRegularExpression::CaseInsensitiveOption);
        const auto pt = pspTable.match(line);
        if (pt.hasMatch()) {
            QString pairs = pt.captured(2);
            pairs.replace('(', ' ').replace(')', ' ').replace(',', ' ');
            const QStringList nums = pairs.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            return {QStringLiteral("B%1 %2 %3 %4=%5").arg(name, n1, n2, bkind,
                                                         rewriteExpression(QStringLiteral("table(%1,%2)").arg(pt.captured(1), nums.join(','))))};
        }
    }
    // table=(x1,y1,...) or table(x1 y1 ...) with controlling nodes.
    static const QRegularExpression tblRe(QStringLiteral("\\btable\\s*=?\\s*\\("), QRegularExpression::CaseInsensitiveOption);
    const auto tm = tblRe.match(line);
    if ((k == 'E' || k == 'G') && tm.hasMatch() && t.size() >= 5) {
        const QString ctrl = QStringLiteral("V(%1,%2)").arg(nodeToken(t[3]), nodeToken(t[4]));
        const int open = tm.capturedEnd() - 1;
        QString tbl = line.mid(open + 1, matchParen(line, open) - open - 1);
        QStringList nums = tokenize(QString(tbl).replace(',', ' '));
        return {QStringLiteral("B%1 %2 %3 %4=%5").arg(name, n1, n2, bkind, rewriteExpression(QStringLiteral("table(%1,%2)").arg(ctrl, nums.join(','))))};
    }
    // LTspice "vto=" / "dir=" make a G/E source one-directional above a threshold.
    if ((k == 'E' || k == 'G') && t.size() >= 6 && (low.contains(QLatin1String("vto=")) || low.contains(QLatin1String("dir=")))) {
        QHash<QString, QString> named;
        QStringList pos = splitParams(t.mid(5), &named);
        const QString gain = rewriteExpression(pos.value(0, QStringLiteral("1")));
        const QString vto = named.value(QStringLiteral("vto"), QStringLiteral("0"));
        const QString dir = named.value(QStringLiteral("dir"), QStringLiteral("1"));
        const QString ctrl = QStringLiteral("V(%1,%2)").arg(nodeToken(t[3]), nodeToken(t[4]));
        return {QStringLiteral("B%1 %2 %3 %4=(%5)*(%6)*uramp((%6)*(%7-(%8)))").arg(name, n1, n2, bkind, gain, dir, ctrl, vto)};
    }
    if (low.contains(QLatin1String("laplace="))) {
        if (t.size() < 5) {
            m_res.errors << QStringLiteral("%1: Laplace source needs controlling nodes").arg(t[0]);
            return {};
        }
        return laplaceSource(name, k, n1, n2, QStringLiteral("V(%1,%2)").arg(nodeToken(t[3]), nodeToken(t[4])), line);
    }
    QStringList out = t;
    out[0] = name;
    out[1] = n1;
    out[2] = n2;
    if (k == 'E' || k == 'G') {
        const bool poly = low.contains(QLatin1String("poly("));
        if (!poly && t.size() >= 5) {
            out[3] = nodeToken(t[3]);
            out[4] = nodeToken(t[4]);
        }
    } else {
        // F/H: controlling source name (unless POLY form)
        if (!t[3].startsWith(QLatin1String("poly"), Qt::CaseInsensitive))
            out[3] = elementName(t[3]);
        else
            for (int i = 4; i < out.size() && !isNumeric(out[i]); ++i)
                out[i] = elementName(out[i]);
    }
    for (int i = 5; i < out.size(); ++i)
        out[i] = rewriteExpression(out[i]);
    return {out.join(' ')};
}

QStringList NetlistTranslator::translateSemiconductor(const QStringList &t)
{
    const QChar k = t[0][0].toUpper();
    // LTspice's idealized (piecewise-linear) diode: modelled behaviorally so that
    // negative Vfwd/Vrev/epsilon behave as in LTspice.
    if (k == 'D' && t.size() > 3) {
        const QString modelLine = m_modelLines.value(t[3].toLower());
        static const QRegularExpression ideal(QStringLiteral("\\b(ron|roff|vfwd|vrev|rrev|ilimit|revilimit|epsilon|revepsilon)\\s*="),
                                              QRegularExpression::CaseInsensitiveOption);
        if (!modelLine.isEmpty() && ideal.match(modelLine).hasMatch())
            return translateIdealDiode(t, modelLine);
    }
    QStringList out = t;
    out[0] = elementName(t[0]);
    int nodes = 2;
    switch (k.unicode()) {
    case 'D': nodes = 2; break;
    case 'J':
    case 'Z': nodes = 3; break;
    case 'Q':
    case 'M':
        nodes = (t.size() > 4 && isKnownModel(t[4])) ? 3 : 4;
        break;
    }
    for (int i = 1; i <= nodes && i < out.size(); ++i)
        out[i] = nodeToken(out[i]);
    int modelIdx = nodes + 1;
    if (modelIdx >= out.size()) {
        m_res.errors << QStringLiteral("%1: missing model name").arg(t[0]);
        return {out.join(' ')};
    }
    QString modelType = m_modelTypes.value(out[modelIdx].toLower());
    // LTspice accepts "M d g s b model" for VDMOS and ignores the body; ngspice's
    // VDMOS would read a 4th node as a thermal node, so drop it.
    if (k == 'M' && nodes == 4 && m_modelTypes.value(out[5].toLower()) == QLatin1String("VDMOS")) {
        out.removeAt(4);
        nodes = 3;
        modelIdx = 4;
        modelType = QStringLiteral("VDMOS");
    }
    if (k == 'Z' && (modelType == QLatin1String("NIGBT") || modelType == QLatin1String("PIGBT")))
        return translateIgbt(t, m_modelLines.value(out[modelIdx].toLower()), modelType == QLatin1String("PIGBT"));

    // Instance parameters: drop LTspice-only ones, handle diode series count n=.
    QStringList params;
    int series = 1;
    for (int i = modelIdx + 1; i < out.size(); ++i) {
        const QString p = out[i];
        const QString lp = p.toLower();
        if (k == 'D' && lp.startsWith(QLatin1String("n="))) {
            series = std::max(1, int(numberOr(p.mid(2), 1)));
            continue;
        }
        if (lp.startsWith(QLatin1String("mfg=")) || lp.startsWith(QLatin1String("type=")) || lp.startsWith(QLatin1String("pn=")))
            continue;
        static const QRegularExpression parallel(QStringLiteral("^x(\\d+)$"), QRegularExpression::CaseInsensitiveOption);
        const auto pm = parallel.match(p);
        if (pm.hasMatch()) {
            params << QStringLiteral("m=%1").arg(pm.captured(1));
            continue;
        }
        params << rewriteExpression(p);
    }
    if (k == 'D' && series > 1) {
        // Chain of identical diodes in series.
        QStringList chain;
        QString prev = out[1];
        for (int s = 0; s < series; ++s) {
            const QString next = s == series - 1 ? out[2] : internalNode(t[0], s + 1);
            const QString dn = s == 0 ? out[0] : QStringLiteral("%1_s%2").arg(out[0]).arg(s + 1);
            chain << QStringLiteral("%1 %2 %3 %4 %5").arg(dn, prev, next, out[modelIdx], params.join(' ')).trimmed();
            prev = next;
        }
        return chain;
    }
    QStringList res = out.mid(0, modelIdx + 1);
    res << params;
    return {res.join(' ')};
}

QStringList NetlistTranslator::translateDigital(const QStringList &t)
{
    // LTspice A-device: A<name> in1 in2 in3 in4 in5 outbar out com <model> [params]
    if (t.size() < 10) {
        m_res.errors << QStringLiteral("%1: malformed special function device").arg(t[0]);
        return {};
    }
    const QString model = t[9].toUpper();
    QHash<QString, QString> p;
    splitParams(t.mid(10), &p);
    const QString name = elementName(t[0]);
    QStringList ins;
    for (int i = 1; i <= 5; ++i) {
        const QString n = t[i];
        if (n == QLatin1String("0") || n.startsWith(QLatin1String("NC_"), Qt::CaseInsensitive) || n == t[8])
            continue;
        ins << nodeToken(n);
    }
    const QString outBar = t[6], outQ = t[7];
    const QString com = nodeToken(t[8]);
    const double vhigh = numberOr(p.value(QStringLiteral("vhigh")), 1.0);
    const double vlow = numberOr(p.value(QStringLiteral("vlow")), 0.0);
    const double ref = numberOr(p.value(QStringLiteral("ref")), (vhigh + vlow) / 2); // LTspice default: mid-swing
    const double trise = numberOr(p.value(QStringLiteral("trise")), 0.0);
    const double tfall = numberOr(p.value(QStringLiteral("tfall")), trise);
    const double td = numberOr(p.value(QStringLiteral("td")), 0.0);
    const double rout = numberOr(p.value(QStringLiteral("rout")), 1.0);

    auto in = [&](const QString &n) { return QStringLiteral("(V(%1,%2)>%3)").arg(n, com).arg(ref, 0, 'g', 12); };
    auto pin = [&](int idx) -> QString { // 1-based terminal, "" if unused
        const QString n = t[idx];
        if (n == QLatin1String("0") || n.startsWith(QLatin1String("NC_"), Qt::CaseInsensitive) || (idx != 8 && n == t[8]))
            return QString();
        return nodeToken(n);
    };
    QStringList out;
    // A held state: B source -> R -> C; the expression reads the capacitor node. The 1ns time
    // constant must not be far below the simulator's timestep at clock edges: otherwise the
    // capacitor has no memory at that scale, the hold loop becomes algebraic with two solutions
    // (0 and 1), and Newton can settle on either (a toggle flip-flop then follows its clock).
    auto stateNode = [&](const QString &suffix, const QString &nextExpr) {
        const QString st = internalNode(t[0], 10 + suffix.size());
        const QString stc = st + QStringLiteral("c");
        out << QStringLiteral("B%1_%2 %3 %4 V=%5").arg(name, suffix, st, com, nextExpr);
        out << QStringLiteral("R%1_%2 %3 %4 1k").arg(name, suffix, st, stc);
        out << QStringLiteral("C%1_%2 %3 %4 1p").arg(name, suffix, stc, com);
        return stc;
    };
    auto hold = [&](const QString &stc) { return QStringLiteral("(V(%1,%2)>0.5 ? 1 : 0)").arg(stc, com); };
    // Combinational gates use a continuous threshold (0.1mV wide) instead of a hard comparison:
    // inputs derived from other states move slightly on every Newton iteration, and a hard
    // threshold sitting at such an input makes the output flip until the timestep collapses.
    auto smoothIn = [&](const QString &n) {
        return QStringLiteral("(0.5+0.5*tanh(1e4*(V(%1,%2)-%3)))").arg(n, com).arg(ref, 0, 'g', 12);
    };
    QString logic;
    if (model == QLatin1String("AND") || model == QLatin1String("OR") || model == QLatin1String("XOR")) {
        if (ins.isEmpty()) {
            logic = QStringLiteral("0");
        } else if (model == QLatin1String("XOR")) {
            // True when exactly one input is true.
            QStringList terms;
            for (int i = 0; i < ins.size(); ++i) {
                QStringList f{smoothIn(ins[i])};
                for (int j = 0; j < ins.size(); ++j)
                    if (j != i)
                        f << QStringLiteral("(1-%1)").arg(smoothIn(ins[j]));
                terms << f.join('*');
            }
            logic = QStringLiteral("(%1)").arg(terms.join('+'));
        } else if (model == QLatin1String("AND")) {
            QStringList f;
            for (const QString &n : ins)
                f << smoothIn(n);
            logic = QStringLiteral("(%1)").arg(f.join('*'));
        } else {
            QStringList f;
            for (const QString &n : ins)
                f << QStringLiteral("(1-%1)").arg(smoothIn(n));
            logic = QStringLiteral("(1-%1)").arg(f.join('*'));
        }
    } else if (model == QLatin1String("BUF") || model == QLatin1String("BUF1") || model == QLatin1String("INV")) {
        logic = ins.isEmpty() ? QStringLiteral("0") : smoothIn(ins[0]);
        if (model == QLatin1String("INV"))
            logic = QStringLiteral("(1-%1)").arg(logic);
    } else if (model == QLatin1String("SCHMITT") || model == QLatin1String("SCHMTBUF") || model == QLatin1String("SCHMTINV")) {
        const double vt = numberOr(p.value(QStringLiteral("vt")), ref);
        const double vh = numberOr(p.value(QStringLiteral("vh")), 0.0);
        const QString a = pin(1), b = pin(2);
        const QString x = b.isEmpty() ? QStringLiteral("V(%1,%2)").arg(a.isEmpty() ? com : a, com) : QStringLiteral("V(%1,%2)").arg(a, b);
        const QString stc = internalNode(t[0], 11) + QStringLiteral("c");
        stateNode(QStringLiteral("s"), QStringLiteral("(%1 > %2 ? 1 : (%1 < %3 ? 0 : %4))")
                                           .arg(x).arg(vt + vh, 0, 'g', 12).arg(vt - vh, 0, 'g', 12).arg(hold(stc)));
        logic = hold(stc);
        if (model == QLatin1String("SCHMTINV"))
            logic = QStringLiteral("(1-%1)").arg(logic);
    } else if (model == QLatin1String("SRFLOP")) {
        const QString sPin = pin(1), rPin = pin(2);
        const QString stc = internalNode(t[0], 11) + QStringLiteral("c");
        const QString sExpr = sPin.isEmpty() ? QStringLiteral("0") : in(sPin);
        const QString rExpr = rPin.isEmpty() ? QStringLiteral("0") : in(rPin);
        stateNode(QStringLiteral("s"), QStringLiteral("(%1 ? 1 : (%2 ? 0 : %3))").arg(sExpr, rExpr, hold(stc)));
        logic = hold(stc);
    } else if (model == QLatin1String("DFLOP")) {
        // Edge-triggered storage uses ngspice's event-driven XSPICE flip-flop: a latch built from
        // behavioral sources decides "hold" from the current Newton iterate rather than the
        // previous time point, and with timesteps longer than its time constant both states solve
        // the equations (a divide-by-two flip-flop then occasionally misses a toggle).
        const QString d = pin(1), clk = pin(3), pre = pin(4), clr = pin(5);
        const QString base = QString(name).toLower();
        auto digIn = [&](const QString &n, const QString &tag) -> QString {
            if (n.isEmpty())
                return QStringLiteral("NULL");
            QString src = n;
            if (com != QLatin1String("0")) {
                src = internalNode(t[0], 30 + tag.size()) + tag;
                out << QStringLiteral("B%1_r%2 %3 0 V=V(%4,%5)").arg(name, tag, src, n, com);
            }
            const QString dn = QStringLiteral("%1_d%2").arg(base, tag);
            out << QStringLiteral("A%1_i%2 [%3] [%4] %5_adc").arg(name, tag, src, dn, base);
            return dn;
        };
        const QString dD = d.isEmpty() ? QString() : digIn(d, QStringLiteral("d"));
        const QString dC = digIn(clk, QStringLiteral("c"));
        const QString dS = digIn(pre, QStringLiteral("s"));
        const QString dR = digIn(clr, QStringLiteral("r"));
        out << QStringLiteral(".model %1_adc adc_bridge(in_low=%2 in_high=%2 rise_delay=1e-12 fall_delay=1e-12)").arg(base).arg(ref, 0, 'g', 12); // V > Ref is high
        QString dataNode = dD;
        if (dataNode.isEmpty()) {
            dataNode = base + QStringLiteral("_dzero");
            out << QStringLiteral("A%1_z %2 %3_zero").arg(name, dataNode, base);
            out << QStringLiteral(".model %1_zero d_pulldown").arg(base);
        }
        const double dly = 1e-12; // Td is applied by the output stage (drive)
        out << QStringLiteral("A%1_ff %2 %3 %4 %5 %6_q %6_qb %6_ffm").arg(name, dataNode, dC, dS, dR, base);
        out << QStringLiteral(".model %1_ffm d_dff(clk_delay=%2 set_delay=%2 reset_delay=%2 rise_delay=1e-12 fall_delay=1e-12)").arg(base).arg(dly, 0, 'g', 12);
        const QString aq = internalNode(t[0], 35);
        out << QStringLiteral("A%1_o [%2_q] [%3] %2_dac").arg(name, base, aq);
        out << QStringLiteral(".model %1_dac dac_bridge(out_low=0 out_high=1 out_undef=0.5 t_rise=1e-12 t_fall=1e-12)").arg(base);
        logic = QStringLiteral("V(%1)").arg(aq);
    } else if (model == QLatin1String("OTA")) {
        // Transconductance amplifier: I(out) = G * (V(in+,in-) + ref) [* V(mult+,mult-)],
        // limited to [Isink, Iout] (unless "linear") and clamped to the output compliance range.
        // Parameter values are passed through in braces so subcircuit parameters evaluate in place.
        auto pv = [&](const char *key, const QString &def) {
            const QString k = QLatin1String(key);
            if (!p.contains(k))
                return def;
            QString v = p.value(k);
            if (units::isBraced(v))
                v = v.mid(1, v.size() - 2);
            return QStringLiteral("{%1}").arg(rewriteExpression(v));
        };
        bool linear = false;
        for (int i = 10; i < t.size(); ++i)
            if (t[i].compare(QLatin1String("linear"), Qt::CaseInsensitive) == 0)
                linear = true;
        const QString g = pv("g", QStringLiteral("1"));
        const QString ref = pv("ref", QStringLiteral("0"));
        const QString inn = pin(1), inp = pin(2), m1 = pin(3), m2 = pin(4), vneg = pin(5), vpos = pin(6);
        const QString outN = t[7] == QLatin1String("0") ? QString() : nodeToken(t[7]);
        if (outN.isEmpty())
            return out;
        auto v = [&](const QString &n) { return n.isEmpty() ? QStringLiteral("V(%1)").arg(com) : QStringLiteral("V(%1)").arg(n); };
        QString vin = QStringLiteral("(%1-%2+%3)").arg(v(inp), v(inn), ref);
        if (!m1.isEmpty() || !m2.isEmpty())
            vin = QStringLiteral("(%1*(%2-%3))").arg(vin, v(m1), v(m2));
        QString iexpr = QStringLiteral("%1*%2").arg(g, vin);
        const bool hasIout = p.contains(QStringLiteral("iout")), hasIsink = p.contains(QStringLiteral("isink"));
        if (!linear && (hasIout || hasIsink)) {
            const QString iout = pv("iout", hasIsink ? QStringLiteral("(-(%1))").arg(pv("isink", QStringLiteral("0"))) : QStringLiteral("1e30"));
            const QString isink = pv("isink", QStringLiteral("(-(%1))").arg(iout));
            if (!hasIsink)
                iexpr = QStringLiteral("%1*tanh((%2)/%1)").arg(iout, iexpr);
            else
                iexpr = QStringLiteral("min(max(%1,%2),%3)").arg(iexpr, isink, iout);
        }
        QString clamp;
        const bool hugeHigh = numberOr(p.value(QStringLiteral("vhigh")), 0) >= 1e100;
        const bool hugeLow = numberOr(p.value(QStringLiteral("vlow")), 0) <= -1e100;
        const QString hi = vpos.isEmpty() ? QStringLiteral("(V(%1)+%2)").arg(com, pv("vhigh", QStringLiteral("1e30"))) : QStringLiteral("V(%1)").arg(vpos);
        const QString lo = vneg.isEmpty() ? QStringLiteral("(V(%1)+%2)").arg(com, pv("vlow", QStringLiteral("-1e30"))) : QStringLiteral("V(%1)").arg(vneg);
        if (!(vpos.isEmpty() && hugeHigh && vneg.isEmpty() && hugeLow) && (p.contains(QStringLiteral("vhigh")) || p.contains(QStringLiteral("vlow")) || !vpos.isEmpty() || !vneg.isEmpty()))
            clamp = QStringLiteral(" - 1k*(uramp(V(%1)-%2) - uramp(%3-V(%1)))").arg(outN, hi, lo);
        // Current flows into the output node (B source I flows from n+ to n- through the source).
        out << QStringLiteral("B%1_ota %2 %3 I=%4%5").arg(name, com, outN, iexpr, clamp);
        if (p.contains(QStringLiteral("rout")))
            out << QStringLiteral("R%1_rout %2 %3 %4").arg(name, outN, com, pv("rout", QStringLiteral("1e12")));
        if (p.contains(QStringLiteral("cout")))
            out << QStringLiteral("C%1_cout %2 %3 %4").arg(name, outN, com, pv("cout", QStringLiteral("0")));
        return out;
    } else if (model == QLatin1String("VARISTOR")) {
        // Conducts between out and com once |V(out,com)| exceeds the control voltage V(in+,in-).
        const QString inn = pin(1), inp = pin(2);
        const QString outN = nodeToken(t[7]);
        const double rclamp = numberOr(p.value(QStringLiteral("rclamp")), 1.0);
        const QString vc = QStringLiteral("abs(V(%1)-V(%2))").arg(inp.isEmpty() ? com : inp, inn.isEmpty() ? com : inn);
        out << QStringLiteral("B%1_var %2 %3 I=sgn(V(%2,%3))*uramp(abs(V(%2,%3))-%4)/%5").arg(name, outN, com, vc).arg(rclamp, 0, 'g', 12);
        return out;
    } else if (model == QLatin1String("PHASEDET")) {
        // Tri-state phase-frequency detector with a charge-pump output (characterized against
        // LTspice with stepped phase offsets): a falling crossing of input 1 sets UP, of input 2
        // sets DN, both set resets both; the output sources Iout*(UP-DN) into terminal 7,
        // limited to [Vlow, Vhigh]. The edge polarity matters: it decides which of the two
        // equivalent phase branches (phi or phi-360) the detector settles into from t=0.
        const QString in1 = pin(1), in2 = pin(2);
        if (t[7] == QLatin1String("0") || t[7].startsWith(QLatin1String("NC_"), Qt::CaseInsensitive))
            return out;
        const QString outN = nodeToken(t[7]);
        const QString base = QString(name).toLower();
        auto digIn = [&](const QString &n, const QString &tag) -> QString {
            QString src = n.isEmpty() ? com : n;
            if (com != QLatin1String("0")) {
                src = internalNode(t[0], 40 + tag.size()) + tag;
                out << QStringLiteral("B%1_r%2 %3 0 V=V(%4,%5)").arg(name, tag, src, n.isEmpty() ? com : n, com);
            }
            const QString dn = QStringLiteral("%1_d%2").arg(base, tag);
            out << QStringLiteral("A%1_i%2 [%3] [%4] %5_adc").arg(name, tag, src, dn, base);
            return dn;
        };
        const QString a1 = digIn(in1, QStringLiteral("a")), a2 = digIn(in2, QStringLiteral("b"));
        const QString c1 = base + QStringLiteral("_na"), c2 = base + QStringLiteral("_nb");
        out << QStringLiteral("A%1_na %2 %3 %4_inv").arg(name, a1, c1, base);
        out << QStringLiteral("A%1_nb %2 %3 %4_inv").arg(name, a2, c2, base);
        out << QStringLiteral(".model %1_inv d_inverter(rise_delay=1e-12 fall_delay=1e-12)").arg(base);
        out << QStringLiteral(".model %1_adc adc_bridge(in_low=%2 in_high=%2 rise_delay=1e-12 fall_delay=1e-12)").arg(base).arg(ref, 0, 'g', 12); // V > Ref is high
        out << QStringLiteral("A%1_one %2_one %2_hi").arg(name, base);
        out << QStringLiteral(".model %1_hi d_pullup").arg(base);
        out << QStringLiteral("A%1_up %2_one %3 NULL %2_rst %2_up %2_upb %2_ffm").arg(name, base, c1);
        out << QStringLiteral("A%1_dn %2_one %3 NULL %2_rst %2_dn %2_dnb %2_ffm").arg(name, base, c2);
        out << QStringLiteral(".model %1_ffm d_dff(clk_delay=1e-12 set_delay=1e-12 reset_delay=1e-12 rise_delay=1e-12 fall_delay=1e-12)").arg(base);
        out << QStringLiteral("A%1_and [%2_up %2_dn] %2_rst %2_andm").arg(name, base);
        out << QStringLiteral(".model %1_andm d_and(rise_delay=1e-12 fall_delay=1e-12)").arg(base);
        const QString up = internalNode(t[0], 45), dn = internalNode(t[0], 46);
        out << QStringLiteral("A%1_o [%2_up %2_dn] [%3 %4] %2_dac").arg(name, base, up, dn);
        out << QStringLiteral(".model %1_dac dac_bridge(out_low=0 out_high=1 out_undef=0.5 t_rise=1e-12 t_fall=1e-12)").arg(base);
        const double iout = numberOr(p.value(QStringLiteral("iout")), 1e-6);
        QString clamp;
        if (p.contains(QStringLiteral("vhigh")) || p.contains(QStringLiteral("vlow")))
            clamp = QStringLiteral(" - 1k*(uramp(V(%1,%2)-%3) - uramp(%4-V(%1,%2)))").arg(outN, com).arg(vhigh, 0, 'g', 12).arg(vlow, 0, 'g', 12);
        out << QStringLiteral("B%1_cp %2 %3 I=%4*(V(%5)-V(%6))%7").arg(name, com, outN).arg(iout, 0, 'g', 12).arg(up, dn, clamp);
        return out;
    } else if (model == QLatin1String("SAMPLEHOLD")) {
        // in+ in- CLK S/H . . out com. With S/H connected the output tracks V(in+,in-) while S/H is
        // high and holds while low; with CLK connected it samples on the rising edge (master-slave).
        // Each hold capacitor is charged through a gated 10S conductance (1ns tracking time constant).
        const QString inp = pin(1), inn = pin(2), clk = pin(3), sh = pin(4);
        if (t[7] == QLatin1String("0") || t[7].startsWith(QLatin1String("NC_"), Qt::CaseInsensitive))
            return out;
        const QString outN = nodeToken(t[7]);
        const QString vin = QStringLiteral("V(%1,%2)").arg(inp.isEmpty() ? com : inp, inn.isEmpty() ? com : inn);
        auto trackHold = [&](int idx, const QString &gate, const QString &src) {
            const QString h = internalNode(t[0], idx);
            out << QStringLiteral("B%1_h%2 %3 %4 I=(%5 ? 10 : 0)*((%6)-V(%4,%3))").arg(name).arg(idx).arg(com, h, gate, src);
            out << QStringLiteral("C%1_h%2 %3 %4 10n").arg(name).arg(idx).arg(h, com);
            return QStringLiteral("V(%1,%2)").arg(h, com);
        };
        QString value = vin;
        if (!clk.isEmpty()) {
            const QString master = trackHold(21, QStringLiteral("!%1").arg(in(clk)), vin);
            value = trackHold(22, in(clk), master);
        } else if (!sh.isEmpty()) {
            value = trackHold(21, in(sh), vin);
        }
        out << QStringLiteral("B%1_o %2 %3 V=%4").arg(name, outN, com, value);
        return out;
    } else if (model == QLatin1String("MODULATOR")) {
        // VCO: frequency = space + (mark-space)*V(FM); amplitude V(AM) (1V if unused). The phase,
        // in cycles, is the voltage of a 1F capacitor charged by a current equal to the frequency.
        const QString fm = pin(1), am = pin(2);
        const double mark = numberOr(p.value(QStringLiteral("mark")), 1.0);
        const double space = numberOr(p.value(QStringLiteral("space")), 0.0);
        const QString ph = internalNode(t[0], 21);
        // The phase starts at zero: no charging current during the operating point (time=0).
        out << QStringLiteral("B%1_ph %2 %3 I=(time>0 ? %4+%5*V(%6,%2) : 0)").arg(name, com, ph).arg(space, 0, 'g', 15).arg(mark - space, 0, 'g', 15).arg(fm.isEmpty() ? com : fm);
        out << QStringLiteral("C%1_ph %2 %3 1").arg(name, ph, com);
        // The sine outputs are not integrated states, so the timestep must resolve them explicitly.
        const double fmax = std::max({std::fabs(mark), std::fabs(space), std::fabs(2 * mark - space)});
        if (fmax > 0) {
            const double limit = 1.0 / (50 * fmax);
            m_deviceMaxStep = m_deviceMaxStep > 0 ? std::min(m_deviceMaxStep, limit) : limit;
        }
        const QString amp = am.isEmpty() ? QStringLiteral("1") : QStringLiteral("V(%1,%2)").arg(am, com);
        auto output = [&](int idx, const char *fn, const char *suffix) {
            if (t[idx] == QLatin1String("0") || t[idx].startsWith(QLatin1String("NC_"), Qt::CaseInsensitive) || t[idx] == t[8])
                return;
            out << QStringLiteral("B%1_%2 %3 %4 V=%5*%6(2*pi*V(%7,%4))").arg(name, QLatin1String(suffix), nodeToken(t[idx]), com, amp, QLatin1String(fn), ph);
        };
        output(6, "cos", "cos");
        output(7, "sin", "sin");
        return out;
    } else {
        m_res.errors << QStringLiteral("Special function %2 is not supported by the ngspice engine (%1)").arg(t[0], model);
        return {};
    }

    auto drive = [&](const QString &outNode, const QString &lg, const QString &suffix) {
        if (outNode == QLatin1String("0") || outNode.startsWith(QLatin1String("NC_"), Qt::CaseInsensitive))
            return;
        const QString o = nodeToken(outNode);
        const QString src = internalNode(t[0], suffix == QLatin1String("q") ? 1 : 2);
        out << QStringLiteral("B%1_%2 %3 %4 V=%5+%6*%7").arg(name, suffix, src, com).arg(vlow, 0, 'g', 12).arg(vhigh - vlow, 0, 'g', 12).arg(lg);
        QString drv = src;
        if (td > 0) {
            const QString del = internalNode(t[0], suffix == QLatin1String("q") ? 3 : 4);
            out << QStringLiteral("T%1_%2 %3 %4 %5 %4 Z0=1k Td=%6").arg(name, suffix, src, com, del).arg(td, 0, 'g', 12);
            out << QStringLiteral("R%1_%2t %3 %4 1k").arg(name, suffix, del, com);
            drv = del;
        }
        out << QStringLiteral("R%1_%2o %3 %4 %5").arg(name, suffix, drv, o).arg(rout, 0, 'g', 12);
        const double tr = std::max(trise, tfall);
        if (tr > 0)
            out << QStringLiteral("C%1_%2o %3 %4 %5").arg(name, suffix, o, com).arg(tr / 2.2 / rout, 0, 'g', 12);
    };
    drive(outQ, logic, QStringLiteral("q"));
    drive(outBar, QStringLiteral("(1-%1)").arg(logic), QStringLiteral("qb"));
    return out;
}

QStringList NetlistTranslator::translateSwitch(const QStringList &t)
{
    // LTspice switch models support parameters ngspice's switch lacks
    // (series voltage, current limit, one-way conduction, smooth transition
    // via negative hysteresis). Those are modelled behaviorally; plain
    // hysteresis switches keep ngspice's native element.
    const QChar k = t[0][0].toUpper();
    const int modelIdx = k == 'S' ? 5 : 4;
    if (t.size() <= modelIdx)
        return {};
    const QString modelLine = m_modelLines.value(t[modelIdx].toLower());
    if (modelLine.isEmpty())
        return {};
    QHash<QString, QString> mp;
    {
        QString body = modelLine;
        const int open = body.indexOf('(');
        if (open >= 0)
            body = body.mid(open + 1, body.lastIndexOf(')') - open - 1);
        else
            body = body.section(QRegularExpression(QStringLiteral("\\s+")), 3);
        body = normalizeEquals(body);
        for (const QString &tok : tokenize(QString(body).replace(',', ' '))) {
            const int eq = tok.indexOf('=');
            if (eq > 0)
                mp.insert(tok.left(eq).toLower(), tok.mid(eq + 1));
            else
                mp.insert(tok.toLower(), QStringLiteral("1"));
        }
    }
    auto num = [&](const QString &key, double def) { return numberOr(mp.value(key), def); };
    const bool vhNegative = num(k == 'S' ? QStringLiteral("vh") : QStringLiteral("ih"), 0) < 0;
    const bool needed = mp.contains(QStringLiteral("ilimit")) || mp.contains(QStringLiteral("vser")) || mp.contains(QStringLiteral("oneway"))
        || mp.contains(QStringLiteral("level")) || vhNegative;
    if (!needed)
        return {};
    auto val = [&](const QString &key, const QString &def) {
        QString v = mp.value(key, def);
        if (units::isBraced(v))
            v = v.mid(1, v.size() - 2);
        return QStringLiteral("{%1}").arg(rewriteExpression(v)); // braces: numparam evaluates params
    };
    const QString name = elementName(t[0]);
    const QString a = nodeToken(t[1]), b = nodeToken(t[2]);
    QString x;
    if (k == 'S')
        x = QStringLiteral("V(%1,%2)").arg(nodeToken(t[3]), nodeToken(t[4]));
    else
        x = QStringLiteral("I(%1)").arg(elementName(t[3]));
    const QString vt = val(k == 'S' ? QStringLiteral("vt") : QStringLiteral("it"), QStringLiteral("0"));
    const QString vh = val(k == 'S' ? QStringLiteral("vh") : QStringLiteral("ih"), QStringLiteral("0"));
    const QString ron = val(QStringLiteral("ron"), QStringLiteral("1"));
    const QString roff = val(QStringLiteral("roff"), QStringLiteral("1e12"));
    QStringList out;
    if (num(QStringLiteral("level"), 1) == 2) {
        // Level 2 (documented): g(Vc) = exp(A*atan((Vc-Vt)/|Vh|) + B), A = ln(Roff/Ron)/pi,
        // B = ln(1/(Roff*Ron))/2, and I = Ilimit*tanh(g*V/Ilimit) with Ilimit defaulting to 10A
        // (the help writes tanh(g*V); the current must be normalized for the limit to make sense).
        // Vser or "oneway" make it conduct only from n2 to n1 (as the op-amp macromodels' rail
        // switches "S1 out V+ ..." and "S2 V- out ..." require), once V(n2,n1) exceeds Vser;
        // epsilon rounds that corner.
        const QString g = QStringLiteral("pow(2.718281828459045,ln(%1/%2)/pi*atan((%3-(%4))/max(abs(%5),1e-12))+ln(1/(%1*%2))/2)").arg(roff, ron, x, vt, vh);
        const QString il = val(QStringLiteral("ilimit"), QStringLiteral("10"));
        if (mp.contains(QStringLiteral("vser")) || mp.contains(QStringLiteral("oneway"))) {
            // A zero Vser (often a parameter, e.g. Vser={Rail}) leaves the switch bidirectional.
            const QString vs = val(QStringLiteral("vser"), QStringLiteral("0"));
            const QString eps = QStringLiteral("max(abs(%1),1e-9)").arg(val(QStringLiteral("epsilon"), QStringLiteral("0")));
            const QString u = QStringLiteral("(V(%1,%2)-(%3))").arg(b, a, vs);
            const QString veff = QStringLiteral("(%1<=0 ? 0 : (%1>=%2 ? %1-%2/2 : %1*%1/(2*%2)))").arg(u, eps);
            const QString oneway = QStringLiteral("%1*tanh(%2*%3/%1)").arg(il, g, veff);
            const QString both = QStringLiteral("%1*tanh(%2*V(%3,%4)/%1)").arg(il, g, b, a);
            out << QStringLiteral("B%1 %2 %3 I=%4").arg(name, b, a,
                                                        mp.contains(QStringLiteral("oneway")) ? oneway
                                                                                             : QStringLiteral("((%1) != 0 ? %2 : %3)").arg(vs, oneway, both));
        } else {
            out << QStringLiteral("B%1 %2 %3 I=%4*tanh(%5*V(%2,%3)/%4)").arg(name, a, b, il, g);
        }
        return out;
    }
    QString st; // 0..1 switch state
    if (vhNegative || num(k == 'S' ? QStringLiteral("vh") : QStringLiteral("ih"), 0) == 0) {
        // Smooth transition between vt-|vh| and vt+|vh| (a tiny window when vh=0).
        const QString w = QStringLiteral("max(abs(%1),1e-6)").arg(vh);
        const QString u = QStringLiteral("min(max((%1-(%2)+%3)/(2*%3),0),1)").arg(x, vt, w);
        st = QStringLiteral("(%1*%1*(3-2*%1))").arg(u);
    } else {
        // Hysteresis: latched state held on a small RC.
        const QString n1 = internalNode(t[0], 41), n2 = internalNode(t[0], 42);
        out << QStringLiteral("B%1_st %2 0 V=(%3 > %4+%5 ? 1 : (%3 < %4-%5 ? 0 : (V(%6) > 0.5 ? 1 : 0)))").arg(name, n1, x, vt, vh, n2);
        out << QStringLiteral("R%1_st %2 %3 1k").arg(name, n1, n2);
        out << QStringLiteral("C%1_st %2 0 1p").arg(name, n2); // 1ns: see stateNode in translateDigital
        st = QStringLiteral("V(%1)").arg(n2);
    }
    // Conductance interpolated logarithmically between Goff and Gon.
    const QString g = QStringLiteral("pow(2.718281828459045,ln(1/%1)+%2*(ln(1/%3)-ln(1/%1)))").arg(roff, st, ron);
    const QString v = QStringLiteral("V(%1,%2)").arg(a, b);
    QString veff = v;
    if (mp.contains(QStringLiteral("vser"))) {
        const QString vs = val(QStringLiteral("vser"), QStringLiteral("0"));
        veff = QStringLiteral("(%1 > %2 ? %1-%2 : (%1 < -(%2) ? %1+%2 : 0))").arg(v, vs);
    }
    if (mp.contains(QStringLiteral("oneway")))
        veff = QStringLiteral("max(%1,0)").arg(veff);
    QString ion = QStringLiteral("(%1-1/%2)*%3").arg(g, roff, veff);
    if (mp.contains(QStringLiteral("ilimit"))) {
        const QString il = val(QStringLiteral("ilimit"), QStringLiteral("1e30"));
        ion = QStringLiteral("%1*tanh((%2)/%1)").arg(il, ion);
    }
    out << QStringLiteral("B%1 %2 %3 I=%4+%5/%6").arg(name, a, b, ion, v, roff);
    if (mp.contains(QStringLiteral("lser")))
        m_res.warnings << QStringLiteral("%1: switch series inductance (Lser) is ignored").arg(t[0]);
    return out;
}

QStringList NetlistTranslator::translateIdealDiode(const QStringList &t, const QString &modelLine)
{
    QHash<QString, QString> mp;
    QString body = modelLine;
    const int open = body.indexOf('(');
    if (open >= 0)
        body = body.mid(open + 1, body.lastIndexOf(')') - open - 1);
    else
        body = body.section(QRegularExpression(QStringLiteral("\\s+")), 3);
    for (const QString &tok : tokenize(normalizeEquals(body).replace(',', ' '))) {
        const int eq = tok.indexOf('=');
        if (eq > 0)
            mp.insert(tok.left(eq).toLower(), tok.mid(eq + 1));
    }
    auto val = [&](const QString &key, const QString &def) {
        QString v = mp.value(key, def);
        if (units::isBraced(v))
            v = v.mid(1, v.size() - 2);
        return QStringLiteral("{%1}").arg(rewriteExpression(v));
    };
    const QString name = elementName(t[0]);
    const QString a = nodeToken(t[1]), c = nodeToken(t[2]);
    // Instance multiplicity (m=) and series count (n=).
    QString m = QStringLiteral("1"), n = QStringLiteral("1");
    for (int i = 4; i < t.size(); ++i) {
        const QString lp = t[i].toLower();
        if (lp.startsWith(QLatin1String("m="))) m = t[i].mid(2);
        else if (lp.startsWith(QLatin1String("n="))) n = t[i].mid(2);
        else if (isNumeric(t[i])) m = t[i]; // area
    }
    const QString ron = val(QStringLiteral("ron"), QStringLiteral("1"));
    const QString roff = val(QStringLiteral("roff"), QStringLiteral("1e12"));
    const QString vfwd = val(QStringLiteral("vfwd"), QStringLiteral("0"));
    const QString vrev = val(QStringLiteral("vrev"), QStringLiteral("1e30"));
    const QString rrev = mp.contains(QStringLiteral("rrev")) ? val(QStringLiteral("rrev"), QStringLiteral("1")) : ron;
    const QString eps = QStringLiteral("max(abs(%1),1e-9)").arg(val(QStringLiteral("epsilon"), QStringLiteral("0")));
    const QString reps = QStringLiteral("max(abs(%1),1e-9)").arg(val(QStringLiteral("revepsilon"), QStringLiteral("0")));
    const QString v = QStringLiteral("(V(%1,%2)/(%3))").arg(a, c, n);
    // Smooth ramp: 0 below 0, quadratic over [0, e], linear above.
    auto ramp = [](const QString &x, const QString &e) {
        return QStringLiteral("((%1)<=0 ? 0 : ((%1)>=%2 ? (%1)-%2/2 : (%1)*(%1)/(2*%2)))").arg(x, e);
    };
    QString fwd = QStringLiteral("(1/%1-1/%2)*%3").arg(ron, roff, ramp(QStringLiteral("%1-%2").arg(v, vfwd), eps));
    QString rev = QStringLiteral("(1/%1-1/%2)*%3").arg(rrev, roff, ramp(QStringLiteral("-%1-%2").arg(v, vrev), reps));
    if (mp.contains(QStringLiteral("ilimit"))) {
        const QString il = val(QStringLiteral("ilimit"), QStringLiteral("1e30"));
        fwd = QStringLiteral("%1*tanh((%2)/%1)").arg(il, fwd);
    }
    if (mp.contains(QStringLiteral("revilimit"))) {
        const QString il = val(QStringLiteral("revilimit"), QStringLiteral("1e30"));
        rev = QStringLiteral("%1*tanh((%2)/%1)").arg(il, rev);
    }
    return {QStringLiteral("B%1 %2 %3 I=(%4)*(%5-%6+%7/%8)").arg(name, a, c, m, fwd, rev, v, roff)};
}

QStringList NetlistTranslator::laplaceSource(const QString &name, QChar kind, const QString &n1, const QString &n2, const QString &input,
                                             const QString &line)
{
    // Extract the expression after "Laplace=" (up to an LTspice-only option or end of line).
    static const QRegularExpression re(QStringLiteral("laplace\\s*=\\s*(.*?)(\\s+(window|nfft|mtol|cache)\\s*=.*)?$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(line);
    if (!m.hasMatch()) {
        m_res.errors << QStringLiteral("%1: cannot read Laplace expression").arg(name);
        return {};
    }
    QString expr = m.captured(1).trimmed();
    if (units::isBraced(expr))
        expr = expr.mid(1, expr.size() - 2);
    g_params = &m_res.params;
    QString err;
    const auto tf = laplace::parse(expr, [](const QString &id) -> std::optional<double> {
        if (!g_params || !g_params->contains(id.toLower()))
            return std::nullopt;
        const double v = evalParamExpr(g_params->value(id.toLower()));
        if (std::isnan(v))
            return std::nullopt;
        return v;
    }, &err);
    if (!tf) {
        m_res.errors << QStringLiteral("%1: %2").arg(name, err);
        return {};
    }
    if (tf->num.size() > tf->den.size()) {
        m_res.errors << QStringLiteral("%1: improper Laplace transfer function (numerator order > denominator order)").arg(name);
        return {};
    }
    // Realize H(s) = N(s)/D(s) in controllable canonical form with frequency
    // scaling (s = wc*p) so coefficients stay well conditioned. Each state is
    // the voltage of a 1F capacitor driven by a behavioral current, so the DC
    // operating point and AC response follow from ordinary circuit elements.
    const int n = int(tf->den.size()) - 1; // den is monic (normalized by the parser)
    QStringList out;
    QString u = input;
    if (tf->delay > 0) {
        // Pure delay: ideal transmission line, driven by a buffer and matched at its end.
        const QString a = internalNode(name, 61), d = internalNode(name, 62);
        out << QStringLiteral("B%1_din %2 0 V=%3").arg(name, a, input);
        out << QStringLiteral("T%1_del %2 0 %3 0 Z0=1 Td=%4").arg(name, a, d).arg(tf->delay, 0, 'g', 15);
        out << QStringLiteral("R%1_del %2 0 1").arg(name, d);
        u = QStringLiteral("V(%1)").arg(d);
    }
    auto num = [](double v) { return QString::number(v, 'g', 15); };
    // Fractional-order sections: (1+s/z)/(1+s/p) = p/z + (1-p/z)/(1+s/p), using
    // a 1-ohm/(1/p)-farad lag per section and a buffer node between sections.
    for (int k = 0; k < tf->sections.size(); ++k) {
        const double z = tf->sections[k].first, p = tf->sections[k].second;
        const QString lag = internalNode(name, 100 + 2 * k), buf = internalNode(name, 101 + 2 * k);
        out << QStringLiteral("B%1_f%2 0 %3 I=%4-V(%3)").arg(name).arg(k).arg(lag, u);
        out << QStringLiteral("C%1_f%2 %3 0 %4").arg(name).arg(k).arg(lag, num(1.0 / p));
        out << QStringLiteral("B%1_g%2 %3 0 V=%4*(%5)+%6*V(%7)").arg(name).arg(k).arg(buf, num(p / z), u, num(1.0 - p / z), lag);
        u = QStringLiteral("V(%1)").arg(buf);
    }
    QString yexpr;
    if (n == 0) {
        yexpr = QStringLiteral("%1*(%2)").arg(num(tf->num.value(0) / tf->den[0]), u);
    } else {
        double wc = std::pow(std::fabs(tf->den[0]), 1.0 / n);
        if (!(wc > 0) || !std::isfinite(wc))
            wc = 1.0; // e.g. pure integrators (a0 = 0)
        QVector<double> a(n), b(n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            a[i] = tf->den[i] / std::pow(wc, n - i);
        for (int i = 0; i < tf->num.size(); ++i)
            b[i] = tf->num[i] / std::pow(wc, n - i);
        QStringList states;
        for (int k = 0; k < n; ++k)
            states << internalNode(name, 70 + k);
        for (int k = 0; k < n; ++k) {
            QString deriv;
            if (k < n - 1) {
                deriv = QStringLiteral("V(%1)").arg(states[k + 1]);
            } else {
                deriv = QStringLiteral("(%1)").arg(u);
                for (int i = 0; i < n; ++i)
                    if (a[i] != 0)
                        deriv += QStringLiteral("-(%1)*V(%2)").arg(num(a[i]), states[i]);
            }
            out << QStringLiteral("B%1_s%2 0 %3 I=%4*(%5)").arg(name).arg(k).arg(states[k], num(wc), deriv);
            out << QStringLiteral("C%1_s%2 %3 0 1").arg(name).arg(k).arg(states[k]);
        }
        QStringList terms;
        for (int i = 0; i < n; ++i) {
            const double c = b[i] - b[n] * a[i];
            if (c != 0)
                terms << QStringLiteral("(%1)*V(%2)").arg(num(c), states[i]);
        }
        if (b[n] != 0)
            terms << QStringLiteral("(%1)*(%2)").arg(num(b[n]), u);
        yexpr = terms.isEmpty() ? QStringLiteral("0") : terms.join('+');
    }
    out << QStringLiteral("B%1_lap %2 %3 %4=%5").arg(name, n1, n2, kind == 'G' ? QStringLiteral("I") : QStringLiteral("V"), yexpr);
    return out;
}

QStringList NetlistTranslator::translateMutual(const QStringList &t)
{
    // K1 L1 L2 [L3 ...] coupling
    if (t.size() < 4)
        return {t.join(' ')};
    const QString coupling = rewriteExpression(t.last());
    // Schematic inductors are netlisted as "L§T1a" but coupled as "K1 T1a ...".
    QSet<QString> defined;
    auto collect = [&defined](const QStringList &lines) {
        for (const QString &l : lines)
            if (l.startsWith('L', Qt::CaseInsensitive))
                defined.insert(l.section(' ', 0, 0).toLower());
    };
    collect(m_topElements);
    for (const Subckt &sc : m_subckts)
        collect(sc.body);
    QStringList inductors;
    for (int i = 1; i < t.size() - 1; ++i) {
        const QString prefixed = QStringLiteral("L§") + t[i];
        inductors << elementName(!defined.contains(t[i].toLower()) && defined.contains(prefixed.toLower()) ? prefixed : t[i]);
    }
    if (inductors.size() == 2)
        return {QStringLiteral("%1 %2 %3 %4").arg(elementName(t[0]), inductors[0], inductors[1], coupling)};
    QStringList out;
    int idx = 0;
    for (int i = 0; i < inductors.size(); ++i)
        for (int j = i + 1; j < inductors.size(); ++j)
            out << QStringLiteral("%1_%2 %3 %4 %5").arg(elementName(t[0])).arg(++idx).arg(inductors[i], inductors[j], coupling);
    return out;
}

QString NetlistTranslator::translateModel(const QString &lineIn)
{
    QString line = normalizeEquals(lineIn);
    line.replace(QChar(0xB5), 'u');
    // AKO: inherit from another model.
    static const QRegularExpression ako(QStringLiteral("^\\.model\\s+(\\S+)\\s+ako:\\s*(\\S+)\\s+([A-Za-z]+)\\s*\\(?(.*?)\\)?\\s*$"),
                                        QRegularExpression::CaseInsensitiveOption);
    const auto am = ako.match(line);
    if (am.hasMatch()) {
        QString baseParams;
        const QString base = am.captured(2).toLower();
        auto findBase = [&](const QStringList &lines) -> QString {
            for (const QString &l : lines) {
                const QStringList tt = l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (tt.size() > 2 && tt[0].compare(QLatin1String(".model"), Qt::CaseInsensitive) == 0 && tt[1].toLower() == base) {
                    const int open = l.indexOf('(');
                    return open >= 0 ? l.mid(open + 1, l.lastIndexOf(')') - open - 1) : l.section(' ', 3);
                }
            }
            return QString();
        };
        baseParams = findBase(m_models);
        if (baseParams.isEmpty())
            baseParams = findBase(m_pulledDefs);
        line = QStringLiteral(".model %1 %2(%3 %4)").arg(am.captured(1), am.captured(3), baseParams, am.captured(4));
    }
    // Remove string-valued metadata and LTspice-only rating parameters that can confuse ngspice.
    static const QRegularExpression meta(QStringLiteral("\\s*\\b(mfg|type|pn)\\s*=\\s*(\"[^\"]*\"|\\S+?)(?=[\\s)]|$)"),
                                         QRegularExpression::CaseInsensitiveOption);
    line.replace(meta, QString());
    static const QRegularExpression ratings(QStringLiteral("\\s*\\b(vceo|icrating|vds|qg|iave|vpk|ipk|ron_rating|pdiss)\\s*=\\s*[^\\s)]+"),
                                            QRegularExpression::CaseInsensitiveOption);
    line.replace(ratings, QString());
    // IGBTs are expanded into behavioral elements by translateIgbt(); ngspice has no such model type.
    static const QRegularExpression igbtRe(QStringLiteral("^\\.model\\s+\\S+\\s+[NP]IGBT\\b"), QRegularExpression::CaseInsensitiveOption);
    if (igbtRe.match(line).hasMatch())
        return QStringLiteral("* ") + line;
    // Model type aliases.
    static const QRegularExpression typeRe(QStringLiteral("^(\\.model\\s+\\S+\\s+)(LPNP)\\b"), QRegularExpression::CaseInsensitiveOption);
    line.replace(typeRe, QStringLiteral("\\1PNP"));
    // LTspice's VDMOS has no gate or body-diode capacitance unless given;
    // ngspice's defaults are non-zero.
    {
        static const QRegularExpression vd(QStringLiteral("^\\.model\\s+\\S+\\s+VDMOS\\b"), QRegularExpression::CaseInsensitiveOption);
        if (vd.match(line).hasMatch()) {
            QStringList add;
            for (const char *k : {"cgdmax", "cgdmin", "cgs", "cjo"})
                if (!line.contains(QRegularExpression(QStringLiteral("\\b%1\\s*=").arg(QLatin1String(k)), QRegularExpression::CaseInsensitiveOption)))
                    add << QStringLiteral("%1=0").arg(QLatin1String(k));
            if (!add.isEmpty()) {
                if (line.contains(')'))
                    line.insert(line.lastIndexOf(')'), ' ' + add.join(' '));
                else
                    line += QStringLiteral(" (%1)").arg(add.join(' '));
            }
        }
    }
    // A diode with Is=0 conducts no forward current in LTspice; ngspice clamps Is to 1e-28, which still
    // conducts at ~1.5V. Keep the clamp but make the exponential negligible with a huge emission coefficient.
    {
        static const QRegularExpression dm(QStringLiteral("^\\.model\\s+\\S+\\s+D\\b"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression is0(QStringLiteral("\\bis\\s*=\\s*0+\\.?0*(?=[\\s)]|$)"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression nParam(QStringLiteral("\\s*\\bn\\s*=\\s*[^\\s)]+"), QRegularExpression::CaseInsensitiveOption);
        if (dm.match(line).hasMatch() && is0.match(line).hasMatch()) {
            line.replace(nParam, QString());
            line.replace(is0, QStringLiteral("is=1e-28 n=1000"));
        }
        // LTspice's default breakdown current is 1e-10 (SPICE3/ngspice: 1e-3), which sets where a zener sits.
        static const QRegularExpression bv(QStringLiteral("\\bbv\\s*="), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression ibv(QStringLiteral("\\bibv\\s*="), QRegularExpression::CaseInsensitiveOption);
        if (dm.match(line).hasMatch() && bv.match(line).hasMatch() && !ibv.match(line).hasMatch()) {
            if (line.contains(')'))
                line.insert(line.lastIndexOf(')'), QStringLiteral(" ibv=1e-10"));
            else
                line += QStringLiteral(" ibv=1e-10");
        }
    }
    // Step overrides of model parameters.
    const QStringList mt =line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (mt.size() > 1) {
        const QStringList ov = m_stepModelOverrides.value(mt[1].toLower());
        for (const QString &kv : ov) {
            const QString key = kv.section('=', 0, 0), val = kv.section('=', 1);
            const QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*[^\\s)]+").arg(QRegularExpression::escape(key)), QRegularExpression::CaseInsensitiveOption);
            if (re.match(line).hasMatch())
                line.replace(re, kv);
            else if (line.contains(')'))
                line.insert(line.lastIndexOf(')'), ' ' + kv);
            else
                line += ' ' + kv;
        }
    }
    return rewriteExpression(line);
}

QString NetlistTranslator::translateAnalysis(const QString &line)
{
    g_params = &m_res.params;
    QStringList t = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QString kw = t[0].toLower();
    if (kw == QLatin1String(".fra")) {
        // .fra [Tstart=] [dTmax=] [Tstep=] [Tstop=] [uic] [startup]: a transient that ends when
        // every FRA device has finished its sweep.
        double tstart = 0, dtmax = 0, tstep = 0, tstop = 0;
        QStringList flags;
        for (int i = 1; i < t.size(); ++i) {
            const QString lt = t[i].toLower();
            const int eq = lt.indexOf('=');
            if (eq > 0) {
                const QString k = lt.left(eq);
                const double v = numberOr(t[i].mid(eq + 1), 0);
                if (k == QLatin1String("tstart")) tstart = v;
                else if (k == QLatin1String("dtmax")) dtmax = v;
                else if (k == QLatin1String("tstep")) tstep = v;
                else if (k == QLatin1String("tstop")) tstop = v;
            } else if (lt == QLatin1String("uic")) {
                flags << QStringLiteral("uic");
            } else if (lt == QLatin1String("startup")) {
                m_startup = true;
            }
        }
        if (tstop <= 0)
            for (const fra::Analyzer &a : m_res.fraAnalyzers)
                tstop = std::max(tstop, a.stopTime());
        if (tstop <= 0)
            return QStringLiteral(".tran 1e-9 1e-6"); // placeholder until the FRA devices are known
        if (tstep <= 0)
            tstep = (tstop - tstart) / 1000.0;
        QString out = QStringLiteral(".tran %1 %2 %3").arg(tstep, 0, 'g', 12).arg(tstop, 0, 'g', 12).arg(tstart, 0, 'g', 12);
        if (dtmax > 0)
            out += QStringLiteral(" %1").arg(dtmax, 0, 'g', 12);
        if (!flags.isEmpty())
            out += ' ' + flags.join(' ');
        return out;
    }
    if (kw == QLatin1String(".tran")) {
        QStringList nums;
        QStringList flags;
        for (int i = 1; i < t.size(); ++i) {
            const QString lt = t[i].toLower();
            if (lt == QLatin1String("uic")) flags << QStringLiteral("uic");
            else if (lt == QLatin1String("startup")) m_startup = true;
            else if (lt == QLatin1String("steady") || lt == QLatin1String("nodiscard") || lt == QLatin1String("step"))
                m_res.warnings << QStringLiteral(".tran modifier \"%1\" is not supported and was ignored").arg(t[i]);
            else if (lt.contains('=')) continue;
            else nums << t[i];
        }
        double tstep = 0, tstop = 0, tstart = 0, tmax = 0;
        if (nums.size() == 1) {
            tstop = numberOr(nums[0], 0);
        } else if (nums.size() >= 2) {
            tstep = numberOr(nums[0], 0);
            tstop = numberOr(nums[1], 0);
            if (nums.size() >= 3) tstart = numberOr(nums[2], 0);
            if (nums.size() >= 4) tmax = numberOr(nums[3], 0);
        }
        if (tstop <= 0) {
            m_res.errors << QStringLiteral("Invalid .tran stop time: %1").arg(line);
            return QString();
        }
        if (tmax <= 0 && m_tranMaxStep > 0)
            tmax = m_tranMaxStep;
        if (tstep <= 0)
            tstep = (tstop - tstart) / 1024.0;
        // LTspice's maximum step is Tstop/1024 unless given; unlike ngspice it is not limited by the
        // time step argument (".tran 1u 100m" still steps up to 97.7us). Measured on its raw files.
        if (tmax <= 0)
            tmax = tstop / 1024.0;
        // Device limits only ever tighten the step.
        if (m_deviceMaxStep > 0) {
            const double effective = tmax;
            if (m_deviceMaxStep < effective)
                tmax = m_deviceMaxStep;
        }
        QString out = QStringLiteral(".tran %1 %2").arg(tstep, 0, 'g', 12).arg(tstop, 0, 'g', 12);
        if (tstart > 0 || tmax > 0)
            out += QStringLiteral(" %1").arg(tstart, 0, 'g', 12);
        if (tmax > 0)
            out += QStringLiteral(" %1").arg(tmax, 0, 'g', 12);
        if (!flags.isEmpty())
            out += ' ' + flags.join(' ');
        return out;
    }
    if (kw == QLatin1String(".ac")) {
        if (t.size() >= 3 && t[1].compare(QLatin1String("list"), Qt::CaseInsensitive) == 0) {
            // ngspice has no list sweep: one frequency maps to "lin 1 f f"; several to a log sweep across them.
            QList<double> fs;
            for (int i = 2; i < t.size(); ++i)
                fs << numberOr(t[i], 0);
            std::sort(fs.begin(), fs.end());
            if (fs.size() == 1)
                return QStringLiteral(".ac lin 1 %1 %1").arg(fs[0], 0, 'g', 12);
            m_res.warnings << QStringLiteral(".ac list is approximated by a logarithmic sweep over the listed range");
            return QStringLiteral(".ac dec 50 %1 %2").arg(fs.first(), 0, 'g', 12).arg(fs.last(), 0, 'g', 12);
        }
        return t.join(' ');
    }
    if (kw == QLatin1String(".dc")) {
        QStringList parts = t.mid(1);
        if (!parts.isEmpty() && QStringList{QStringLiteral("lin"), QStringLiteral("dec"), QStringLiteral("oct")}.contains(parts[0].toLower())) {
            if (parts[0].toLower() != QLatin1String("lin"))
                m_res.warnings << QStringLiteral(".dc %1 sweeps are approximated with linear steps").arg(parts[0]);
            parts.removeFirst();
        }
        if (!parts.isEmpty() && parts[0].compare(QLatin1String("param"), Qt::CaseInsensitive) == 0) {
            m_res.errors << QStringLiteral(".dc param sweeps are not supported; use .step param with .op instead");
            return QString();
        }
        if (!parts.isEmpty()) {
            m_res.dcSweepSource = parts[0];
            parts[0] = elementName(parts[0]);
        }
        return QStringLiteral(".dc %1").arg(parts.join(' '));
    }
    if (kw == QLatin1String(".op"))
        return QStringLiteral(".op");
    if (kw == QLatin1String(".noise")) {
        // .noise V(out[,ref]) src dec pts fstart fstop
        if (t.size() >= 3)
            t[2] = elementName(t[2]);
        if (t.size() >= 5 && t[3].compare(QLatin1String("list"), Qt::CaseInsensitive) == 0) {
            QList<double> fs;
            for (int i = 4; i < t.size(); ++i)
                fs << numberOr(t[i], 0);
            std::sort(fs.begin(), fs.end());
            const QString head = t.mid(0, 3).join(' ');
            if (fs.size() == 1)
                return QStringLiteral("%1 lin 1 %2 %2").arg(head).arg(fs[0], 0, 'g', 12);
            return QStringLiteral("%1 dec 50 %2 %3").arg(head).arg(fs.first(), 0, 'g', 12).arg(fs.last(), 0, 'g', 12);
        }
        return t.join(' ');
    }
    if (kw == QLatin1String(".tf")) {
        if (t.size() >= 3)
            t[2] = elementName(t[2]);
        return t.join(' ');
    }
    return QString();
}

void NetlistTranslator::handleOptions(const QString &line)
{
    const QStringList t = normalizeEquals(line).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).mid(1);
    static const QSet<QString> passthrough = {QStringLiteral("abstol"), QStringLiteral("reltol"), QStringLiteral("vntol"), QStringLiteral("chgtol"),
                                              QStringLiteral("trtol"), QStringLiteral("gmin"), QStringLiteral("itl1"), QStringLiteral("itl2"),
                                              QStringLiteral("itl4"), QStringLiteral("itl5"), QStringLiteral("temp"), QStringLiteral("tnom"),
                                              QStringLiteral("cshunt"), QStringLiteral("maxord"), QStringLiteral("gminsteps"), QStringLiteral("srcsteps"),
                                              QStringLiteral("noopiter"), QStringLiteral("pivtol"), QStringLiteral("pivrel"), QStringLiteral("rshunt"),
                                              QStringLiteral("method"), QStringLiteral("xmu"), QStringLiteral("defad"), QStringLiteral("defas"),
                                              QStringLiteral("defl"), QStringLiteral("defw"), QStringLiteral("scale")};
    QStringList kept;
    for (const QString &opt : t) {
        const QString key = opt.section('=', 0, 0).toLower();
        const QString val = opt.section('=', 1);
        if (key == QLatin1String("maxstep")) {
            m_tranMaxStep = numberOr(val, 0);

        } else if (key == QLatin1String("meascplxfmt")) {
            m_res.measComplexFormat = val.toLower();
        } else if (key == QLatin1String("thev_induc")) {
            m_thevInduc = numberOr(val, 0) != 0;
        } else if (key == QLatin1String("method")) {
            QString v = val.toLower();
            if (v == QLatin1String("modtrap"))
                v = QStringLiteral("trap");
            kept << QStringLiteral("method=%1").arg(v);
        } else if (passthrough.contains(key)) {
            kept << opt;
        }
        // Everything else (plotwinsize, numdgt, ...) is LTspice specific and ignored.
    }
    if (!kept.isEmpty())
        m_optionLines << QStringLiteral(".options %1").arg(kept.join(' '));
}

void NetlistTranslator::disambiguateModels(QStringList &out)
{
    // LTspice resolves a model by name *and* device type, so libraries may
    // define e.g. ".model N NPN" and ".model N VDMOS" side by side. ngspice
    // cannot, so give each type its own name within the scope.
    struct Scope {
        int begin;
        int end;
    };
    QList<Scope> scopes;
    QList<int> stack;
    scopes.append({0, int(out.size())});
    for (int i = 0; i < out.size(); ++i) {
        const QString low = out[i].trimmed().toLower();
        if (low.startsWith(QLatin1String(".subckt")))
            stack.push_back(i);
        else if (low.startsWith(QLatin1String(".ends")) && !stack.isEmpty())
            scopes.append({stack.takeLast(), i});
    }
    static const QRegularExpression modelRe(QStringLiteral("^\\.model\\s+(\\S+)\\s+([A-Za-z]+)"), QRegularExpression::CaseInsensitiveOption);
    auto typesFor = [](QChar kind) -> QStringList {
        switch (kind.toUpper().unicode()) {
        case 'Q': return {QStringLiteral("NPN"), QStringLiteral("PNP"), QStringLiteral("LPNP")};
        case 'M': return {QStringLiteral("NMOS"), QStringLiteral("PMOS"), QStringLiteral("VDMOS")};
        case 'J': return {QStringLiteral("NJF"), QStringLiteral("PJF")};
        case 'D': return {QStringLiteral("D")};
        case 'Z': return {QStringLiteral("NMF"), QStringLiteral("PMF")};
        case 'S': return {QStringLiteral("SW"), QStringLiteral("VSWITCH")};
        case 'W': return {QStringLiteral("CSW"), QStringLiteral("ISWITCH")};
        default: return {};
        }
    };
    for (const Scope &sc : scopes) {
        // Only direct members of this scope (skip nested subcircuits).
        QList<int> members;
        int depth = 0;
        for (int i = sc.begin; i < sc.end; ++i) {
            const QString low = out[i].trimmed().toLower();
            if (i != sc.begin && low.startsWith(QLatin1String(".subckt"))) {
                ++depth;
                continue;
            }
            if (low.startsWith(QLatin1String(".ends")) && depth > 0) {
                --depth;
                continue;
            }
            if (depth == 0 && i != sc.begin)
                members << i;
        }
        QHash<QString, QStringList> types; // lower name -> types
        for (int i : members) {
            const auto m = modelRe.match(out[i].trimmed());
            if (m.hasMatch() && !types[m.captured(1).toLower()].contains(m.captured(2).toUpper()))
                types[m.captured(1).toLower()] << m.captured(2).toUpper();
        }
        for (auto it = types.constBegin(); it != types.constEnd(); ++it) {
            if (it.value().size() < 2)
                continue;
            const QString name = it.key();
            for (int i : members) {
                QString &l = out[i];
                const auto m = modelRe.match(l.trimmed());
                if (m.hasMatch()) {
                    if (m.captured(1).toLower() == name)
                        l.replace(m.capturedStart(1) + (l.size() - l.trimmed().size()), m.capturedLength(1),
                                  QStringLiteral("%1__%2").arg(m.captured(1), m.captured(2).toLower()));
                    continue;
                }
                if (l.trimmed().startsWith('.') || l.trimmed().isEmpty())
                    continue;
                const QStringList cand = typesFor(l.trimmed()[0]);
                QString chosen;
                for (const QString &ty : it.value())
                    if (cand.contains(ty)) {
                        chosen = ty;
                        break;
                    }
                if (chosen.isEmpty())
                    continue;
                // MOS: prefer the type matching the instance (VDMOS for 3 nodes is ambiguous; keep first match).
                QStringList toks = l.split(' ');
                for (int k = 1; k < toks.size(); ++k) {
                    if (toks[k].toLower() == name) {
                        toks[k] = QStringLiteral("%1__%2").arg(toks[k], chosen.toLower());
                        break;
                    }
                }
                l = toks.join(' ');
            }
        }
    }
}

QStringList NetlistTranslator::floatingNodeShunts(const QStringList &elements)
{
    QHash<QString, QString> parent;
    std::function<QString(const QString &)> find = [&](const QString &n) -> QString {
        QString p = parent.value(n, n);
        if (p == n)
            return n;
        p = find(p);
        parent[n] = p;
        return p;
    };
    auto join = [&](const QString &a, const QString &b) {
        const QString ra = find(a), rb = find(b);
        if (ra != rb)
            parent[ra] = rb;
    };
    QStringList order; // nodes in first-seen order, for deterministic output
    auto see = [&](const QString &n) {
        if (!parent.contains(n)) {
            parent.insert(n, n);
            order << n;
        }
    };
    auto node = [](const QStringList &t, int i) { return t[i].toLower() == QLatin1String("gnd") ? QStringLiteral("0") : t[i].toLower(); };
    see(QStringLiteral("0"));
    for (const QString &l : elements) {
        const QStringList t = tokenize(l);
        if (t.isEmpty() || l.startsWith('*') || l.startsWith('.'))
            continue;
        const QChar k = t[0][0].toUpper();
        auto nodes = [&](int n) {
            QStringList r;
            for (int i = 1; i <= n && i < t.size(); ++i) {
                r << node(t, i);
                see(r.last());
            }
            return r;
        };
        auto chain = [&](const QStringList &ns) {
            for (int i = 1; i < ns.size(); ++i)
                join(ns[0], ns[i]);
        };
        switch (k.unicode()) {
        case 'C':
        case 'I':
        case 'G':
        case 'F':
            nodes(2); // no DC path between the terminals
            break;
        case 'B':
            if (l.contains(QRegularExpression(QStringLiteral("^\\S+\\s+\\S+\\s+\\S+\\s+I\\s*="), QRegularExpression::CaseInsensitiveOption)))
                nodes(2);
            else
                chain(nodes(2));
            break;
        case 'R':
        case 'L':
        case 'V':
        case 'D':
        case 'E':
        case 'H':
        case 'W':
            chain(nodes(2));
            break;
        case 'Q':
        case 'J':
            chain(nodes(3));
            break;
        case 'M': {
            const QStringList ns = nodes(3);
            if (ns.size() == 3)
                join(ns[0], ns[2]);
            break;
        }
        case 'S':
            chain(nodes(2));
            nodes(4);
            break;
        case 'T': {
            const QStringList ns = nodes(4);
            if (ns.size() == 4) {
                join(ns[0], ns[2]);
                join(ns[1], ns[3]);
            }
            break;
        }
        case 'X': {
            // Pins are the tokens before the subcircuit name; treat the subcircuit as conducting between all of them.
            int last = t.size();
            for (int i = 1; i < t.size(); ++i)
                if (t[i].contains('=') || t[i].compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0) {
                    last = i;
                    break;
                }
            QStringList ns = nodes(last - 2);
            chain(ns);
            break;
        }
        case 'K':
            break;
        default:
            // Unknown devices (XSPICE A-elements etc.): assume every node they touch is DC-tied to ground.
            for (int i = 1; i < t.size(); ++i) {
                QString n = t[i].toLower();
                n.remove('[').remove(']').remove('%');
                if (n.isEmpty() || n.contains('=') || n.contains('('))
                    continue;
                see(n);
                join(n, QStringLiteral("0"));
            }
            break;
        }
    }
    QStringList out;
    QSet<QString> done;
    const QString ground = find(QStringLiteral("0"));
    int idx = 0;
    for (const QString &n : order) {
        const QString r = find(n);
        if (r == ground || done.contains(r))
            continue;
        done.insert(r);
        out << QStringLiteral("R__dcpath%1 %2 0 1T").arg(++idx).arg(n);
    }
    return out;
}

QStringList NetlistTranslator::translateIgbt(const QStringList &t, const QString &modelLine, bool pChannel)
{
    // Hefner IGBT (the model LTspice documents for NIGBT/PIGBT): a MOSFET channel feeding the
    // wide n- base of a PNP whose stored charge Q is a state variable.
    //   Imos  = KF*KP*((Vgs-Vt)*Vds - KF*Vds^2/2)/(1+Theta*(Vgs-Vt)) (triode),
    //           KP*(Vgs-Vt)^2/2/(1+Theta*(Vgs-Vt)) (saturation)
    //   dQ/dt = Imos - Q/Tau - Q^2*4*Isne*NB^2/(ni^2*QB^2),  QB = q*A*W*NB
    //   holes collected at the emitter: Imos/b + 4*Dp*Q/W^2,  b = MUN/MUP
    //   anode junction: p0*(p0+NB) = ni^2*exp(Vae/Vt), Q = q*A*L*p0*tanh(W/(2L))
    //   base width W = WB minus the base-collector depletion width; the base resistance is
    //   conductivity modulated by Q. Cgs is linear, Cgd and Cds follow depletion widths.
    QHash<QString, double> mp{{QStringLiteral("agd"), 5e-6}, {QStringLiteral("area"), 1e-5}, {QStringLiteral("cgs"), 1.24e-8},
                              {QStringLiteral("coxd"), 3.5e-8}, {QStringLiteral("jsne"), 6.5e-13}, {QStringLiteral("kf"), 1.0},
                              {QStringLiteral("kp"), 0.38}, {QStringLiteral("mun"), 1500}, {QStringLiteral("mup"), 450},
                              {QStringLiteral("nb"), 2e14}, {QStringLiteral("tau"), 7.1e-6}, {QStringLiteral("theta"), 0.02},
                              {QStringLiteral("vt"), 4.7}, {QStringLiteral("vtd"), 1e-3}, {QStringLiteral("wb"), 9e-5},
                              {QStringLiteral("subthres"), 0.02}};
    {
        QString body = modelLine;
        const int open = body.indexOf('(');
        if (open >= 0)
            body = body.mid(open + 1, body.lastIndexOf(')') - open - 1);
        else
            body = body.section(QRegularExpression(QStringLiteral("\\s+")), 3);
        for (const QString &tok : tokenize(normalizeEquals(body).replace(',', ' '))) {
            const int eq = tok.indexOf('=');
            if (eq > 0 && mp.contains(tok.left(eq).toLower()))
                mp[tok.left(eq).toLower()] = numberOr(tok.mid(eq + 1), mp.value(tok.left(eq).toLower()));
        }
    }
    // Instance: Z C G E model [area] [m=..]
    double areaFactor = 1.0, mult = 1.0;
    for (int i = 5; i < t.size(); ++i) {
        const QString lp = t[i].toLower();
        if (lp.startsWith(QLatin1String("m=")))
            mult = numberOr(t[i].mid(2), 1.0);
        else if (lp.startsWith(QLatin1String("area=")))
            areaFactor = numberOr(t[i].mid(5), 1.0);
        else if (isNumeric(t[i]))
            areaFactor = numberOr(t[i], 1.0);
    }
    const double q = 1.602176634e-19, vth = 1.380649e-23 * 300.15 / q, eps = 11.7 * 8.854187817e-14, ni = 1.45e10;
    const double A = mp["area"] * 1e4 * areaFactor * mult; // cm^2
    const double agd = std::min(mp["agd"] * 1e4 * areaFactor * mult, A);
    const double nb = mp["nb"], wb = mp["wb"] * 100, b = mp["mun"] / mp["mup"];
    const double dn = mp["mun"] * vth, dp = mp["mup"] * vth, damb = 2 * dn * dp / (dn + dp);
    const double l = std::sqrt(damb * mp["tau"]);
    const double kdep = std::sqrt(2 * eps / (q * nb));
    const double isne = mp["jsne"] * A;
    const double kc = 4 * isne * nb * nb / (ni * ni * std::pow(q * A * nb, 2)); // c = kc/W^2
    const double s = pChannel ? -1.0 : 1.0;
    auto num = [](double v) { return QString::number(v, 'g', 12); };
    // Smooth max(u,0): square roots of depletion voltages need a finite derivative at the corner.
    auto pos = [](const QString &u) { return QStringLiteral("(0.5*((%1)+sqrt((%1)*(%1)+1e-4)))").arg(u); };

    const QString name = elementName(t[0]);
    const QString c = nodeToken(t[1]), g = nodeToken(t[2]), e = nodeToken(t[3]);
    const QString a = internalNode(t[0], 81), d = internalNode(t[0], 82), nvae = internalNode(t[0], 83);
    // Helper nodes hold shared subexpressions (ngspice differentiates each behavioral expression
    // symbolically, so repeating them makes every evaluation far slower).
    const QString nvgt = internalNode(t[0], 84), nim = internalNode(t[0], 85), nw = internalNode(t[0], 86), nq = internalNode(t[0], 87);
    const QString sg = num(s);
    const QString vgs = QStringLiteral("(%1*V(%2,%3))").arg(sg, g, e);
    const QString vds = QStringLiteral("(%1*V(%2,%3))").arg(sg, d, e);
    // The node holds W in um; floored where used, since Newton starts every node at 0V.
    const QString w = QStringLiteral("(max(V(%1),%2)*1e-4)").arg(nw, num(wb * 0.01 * 1e4));
    const QString qs = QStringLiteral("(V(%1)*1e-6)").arg(nq); // node holds Q in uC
    // Q clamped positive smoothly where it must be (carrier density, conductivity): a hard max()
    // kink stalls Newton exactly where the off-state solution sits.
    const QString qq = QStringLiteral("(0.5*(V(%1)+sqrt(V(%1)*V(%1)+1e-30))*1e-6)").arg(nq);
    const double sub = std::max(mp["subthres"], 1e-4);
    QStringList out;
    // Base width W = WB minus the base-collector depletion width (kept above 1% of WB).
    out << QStringLiteral("B%1_w %2 0 V=1e4*max(%3-%4*sqrt(%5),%6)").arg(name, nw, num(wb), num(kdep), pos(vds + QStringLiteral("+0.6")), num(wb * 0.01));
    // Softplus threshold (subthreshold conduction), written so exp() never sees a large argument.
    const QString y = QStringLiteral("(%1-%2)").arg(vgs, num(mp["vt"]));
    out << QStringLiteral("B%1_vgt %2 0 V=max(%4,0)+%3*ln(1+exp(-abs(%4)/%3))").arg(name, nvgt, num(sub), y);
    const QString vgt = QStringLiteral("V(%1)").arg(nvgt);
    const QString den = QStringLiteral("(1+%1*%2)").arg(num(mp["theta"]), vgt);
    const QString kf = num(mp["kf"]), kp = num(mp["kp"]);
    out << QStringLiteral("B%1_im %2 0 V=(%3>=%4/%5 ? %6*%4*%4/2/%7 : %5*%6*(%4*%3-%5*%3*%3/2)/%7)").arg(name, nim, vds, vgt, kf, kp, den);
    const QString imos = QStringLiteral("V(%1)").arg(nim);
    out << QStringLiteral("B%1_mos %2 %3 I=%4*%5").arg(name, d, e, sg, imos);
    // Base charge Q and anode junction voltage Vae: p0*(p0+NB) = ni^2*exp(Vae/Vt),
    // Q = q*A*L*p0*tanh(W/(2L)), dQ/dt = Imos - Q/Tau - Q^2*4*Isne*NB^2/(ni^2*QB^2).
    // DC sweeps solve for Vae (diode-like conditioning: fast and robust at low Vce); everything
    // else integrates Q on a linear capacitor and derives Vae from it, which is what keeps
    // switching transients stable. Both give the same static solution.
    const bool dcSweep = m_res.analysis == QLatin1String("dc");
    if (dcSweep) {
        // pow() rather than exp(): ngspice caps exp() arguments at 14 in LTspice-compatibility mode;
        // above 60*Vt the exponential continues linearly (as SPICE junction models limit it).
        // Under reverse bias Q no longer depends on Vae, so a 1nS term anchors the node near 0V.
        const QString ev = QStringLiteral("(pow(2.718281828459045,min(V(%1)/%2,60))*(1+max(V(%1)/%2-60,0)))").arg(nvae, num(vth));
        const QString x = QStringLiteral("(%1*(%2-1))").arg(num(ni * ni), ev);
        out << QStringLiteral("B%1_qv %2 0 V=1e6*%3*tanh(%4/%5)*2*%6/(%7+sqrt(%8+4*%6))")
                   .arg(name, nq, num(q * A * l), w, num(2 * l), x, num(nb), num(nb * nb));
        out << QStringLiteral("B%1_bal 0 %2 I=%3-%4/%5-%6*%4*abs(%4)/(%7*%7)-1e-9*V(%2)").arg(name, nvae, imos, qs, num(mp["tau"]), num(kc), w);
        out << QStringLiteral("R%1_bal %2 0 1e15").arg(name, nvae);
        out << QStringLiteral("B%1_ae %2 %3 V=%4*V(%5)").arg(name, c, a, sg, nvae);
    } else {
        out << QStringLiteral("B%1_q 0 %2 I=%3-%4/%5-%6*%4*abs(%4)/(%7*%7)").arg(name, nq, imos, qs, num(mp["tau"]), num(kc), w);
        out << QStringLiteral("C%1_q %2 0 1e-6").arg(name, nq);
        out << QStringLiteral("R%1_q %2 0 1e15").arg(name, nq);
        const QString p0 = QStringLiteral("(%1/(%2*tanh(%3/%4)))").arg(qq, num(q * A * l), w, num(2 * l));
        out << QStringLiteral("B%1_ae %2 %3 V=%4*%5*ln(1+%6*(%6+%7)/%8)").arg(name, c, a, sg, num(vth), p0, num(nb), num(ni * ni));
    }
    // Holes collected at the emitter, and the conductivity-modulated base resistance.
    out << QStringLiteral("B%1_hole %2 %3 I=%4*(%5/%6+%7*%8/(%9*%9))").arg(name, d, e, sg, imos, num(b), num(4 * dp), qs, w);
    out << QStringLiteral("B%1_rb %2 %3 I=V(%2,%3)*%4*(%5*%6+%7*%8/(%4*%9))/%9")
               .arg(name, a, d, num(q * A), num(mp["mun"]), num(nb), num(mp["mun"] + mp["mup"]), qq, w);
    // Capacitances. Nonlinear ones are charge-based: a behavioral source gives Q(V) (in nC) as a
    // voltage, a linear 1nF capacitor turns its rate of change into a current sensed by a 0V
    // source, and a current-controlled source injects it between the device nodes. ngspice's own
    // integrator handles the dynamics (behavioral ddt() is unstable when it dominates a node).
    out << QStringLiteral("C%1_gs %2 %3 %4").arg(name, g, e, num(mp["cgs"] * A));
    auto chargeCap = [&](const QString &tag, int idx, const QString &n1, const QString &n2, const QString &qexpr,
                         const QString &polarity) {
        const QString qx = internalNode(t[0], idx), qy = internalNode(t[0], idx + 1);
        out << QStringLiteral("B%1_%2q %3 0 V=1e9*(%4)").arg(name, tag, qx, qexpr);
        out << QStringLiteral("C%1_%2 %3 %4 1e-9").arg(name, tag, qx, qy);
        out << QStringLiteral("V%1_%2 %3 0 0").arg(name, tag, qy);
        out << QStringLiteral("F%1_%2 %3 %4 V%1_%2 %5").arg(name, tag, n1, n2, polarity);
    };
    // Gate-drain: oxide (Coxd*Agd) when accumulated (Vdg < -Vtd), otherwise in series with the
    // depletion capacitance K/sqrt(Vdg+Vtd): Q = 2K*(s - a*ln(1+s/a)), s = sqrt(Vdg+Vtd), a = K/Cox.
    const double coxd = mp["coxd"] * agd, kgd = eps * agd / kdep, agdRatio = kgd / std::max(coxd, 1e-30);
    const QString vdg = QStringLiteral("(%1*V(%2,%3))").arg(sg, d, g);
    const QString ugd = QStringLiteral("(%1+%2)").arg(vdg, num(mp["vtd"]));
    const QString sgd = QStringLiteral("sqrt(max(%1,0)+1e-12)").arg(ugd);
    chargeCap(QStringLiteral("gd"), 90, d, g,
              QStringLiteral("%1*min(%2,0)+%3*(%4-%5*ln(1+%4/%5))").arg(num(coxd), ugd, num(2 * kgd), sgd, num(agdRatio)), sg);
    // Drain-source (base-collector junction) depletion: Q = 2*K*sqrt(Vds+0.6).
    const QString qdep = QStringLiteral("(%1*sqrt(%2+0.01))").arg(num(2 * eps * std::max(A - agd, 0.0) / kdep), pos(vds + QStringLiteral("+0.6")));
    chargeCap(QStringLiteral("ds"), 92, d, e, qdep, sg);
    // Collector-emitter redistribution (Ccer = Q*Cbcj/(3*QB), QB = q*A*W*NB): stored base charge
    // swept out as the depletion layer widens, which sets the voltage rise rate at turn-off.
    chargeCap(QStringLiteral("cer"), 94, d, e, QStringLiteral("%1/(3*%2*%3)*%4").arg(qq, num(q * A * nb), w, qdep), sg);
    m_res.warnings << QStringLiteral("%1: IGBT simulated with a Hefner-equation behavioral model (static curves match LTspice's closely; "
                                     "switching transients are approximate)").arg(t[0]);
    return out;
}

QString NetlistTranslator::pwlFromWav(const QString &fileIn, int channel)
{
    // LTspice plays a .wav file as a voltage: full scale is +-1V (8-bit samples map to
    // (b-128)/127), sample k at time k/rate.
    QString file = fileIn;
    if (file.startsWith('"') && file.endsWith('"'))
        file = file.mid(1, file.size() - 2);
    QString path = file;
    if (!QFileInfo(path).isAbsolute())
        path = QDir(m_opts.baseDir).filePath(file.replace('\\', '/'));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_res.errors << QStringLiteral("Cannot open wave file \"%1\"").arg(fileIn);
        return QStringLiteral("0 0");
    }
    const QByteArray d = f.readAll();
    auto u16 = [&d](qsizetype i) { return quint16(quint8(d[i]) | quint8(d[i + 1]) << 8); };
    auto u32 = [&u16](qsizetype i) { return quint32(u16(i)) | quint32(u16(i + 2)) << 16; };
    if (d.size() < 12 || !d.startsWith("RIFF") || d.mid(8, 4) != "WAVE") {
        m_res.errors << QStringLiteral("\"%1\" is not a WAV file").arg(fileIn);
        return QStringLiteral("0 0");
    }
    int format = 0, channels = 0, bits = 0;
    quint32 rate = 0;
    qsizetype dataPos = -1, dataLen = 0;
    for (qsizetype pos = 12; pos + 8 <= d.size();) {
        const QByteArray id = d.mid(pos, 4);
        const quint32 len = u32(pos + 4);
        if (id == "fmt " && pos + 24 <= d.size()) {
            format = u16(pos + 8);
            channels = u16(pos + 10);
            rate = u32(pos + 12);
            bits = u16(pos + 22);
            if (format == 0xFFFE && len >= 26 && pos + 34 <= d.size())
                format = u16(pos + 32); // WAVE_FORMAT_EXTENSIBLE: sub-format
        } else if (id == "data") {
            dataPos = pos + 8;
            dataLen = std::min<qsizetype>(len, d.size() - dataPos);
            break;
        }
        pos += 8 + len + (len & 1);
    }
    const int bytes = bits / 8;
    if (dataPos < 0 || rate == 0 || channels <= 0 || bytes <= 0 || (format != 1 && format != 3)) {
        m_res.errors << QStringLiteral("Unsupported WAV format in \"%1\"").arg(fileIn);
        return QStringLiteral("0 0");
    }
    channel = std::clamp(channel, 0, channels - 1);
    const qsizetype frames = dataLen / (qsizetype(bytes) * channels);
    QStringList pts;
    pts.reserve(frames);
    for (qsizetype k = 0; k < frames; ++k) {
        const qsizetype at = dataPos + (k * channels + channel) * bytes;
        double v = 0;
        if (format == 3 && bytes == 4) {
            const quint32 raw = u32(at);
            float fv;
            std::memcpy(&fv, &raw, 4);
            v = fv;
        } else if (bytes == 1) {
            v = (quint8(d[at]) - 128) / 127.0;
        } else {
            qint64 iv = 0;
            for (int b = 0; b < bytes; ++b)
                iv |= qint64(quint8(d[at + b])) << (8 * b);
            const qint64 sign = qint64(1) << (8 * bytes - 1);
            iv = (iv ^ sign) - sign; // sign-extend
            v = double(iv) / double(sign - 1);
        }
        pts << QStringLiteral("%1 %2").arg(double(k) / rate, 0, 'g', 12).arg(v, 0, 'g', 7);
    }
    return pts.isEmpty() ? QStringLiteral("0 0") : pts.join(' ');
}

QStringList NetlistTranslator::translateFraAnalyzer(const QStringList &t)
{
    // "@name in out ..." (Middlebrook voltage injection, or current injection with "zm").
    QString err;
    g_params = &m_res.params;
    const auto a = fra::parseAnalyzer(t, [](const QString &v) { return numberOr(v, 0); }, &err);
    if (!a) {
        m_res.errors << err;
        return {};
    }
    m_res.fraAnalyzers << *a;
    const QString base = QStringLiteral("fra_") + elementName(a->name);
    const QString in = nodeToken(a->in), out = nodeToken(a->out);
    QStringList lines;
    auto num = [](double v) { return QString::number(v, 'g', 15); };
    if (a->impedance) {
        // Current stimulus into "in" (returning through "out"), with the parallel resistance.
        if (a->enabled)
            lines << QStringLiteral("B%1 %2 %3 I=%4").arg(base, out, in, fra::stimulusExpression(*a));
        lines << QStringLiteral("R%1 %2 %3 %4").arg(base, in, out, num(a->rpar));
    } else {
        // Series voltage source: V(in) - V(out) = stimulus, through the parallel resistance rpar.
        const QString mid = internalNode(QStringLiteral("fra") + a->name, 1);
        lines << QStringLiteral("B%1 %2 %3 V=%4").arg(base, in, mid, a->enabled ? fra::stimulusExpression(*a) : QStringLiteral("0"));
        lines << QStringLiteral("R%1 %2 %3 %4").arg(base, mid, out, num(a->rpar));
    }
    if (a->enabled) {
        // A unit-amplitude copy of the stimulus across a 1F capacitor: ngspice's truncation-error
        // control then resolves every stimulus frequency, including in otherwise resistive circuits.
        const QString lte = internalNode(QStringLiteral("fra") + a->name, 2);
        lines << QStringLiteral("B%1_lte %2 0 V=%3").arg(base, lte, fra::stimulusExpression(*a, true));
        lines << QStringLiteral("C%1_lte %2 0 1").arg(base, lte);
        // Breakpoints at every segment boundary (ngspice cannot see them inside a behavioral
        // expression): a zero-valued PWL source with corners there.
        QStringList pts;
        double last = -1;
        for (const fra::Segment &sg : a->segments)
            for (double tb : {sg.start, sg.end})
                if (tb > last) {
                    pts << QStringLiteral("%1 0").arg(num(tb));
                    last = tb;
                }
        const QString bp = internalNode(QStringLiteral("fra") + a->name, 3);
        lines << QStringLiteral("V%1_bp %2 0 PWL(%3)").arg(base, bp, pts.join(' '));
        lines << QStringLiteral("R%1_bp %2 0 1").arg(base, bp);
    }
    return lines;
}

