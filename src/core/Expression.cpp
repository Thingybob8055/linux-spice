#include "Expression.h"

#include "Units.h"

#include <QMutex>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <cmath>

// ---------------------------------------------------------------- Unit

Unit Unit::operator*(const Unit &o) const
{
    if (!known || !o.known)
        return unknown();
    return {qint8(v + o.v), qint8(a + o.a), qint8(s + o.s), true};
}

Unit Unit::operator/(const Unit &o) const
{
    if (!known || !o.known)
        return unknown();
    return {qint8(v - o.v), qint8(a - o.a), qint8(s - o.s), true};
}

Unit Unit::pow(int n) const
{
    if (!known)
        return unknown();
    return {qint8(v * n), qint8(a * n), qint8(s * n), true};
}

Unit Unit::fromSymbol(const QString &sym)
{
    const QString s = sym.trimmed();
    if (s == QLatin1String("V")) return volt();
    if (s == QLatin1String("A")) return amp();
    if (s == QLatin1String("s")) return second();
    if (s == QLatin1String("Hz")) return hertz();
    if (s == QLatin1String("W")) return {1, 1, 0, true};
    if (s.isEmpty()) return none();
    return unknown();
}

QString Unit::symbol() const
{
    if (!known)
        return QString();
    struct Named {
        qint8 v, a, s;
        const char16_t *sym;
    };
    static const Named named[] = {
        {0, 0, 0, u""},   {1, 0, 0, u"V"},   {0, 1, 0, u"A"},    {1, 1, 0, u"W"},    {1, -1, 0, u"Ω"},
        {-1, 1, 0, u"S"}, {0, 0, 1, u"s"},   {0, 0, -1, u"Hz"},  {-1, 1, 1, u"F"},   {1, -1, 1, u"H"},
        {1, 0, 1, u"V·s"}, {0, 1, 1, u"C"},  {1, 1, 1, u"J"},    {1, 0, -1, u"V/s"}, {0, 1, -1, u"A/s"},
        {2, 0, 0, u"V²"}, {0, 2, 0, u"A²"},  {1, 1, -1, u"W/s"}, {0, 0, 2, u"s²"},
    };
    for (const Named &n : named)
        if (n.v == v && n.a == a && n.s == s)
            return QString::fromUtf16(n.sym);
    auto part = [](const char *name, int e) -> QString {
        if (e == 0)
            return QString();
        if (e == 1)
            return QString::fromLatin1(name);
        return QStringLiteral("%1^%2").arg(QLatin1String(name)).arg(e);
    };
    QStringList num, den;
    auto add = [&](const char *name, int e) {
        if (e > 0)
            num << part(name, e);
        else if (e < 0)
            den << part(name, -e);
    };
    add("V", v);
    add("A", a);
    add("s", s);
    QString out = num.isEmpty() ? QStringLiteral("1") : num.join(QStringLiteral("·"));
    if (!den.isEmpty())
        out += QStringLiteral("/") + den.join(QStringLiteral("·"));
    return out;
}

Value Value::cscalar(std::complex<double> z, Unit u)
{
    Value v;
    v.re = {z.real()};
    v.im = {z.imag()};
    v.complex = true;
    v.unit = u;
    return v;
}

// ---------------------------------------------------------------- AST

class ExprNode
{
public:
    enum Kind { Number, Ident, Signal, Call, Unary, Binary, Ternary, StepSel };
    Kind kind;
    double number = 0;
    QString name; // ident / function / signal text / operator
    QList<ExprPtr> args;
    int step = -1;
    explicit ExprNode(Kind k) : kind(k) {}
};

namespace {

struct UserFunc {
    QStringList params;
    QString body;
    ExprPtr ast;
};

QMutex g_funcMutex;
QHash<QString, UserFunc> &userFuncs()
{
    static QHash<QString, UserFunc> f;
    return f;
}

// ---------------------------------------------------------------- Lexer/Parser

class Parser
{
public:
    explicit Parser(const QString &text) : m_s(text) {}

    ExprPtr parse(QString *error)
    {
        skipWs();
        ExprPtr e = parseTernary();
        skipWs();
        if (!m_err.isEmpty() || !e) {
            *error = m_err.isEmpty() ? QStringLiteral("Syntax error") : m_err;
            return {};
        }
        if (m_i < m_s.size()) {
            *error = QStringLiteral("Unexpected '%1' at position %2").arg(m_s.mid(m_i, 8)).arg(m_i + 1);
            return {};
        }
        return e;
    }

private:
    QString m_s;
    int m_i = 0;
    QString m_err;

    QChar peek(int off = 0) const { return m_i + off < m_s.size() ? m_s[m_i + off] : QChar(); }
    void skipWs()
    {
        while (m_i < m_s.size() && m_s[m_i].isSpace())
            ++m_i;
    }
    bool eat(const char *tok)
    {
        skipWs();
        const QLatin1String t(tok);
        if (m_s.mid(m_i, t.size()) == t) {
            m_i += t.size();
            return true;
        }
        return false;
    }
    void fail(const QString &msg)
    {
        if (m_err.isEmpty())
            m_err = msg;
    }

    ExprPtr bin(const QString &op, ExprPtr a, ExprPtr b)
    {
        auto n = ExprPtr::create(ExprNode::Binary);
        n->name = op;
        n->args = {a, b};
        return n;
    }

    ExprPtr parseTernary()
    {
        ExprPtr c = parseOr();
        skipWs();
        if (c && peek() == '?') {
            ++m_i;
            ExprPtr a = parseTernary();
            if (!eat(":")) {
                fail(QStringLiteral("Expected ':'"));
                return {};
            }
            ExprPtr b = parseTernary();
            auto n = ExprPtr::create(ExprNode::Ternary);
            n->args = {c, a, b};
            return n;
        }
        return c;
    }
    ExprPtr parseOr()
    {
        ExprPtr a = parseAnd();
        while (a) {
            skipWs();
            if (peek() == '|') {
                m_i += (peek(1) == '|') ? 2 : 1;
                a = bin(QStringLiteral("|"), a, parseAnd());
            } else
                break;
        }
        return a;
    }
    ExprPtr parseAnd()
    {
        ExprPtr a = parseXor();
        while (a) {
            skipWs();
            if (peek() == '&') {
                m_i += (peek(1) == '&') ? 2 : 1;
                a = bin(QStringLiteral("&"), a, parseXor());
            } else
                break;
        }
        return a;
    }
    ExprPtr parseXor()
    {
        ExprPtr a = parseCmp();
        while (a) {
            skipWs();
            if (peek() == '^') {
                ++m_i;
                a = bin(QStringLiteral("^"), a, parseCmp());
            } else
                break;
        }
        return a;
    }
    ExprPtr parseCmp()
    {
        ExprPtr a = parseAdd();
        while (a) {
            skipWs();
            QString op;
            if (peek() == '>' && peek(1) == '=') op = QStringLiteral(">=");
            else if (peek() == '<' && peek(1) == '=') op = QStringLiteral("<=");
            else if (peek() == '=' && peek(1) == '=') op = QStringLiteral("==");
            else if (peek() == '!' && peek(1) == '=') op = QStringLiteral("!=");
            else if (peek() == '<' && peek(1) == '>') op = QStringLiteral("!=");
            else if (peek() == '>') op = QStringLiteral(">");
            else if (peek() == '<') op = QStringLiteral("<");
            else break;
            m_i += op.size() == 2 ? 2 : 1;
            a = bin(op, a, parseAdd());
        }
        return a;
    }
    ExprPtr parseAdd()
    {
        ExprPtr a = parseMul();
        while (a) {
            skipWs();
            if (peek() == '+' || peek() == '-') {
                const QString op(peek());
                ++m_i;
                a = bin(op, a, parseMul());
            } else
                break;
        }
        return a;
    }
    ExprPtr parseMul()
    {
        ExprPtr a = parsePow();
        while (a) {
            skipWs();
            if ((peek() == '*' && peek(1) != '*') || peek() == '/' || peek() == '%') {
                const QString op(peek());
                ++m_i;
                a = bin(op, a, parsePow());
            } else
                break;
        }
        return a;
    }
    ExprPtr parsePow()
    {
        ExprPtr a = parseUnary();
        skipWs();
        if (a && peek() == '*' && peek(1) == '*') {
            m_i += 2;
            return bin(QStringLiteral("**"), a, parsePowRhs());
        }
        return a;
    }
    ExprPtr parsePowRhs()
    {
        // Right-associative and binds tighter than unary minus on its left only.
        return parsePow();
    }
    ExprPtr parseUnary()
    {
        skipWs();
        if (peek() == '-' || peek() == '+' || (peek() == '!' && peek(1) != '=')) {
            const QString op(peek());
            ++m_i;
            ExprPtr x = parseUnary();
            if (op == QLatin1String("+"))
                return x;
            auto n = ExprPtr::create(ExprNode::Unary);
            n->name = op;
            n->args = {x};
            return n;
        }
        return parsePostfix();
    }
    ExprPtr parsePostfix()
    {
        ExprPtr p = parsePrimary();
        skipWs();
        while (p && peek() == '@') {
            ++m_i;
            skipWs();
            int start = m_i;
            while (m_i < m_s.size() && m_s[m_i].isDigit())
                ++m_i;
            if (start == m_i) {
                fail(QStringLiteral("Expected step number after '@'"));
                return {};
            }
            auto n = ExprPtr::create(ExprNode::StepSel);
            n->step = m_s.mid(start, m_i - start).toInt();
            n->args = {p};
            p = n;
            skipWs();
        }
        return p;
    }

    QString rawParenContent()
    {
        // Assumes current char is '('; returns text up to the matching ')'.
        int depth = 0;
        const int start = m_i + 1;
        for (; m_i < m_s.size(); ++m_i) {
            if (m_s[m_i] == '(')
                ++depth;
            else if (m_s[m_i] == ')') {
                if (--depth == 0) {
                    const QString inner = m_s.mid(start, m_i - start);
                    ++m_i;
                    return inner;
                }
            }
        }
        fail(QStringLiteral("Missing ')'"));
        return QString();
    }

    ExprPtr parsePrimary()
    {
        skipWs();
        const QChar c = peek();
        if (c.isNull()) {
            fail(QStringLiteral("Unexpected end of expression"));
            return {};
        }
        if (c == '(' || c == '{') {
            const char close = c == '(' ? ')' : '}';
            ++m_i;
            ExprPtr e = parseTernary();
            skipWs();
            if (peek() != QChar(close)) {
                fail(QStringLiteral("Expected '%1'").arg(QChar(close)));
                return {};
            }
            ++m_i;
            return e;
        }
        if (c.isDigit() || (c == '.' && peek(1).isDigit())) {
            const int start = m_i;
            while (m_i < m_s.size() && (m_s[m_i].isDigit() || m_s[m_i] == '.'))
                ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
                int j = m_i + 1;
                if (j < m_s.size() && (m_s[j] == '+' || m_s[j] == '-'))
                    ++j;
                if (j < m_s.size() && m_s[j].isDigit()) {
                    m_i = j;
                    while (m_i < m_s.size() && m_s[m_i].isDigit())
                        ++m_i;
                }
            }
            // Engineering suffix and trailing unit letters.
            while (m_i < m_s.size() && (m_s[m_i].isLetter() || m_s[m_i] == QChar(0xB5)))
                ++m_i;
            const auto v = units::parseSpiceNumber(m_s.mid(start, m_i - start));
            if (!v) {
                fail(QStringLiteral("Bad number '%1'").arg(m_s.mid(start, m_i - start)));
                return {};
            }
            auto n = ExprPtr::create(ExprNode::Number);
            n->number = *v;
            return n;
        }
        if (c.isLetter() || c == '_' || c == '$') {
            const int start = m_i;
            while (m_i < m_s.size() && (m_s[m_i].isLetterOrNumber() || m_s[m_i] == '_' || m_s[m_i] == '$' || m_s[m_i] == '.'
                                        || m_s[m_i] == ':' || m_s[m_i] == '#'))
                ++m_i;
            const QString ident = m_s.mid(start, m_i - start);
            skipWs();
            if (peek() == '(') {
                if (isSignalFunction(ident)) {
                    const QString inner = rawParenContent();
                    auto n = ExprPtr::create(ExprNode::Signal);
                    n->name = ident + '(' + inner.trimmed() + ')';
                    return n;
                }
                ++m_i;
                auto n = ExprPtr::create(ExprNode::Call);
                n->name = ident.toLower();
                skipWs();
                if (peek() == ')') {
                    ++m_i;
                    return n;
                }
                while (true) {
                    ExprPtr a = parseTernary();
                    if (!a)
                        return {};
                    n->args << a;
                    skipWs();
                    if (peek() == ',') {
                        ++m_i;
                        continue;
                    }
                    if (peek() == ')') {
                        ++m_i;
                        break;
                    }
                    fail(QStringLiteral("Expected ',' or ')' in call to %1").arg(ident));
                    return {};
                }
                return n;
            }
            auto n = ExprPtr::create(ExprNode::Ident);
            n->name = ident;
            return n;
        }
        // Raw vector names produced by some simulators, e.g. "@r1[i]".
        if (c == '@') {
            const int start = m_i;
            while (m_i < m_s.size() && !m_s[m_i].isSpace() && m_s[m_i] != ')' && m_s[m_i] != ',' && m_s[m_i] != '+'
                   && m_s[m_i] != '*' && m_s[m_i] != '/')
                ++m_i;
            auto n = ExprPtr::create(ExprNode::Signal);
            n->name = m_s.mid(start, m_i - start);
            return n;
        }
        fail(QStringLiteral("Unexpected '%1'").arg(c));
        return {};
    }
};

// ---------------------------------------------------------------- Evaluation helpers

int broadcastSize(const Value &a, const Value &b, bool *ok)
{
    if (a.size() == b.size() || b.size() == 1) {
        *ok = true;
        return a.size();
    }
    if (a.size() == 1) {
        *ok = true;
        return b.size();
    }
    *ok = false;
    return 0;
}

Value makeReal(int n, Unit u)
{
    Value v;
    v.re.resize(n);
    v.unit = u;
    return v;
}

Value makeComplex(int n, Unit u)
{
    Value v;
    v.re.resize(n);
    v.im.resize(n);
    v.complex = true;
    v.unit = u;
    return v;
}

template<typename F>
Value mapReal(const Value &a, Unit u, F f)
{
    Value r = makeReal(a.size(), u);
    for (int k = 0; k < a.size(); ++k)
        r.re[k] = f(a.re[k]);
    return r;
}

template<typename FR, typename FC>
Value mapAny(const Value &a, Unit u, FR fr, FC fc)
{
    if (!a.complex)
        return mapReal(a, u, fr);
    Value r = makeComplex(a.size(), u);
    for (int k = 0; k < a.size(); ++k) {
        const std::complex<double> z = fc(a.c(k));
        r.re[k] = z.real();
        r.im[k] = z.imag();
    }
    return r;
}

template<typename FR, typename FC>
std::optional<Value> zipAny(const Value &a, const Value &b, Unit u, FR fr, FC fc)
{
    bool ok;
    const int n = broadcastSize(a, b, &ok);
    if (!ok)
        return std::nullopt;
    if (!a.complex && !b.complex) {
        Value r = makeReal(n, u);
        for (int k = 0; k < n; ++k)
            r.re[k] = fr(a.r(k), b.r(k));
        return r;
    }
    Value r = makeComplex(n, u);
    for (int k = 0; k < n; ++k) {
        const std::complex<double> z = fc(a.c(k), b.c(k));
        r.re[k] = z.real();
        r.im[k] = z.imag();
    }
    return r;
}

template<typename F>
std::optional<Value> zipReal(const Value &a, const Value &b, Unit u, F f)
{
    bool ok;
    const int n = broadcastSize(a, b, &ok);
    if (!ok)
        return std::nullopt;
    Value r = makeReal(n, u);
    for (int k = 0; k < n; ++k)
        r.re[k] = f(a.r(k), b.r(k));
    return r;
}

double toBool(double x) { return x > 0.5 ? 1.0 : 0.0; }

double tableLookup(double x, const QVector<double> &xs, const QVector<double> &ys)
{
    if (xs.isEmpty())
        return 0.0;
    if (x <= xs.first())
        return ys.first();
    if (x >= xs.last())
        return ys.last();
    for (int i = 1; i < xs.size(); ++i) {
        if (x <= xs[i]) {
            const double t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] + t * (ys[i] - ys[i - 1]);
        }
    }
    return ys.last();
}

class Evaluator
{
public:
    Evaluator(EvalContext &ctx, QString *err) : m_ctx(ctx), m_err(err) {}

    std::optional<Value> eval(const ExprPtr &n, int step = -1);

private:
    EvalContext &m_ctx;
    QString *m_err;
    int m_depth = 0;

    std::optional<Value> fail(const QString &msg)
    {
        if (m_err && m_err->isEmpty())
            *m_err = msg;
        return std::nullopt;
    }
    std::optional<Value> call(const ExprPtr &n, int step);
};

std::optional<Value> Evaluator::eval(const ExprPtr &n, int step)
{
    if (!n)
        return fail(QStringLiteral("Empty expression"));
    switch (n->kind) {
    case ExprNode::Number:
        return Value::scalar(n->number);
    case ExprNode::Signal: {
        auto v = m_ctx.signal(n->name, step);
        if (!v)
            return fail(QStringLiteral("Unknown signal %1").arg(n->name));
        return v;
    }
    case ExprNode::Ident: {
        auto v = m_ctx.variable(n->name, step);
        if (v)
            return v;
        // Allow bare node/vector names as a fallback.
        v = m_ctx.signal(n->name, step);
        if (v)
            return v;
        const QString l = n->name.toLower();
        if (l == QLatin1String("pi"))
            return Value::scalar(M_PI);
        if (l == QLatin1String("e"))
            return Value::scalar(M_E);
        if (l == QLatin1String("k"))
            return Value::scalar(1.3806503e-23);
        if (l == QLatin1String("q"))
            return Value::scalar(1.602176462e-19);
        if (l == QLatin1String("true"))
            return Value::scalar(1);
        if (l == QLatin1String("false"))
            return Value::scalar(0);
        return fail(QStringLiteral("Unknown identifier %1").arg(n->name));
    }
    case ExprNode::StepSel:
        return eval(n->args[0], n->step - 1);
    case ExprNode::Unary: {
        auto a = eval(n->args[0], step);
        if (!a)
            return a;
        if (n->name == QLatin1String("-"))
            return mapAny(*a, a->unit, [](double x) { return -x; }, [](std::complex<double> z) { return -z; });
        return mapReal(*a, Unit(), [](double x) { return x > 0.5 ? 0.0 : 1.0; });
    }
    case ExprNode::Ternary: {
        auto c = eval(n->args[0], step);
        auto a = c ? eval(n->args[1], step) : std::nullopt;
        auto b = a ? eval(n->args[2], step) : std::nullopt;
        if (!b)
            return std::nullopt;
        const int sz = std::max({c->size(), a->size(), b->size()});
        const bool cx = a->complex || b->complex;
        Value r = cx ? makeComplex(sz, a->unit) : makeReal(sz, a->unit);
        for (int k = 0; k < sz; ++k) {
            const bool cond = c->r(c->isScalar() ? 0 : k) > 0.5;
            const Value &src = cond ? *a : *b;
            const int kk = src.isScalar() ? 0 : k;
            r.re[k] = src.r(kk);
            if (cx)
                r.im[k] = src.i(kk);
        }
        return r;
    }
    case ExprNode::Binary: {
        auto a = eval(n->args[0], step);
        if (!a)
            return a;
        auto b = eval(n->args[1], step);
        if (!b)
            return b;
        const QString &op = n->name;
        std::optional<Value> r;
        Unit addUnit = (a->unit == b->unit) ? a->unit : (a->isScalar() && a->unit.isDimensionless() ? b->unit
                                                       : (b->isScalar() && b->unit.isDimensionless() ? a->unit : Unit::unknown()));
        if (op == QLatin1String("+"))
            r = zipAny(*a, *b, addUnit, [](double x, double y) { return x + y; }, [](auto x, auto y) { return x + y; });
        else if (op == QLatin1String("-"))
            r = zipAny(*a, *b, addUnit, [](double x, double y) { return x - y; }, [](auto x, auto y) { return x - y; });
        else if (op == QLatin1String("*"))
            r = zipAny(*a, *b, a->unit * b->unit, [](double x, double y) { return x * y; }, [](auto x, auto y) { return x * y; });
        else if (op == QLatin1String("/"))
            r = zipAny(*a, *b, a->unit / b->unit, [](double x, double y) { return x / y; }, [](auto x, auto y) { return x / y; });
        else if (op == QLatin1String("%"))
            r = zipReal(*a, *b, a->unit, [](double x, double y) { return std::fmod(x, y); });
        else if (op == QLatin1String("**")) {
            Unit u = Unit::unknown();
            if (b->isScalar() && !b->complex && std::floor(b->first()) == b->first())
                u = a->unit.pow(int(b->first()));
            if (a->unit.isDimensionless() && b->unit.isDimensionless())
                u = Unit();
            r = zipAny(*a, *b, u, [](double x, double y) { return std::pow(x, y); },
                       [](auto x, auto y) { return std::pow(x, y); });
        } else if (op == QLatin1String("&"))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return toBool(x) * toBool(y); });
        else if (op == QLatin1String("|"))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return std::max(toBool(x), toBool(y)); });
        else if (op == QLatin1String("^"))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return toBool(x) != toBool(y) ? 1.0 : 0.0; });
        else if (op == QLatin1String(">"))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return x > y ? 1.0 : 0.0; });
        else if (op == QLatin1String("<"))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return x < y ? 1.0 : 0.0; });
        else if (op == QLatin1String(">="))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return x >= y ? 1.0 : 0.0; });
        else if (op == QLatin1String("<="))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return x <= y ? 1.0 : 0.0; });
        else if (op == QLatin1String("=="))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return x == y ? 1.0 : 0.0; });
        else if (op == QLatin1String("!="))
            r = zipReal(*a, *b, Unit(), [](double x, double y) { return x != y ? 1.0 : 0.0; });
        if (!r)
            return fail(QStringLiteral("Vector length mismatch in '%1'").arg(op));
        return r;
    }
    case ExprNode::Call:
        return call(n, step);
    }
    return fail(QStringLiteral("Bad expression"));
}

std::optional<Value> Evaluator::call(const ExprPtr &n, int step)
{
    const QString &f = n->name;
    QList<Value> args;
    for (const ExprPtr &a : n->args) {
        auto v = eval(a, step);
        if (!v)
            return v;
        args << *v;
    }
    auto need = [&](int cnt) -> bool {
        if (args.size() < cnt) {
            fail(QStringLiteral("%1() needs %2 argument(s)").arg(f).arg(cnt));
            return false;
        }
        return true;
    };

    // User functions first (they may shadow nothing built in except by design).
    {
        UserFunc uf;
        bool found = false;
        {
            QMutexLocker lock(&g_funcMutex);
            auto it = userFuncs().constFind(f);
            if (it != userFuncs().constEnd()) {
                uf = it.value();
                found = true;
            }
        }
        if (found) {
            if (m_depth > 50)
                return fail(QStringLiteral("Recursion too deep in %1()").arg(f));
            ParamContext local;
            local.parent = &m_ctx;
            for (int i = 0; i < uf.params.size() && i < args.size(); ++i)
                local.vars.insert(uf.params[i].toLower(), args[i]);
            Evaluator inner(local, m_err);
            inner.m_depth = m_depth + 1;
            return inner.eval(uf.ast, step);
        }
    }

    using C = std::complex<double>;
    const Unit dimless;
    if (f == QLatin1String("abs") || f == QLatin1String("mag")) {
        if (!need(1)) return std::nullopt;
        const Value &a = args[0];
        Value r = makeReal(a.size(), a.unit);
        for (int k = 0; k < a.size(); ++k)
            r.re[k] = a.complex ? std::abs(a.c(k)) : std::fabs(a.re[k]);
        return r;
    }
    if (f == QLatin1String("db")) {
        if (!need(1)) return std::nullopt;
        const Value &a = args[0];
        Value r = makeReal(a.size(), dimless);
        for (int k = 0; k < a.size(); ++k)
            r.re[k] = 20.0 * std::log10(a.complex ? std::abs(a.c(k)) : std::fabs(a.re[k]));
        return r;
    }
    if (f == QLatin1String("ph") || f == QLatin1String("phase") || f == QLatin1String("arg")) {
        if (!need(1)) return std::nullopt;
        const Value &a = args[0];
        Value r = makeReal(a.size(), dimless);
        for (int k = 0; k < a.size(); ++k)
            r.re[k] = std::arg(a.c(k)) * 180.0 / M_PI;
        return r;
    }
    if (f == QLatin1String("re") || f == QLatin1String("real")) {
        if (!need(1)) return std::nullopt;
        Value r = makeReal(args[0].size(), args[0].unit);
        for (int k = 0; k < r.size(); ++k)
            r.re[k] = args[0].r(k);
        return r;
    }
    if (f == QLatin1String("im") || f == QLatin1String("imag")) {
        if (!need(1)) return std::nullopt;
        Value r = makeReal(args[0].size(), args[0].unit);
        for (int k = 0; k < r.size(); ++k)
            r.re[k] = args[0].i(k);
        return r;
    }
    if (f == QLatin1String("conj")) {
        if (!need(1)) return std::nullopt;
        return mapAny(args[0], args[0].unit, [](double x) { return x; }, [](C z) { return std::conj(z); });
    }
    struct Simple {
        const char *name;
        double (*fr)(double);
        C (*fc)(C);
    };
    static const Simple simple[] = {
        {"sin", [](double x) { return std::sin(x); }, [](C z) { return std::sin(z); }},
        {"cos", [](double x) { return std::cos(x); }, [](C z) { return std::cos(z); }},
        {"tan", [](double x) { return std::tan(x); }, [](C z) { return std::tan(z); }},
        {"asin", [](double x) { return std::asin(x); }, [](C z) { return std::asin(z); }},
        {"arcsin", [](double x) { return std::asin(x); }, [](C z) { return std::asin(z); }},
        {"acos", [](double x) { return std::acos(x); }, [](C z) { return std::acos(z); }},
        {"arccos", [](double x) { return std::acos(x); }, [](C z) { return std::acos(z); }},
        {"atan", [](double x) { return std::atan(x); }, [](C z) { return std::atan(z); }},
        {"arctan", [](double x) { return std::atan(x); }, [](C z) { return std::atan(z); }},
        {"sinh", [](double x) { return std::sinh(x); }, [](C z) { return std::sinh(z); }},
        {"cosh", [](double x) { return std::cosh(x); }, [](C z) { return std::cosh(z); }},
        {"tanh", [](double x) { return std::tanh(x); }, [](C z) { return std::tanh(z); }},
        {"asinh", [](double x) { return std::asinh(x); }, [](C z) { return std::asinh(z); }},
        {"acosh", [](double x) { return std::acosh(x); }, [](C z) { return std::acosh(z); }},
        {"atanh", [](double x) { return std::atanh(x); }, [](C z) { return std::atanh(z); }},
        {"exp", [](double x) { return std::exp(x); }, [](C z) { return std::exp(z); }},
        {"ln", [](double x) { return std::log(x); }, [](C z) { return std::log(z); }},
        {"log", [](double x) { return std::log(x); }, [](C z) { return std::log(z); }},
        {"log10", [](double x) { return std::log10(x); }, [](C z) { return std::log10(z); }},
        {"ceil", [](double x) { return std::ceil(x); }, [](C z) { return C(std::ceil(z.real()), 0); }},
        {"floor", [](double x) { return std::floor(x); }, [](C z) { return C(std::floor(z.real()), 0); }},
        {"round", [](double x) { return std::round(x); }, [](C z) { return C(std::round(z.real()), 0); }},
        {"int", [](double x) { return std::trunc(x); }, [](C z) { return C(std::trunc(z.real()), 0); }},
        {"sgn", [](double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); }, [](C z) { return C(z.real() > 0 ? 1 : (z.real() < 0 ? -1 : 0), 0); }},
        {"sign", [](double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); }, [](C z) { return C(z.real() > 0 ? 1 : (z.real() < 0 ? -1 : 0), 0); }},
        {"u", [](double x) { return x > 0 ? 1.0 : 0.0; }, [](C z) { return C(z.real() > 0 ? 1 : 0, 0); }},
        {"buf", [](double x) { return x > 0.5 ? 1.0 : 0.0; }, [](C z) { return C(z.real() > 0.5 ? 1 : 0, 0); }},
        {"inv", [](double x) { return x > 0.5 ? 0.0 : 1.0; }, [](C z) { return C(z.real() > 0.5 ? 0 : 1, 0); }},
        {"uramp", [](double x) { return x > 0 ? x : 0.0; }, [](C z) { return C(z.real() > 0 ? z.real() : 0, 0); }},
    };
    for (const Simple &s : simple) {
        if (f == QLatin1String(s.name)) {
            if (!need(1)) return std::nullopt;
            Unit u = dimless;
            if (f == QLatin1String("uramp") || f == QLatin1String("ceil") || f == QLatin1String("floor")
                || f == QLatin1String("round") || f == QLatin1String("int"))
                u = args[0].unit;
            return mapAny(args[0], u, s.fr, s.fc);
        }
    }
    if (f == QLatin1String("sqrt")) {
        if (!need(1)) return std::nullopt;
        const Value &a = args[0];
        Unit u = Unit::unknown();
        if (a.unit.known && a.unit.v % 2 == 0 && a.unit.a % 2 == 0 && a.unit.s % 2 == 0)
            u = Unit{qint8(a.unit.v / 2), qint8(a.unit.a / 2), qint8(a.unit.s / 2), true};
        return mapAny(a, u, [](double x) { return std::sqrt(x); }, [](C z) { return std::sqrt(z); });
    }
    if (f == QLatin1String("pow") || f == QLatin1String("pwr") || f == QLatin1String("pwrs")) {
        if (!need(2)) return std::nullopt;
        Unit u = Unit::unknown();
        if (args[1].isScalar() && std::floor(args[1].first()) == args[1].first())
            u = args[0].unit.pow(int(args[1].first()));
        if (f == QLatin1String("pow"))
            return zipAny(args[0], args[1], u, [](double x, double y) { return std::pow(x, y); },
                          [](C x, C y) { return std::pow(x, y); });
        if (f == QLatin1String("pwr"))
            return zipReal(args[0], args[1], u, [](double x, double y) { return std::pow(std::fabs(x), y); });
        return zipReal(args[0], args[1], u, [](double x, double y) {
            return (x < 0 ? -1.0 : 1.0) * std::pow(std::fabs(x), y);
        });
    }
    if (f == QLatin1String("atan2")) {
        if (!need(2)) return std::nullopt;
        return zipReal(args[0], args[1], dimless, [](double y, double x) { return std::atan2(y, x); });
    }
    if (f == QLatin1String("hypot")) {
        if (!need(2)) return std::nullopt;
        return zipReal(args[0], args[1], args[0].unit, [](double a, double b) { return std::hypot(a, b); });
    }
    if (f == QLatin1String("min") || f == QLatin1String("max")) {
        if (!need(2)) return std::nullopt;
        const bool isMin = f == QLatin1String("min");
        return zipReal(args[0], args[1], args[0].unit, [isMin](double a, double b) { return isMin ? std::min(a, b) : std::max(a, b); });
    }
    if (f == QLatin1String("limit")) {
        if (!need(3)) return std::nullopt;
        bool ok;
        int sz = broadcastSize(args[0], args[1], &ok);
        sz = std::max(sz, args[2].size());
        Value r = makeReal(sz, args[0].unit);
        for (int k = 0; k < sz; ++k) {
            const double x = args[0].r(args[0].isScalar() ? 0 : k);
            double lo = args[1].r(args[1].isScalar() ? 0 : k), hi = args[2].r(args[2].isScalar() ? 0 : k);
            if (lo > hi)
                std::swap(lo, hi);
            r.re[k] = std::clamp(x, lo, hi);
        }
        return r;
    }
    if (f == QLatin1String("if")) {
        if (!need(3)) return std::nullopt;
        const int sz = std::max({args[0].size(), args[1].size(), args[2].size()});
        const bool cx = args[1].complex || args[2].complex;
        Value r = cx ? makeComplex(sz, args[1].unit) : makeReal(sz, args[1].unit);
        for (int k = 0; k < sz; ++k) {
            const bool cond = args[0].r(args[0].isScalar() ? 0 : k) > 0.5;
            const Value &src = cond ? args[1] : args[2];
            const int kk = src.isScalar() ? 0 : k;
            r.re[k] = src.r(kk);
            if (cx)
                r.im[k] = src.i(kk);
        }
        return r;
    }
    if (f == QLatin1String("table")) {
        if (args.size() < 3 || (args.size() - 1) % 2 != 0)
            return fail(QStringLiteral("table() needs x followed by x,y pairs"));
        QVector<double> xs, ys;
        for (int i = 1; i + 1 < args.size(); i += 2) {
            xs << args[i].first();
            ys << args[i + 1].first();
        }
        return mapReal(args[0], args[2].unit, [&](double x) { return tableLookup(x, xs, ys); });
    }
    if (f == QLatin1String("d") || f == QLatin1String("deriv") || f == QLatin1String("ddt")) {
        if (!need(1)) return std::nullopt;
        auto xa = m_ctx.xAxis(step);
        const Value &a = args[0];
        if (!xa || xa->size() != a.size() || a.size() < 2)
            return fail(QStringLiteral("d() needs a waveform argument"));
        Value r = a.complex ? makeComplex(a.size(), a.unit / xa->unit) : makeReal(a.size(), a.unit / xa->unit);
        const int n = a.size();
        for (int k = 0; k < n; ++k) {
            const int k0 = k == 0 ? 0 : k - 1;
            const int k1 = k == n - 1 ? n - 1 : k + 1;
            const double dx = xa->re[k1] - xa->re[k0];
            if (dx == 0) {
                r.re[k] = k > 0 ? r.re[k - 1] : 0.0;
                if (a.complex)
                    r.im[k] = k > 0 ? r.im[k - 1] : 0.0;
                continue;
            }
            r.re[k] = (a.re[k1] - a.re[k0]) / dx;
            if (a.complex)
                r.im[k] = (a.im[k1] - a.im[k0]) / dx;
        }
        return r;
    }
    if (f == QLatin1String("idt") || f == QLatin1String("sdt") || f == QLatin1String("integ")) {
        if (!need(1)) return std::nullopt;
        auto xa = m_ctx.xAxis(step);
        const Value &a = args[0];
        if (!xa || xa->size() != a.size())
            return fail(QStringLiteral("%1() needs a waveform argument").arg(f));
        Value r = makeReal(a.size(), a.unit * xa->unit);
        double acc = args.size() > 1 ? args[1].first() : 0.0;
        for (int k = 0; k < a.size(); ++k) {
            if (k > 0)
                acc += 0.5 * (a.re[k] + a.re[k - 1]) * (xa->re[k] - xa->re[k - 1]);
            r.re[k] = acc;
        }
        return r;
    }
    if (f == QLatin1String("rand") || f == QLatin1String("random") || f == QLatin1String("white")) {
        if (!need(1)) return std::nullopt;
        // Deterministic hash-based noise so redraws are stable.
        auto hashRand = [](qint64 i) {
            quint64 x = quint64(i) * 0x9E3779B97F4A7C15ull;
            x ^= x >> 31;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 29;
            return double(x & 0xFFFFFFFFull) / double(0xFFFFFFFFull);
        };
        if (f == QLatin1String("rand"))
            return mapReal(args[0], dimless, [&](double x) { return hashRand(qint64(std::floor(x))); });
        return mapReal(args[0], dimless, [&](double x) {
            const double fl = std::floor(x);
            const double t = x - fl;
            const double s = t * t * (3 - 2 * t);
            const double a = hashRand(qint64(fl)), b = hashRand(qint64(fl) + 1);
            const double v = a + s * (b - a);
            return f == QLatin1String("white") ? v - 0.5 : v;
        });
    }
    if (f == QLatin1String("avg") || f == QLatin1String("rms")) {
        // Running average/RMS over the x axis (cumulative), like LTspice's waveform viewer.
        if (!need(1)) return std::nullopt;
        auto xa = m_ctx.xAxis(step);
        const Value &a = args[0];
        if (!xa || xa->size() != a.size())
            return fail(QStringLiteral("%1() needs a waveform argument").arg(f));
        Value r = makeReal(a.size(), a.unit);
        double acc = 0;
        const bool rms = f == QLatin1String("rms");
        for (int k = 0; k < a.size(); ++k) {
            if (k > 0) {
                const double y0 = rms ? a.re[k - 1] * a.re[k - 1] : a.re[k - 1];
                const double y1 = rms ? a.re[k] * a.re[k] : a.re[k];
                acc += 0.5 * (y0 + y1) * (xa->re[k] - xa->re[k - 1]);
            }
            const double span = xa->re[k] - xa->re[0];
            const double m = span > 0 ? acc / span : (rms ? a.re[k] * a.re[k] : a.re[k]);
            r.re[k] = rms ? std::sqrt(std::fabs(m)) : m;
        }
        return r;
    }
    return fail(QStringLiteral("Unknown function %1()").arg(f));
}

void collect(const ExprPtr &n, QStringList *sigNames, QStringList *idents)
{
    if (!n)
        return;
    if (n->kind == ExprNode::Signal && sigNames && !sigNames->contains(n->name))
        sigNames->append(n->name);
    if (n->kind == ExprNode::Ident && idents && !idents->contains(n->name))
        idents->append(n->name);
    for (const ExprPtr &a : n->args)
        collect(a, sigNames, idents);
}

} // namespace

bool isSignalFunction(const QString &ident)
{
    static const QStringList names = {QStringLiteral("v"),  QStringLiteral("i"),  QStringLiteral("ix"), QStringLiteral("ic"),
                                      QStringLiteral("ib"), QStringLiteral("ie"), QStringLiteral("id"), QStringLiteral("ig"),
                                      QStringLiteral("is"), QStringLiteral("ip"), QStringLiteral("vm"), QStringLiteral("vdb"),
                                      QStringLiteral("vp"), QStringLiteral("vr"), QStringLiteral("vi")};
    return names.contains(ident.toLower());
}

// ---------------------------------------------------------------- ParamContext

std::optional<Value> ParamContext::signal(const QString &name, int step)
{
    if (parent)
        return parent->signal(name, step);
    return std::nullopt;
}

std::optional<Value> ParamContext::variable(const QString &name, int step)
{
    auto it = vars.constFind(name.toLower());
    if (it != vars.constEnd())
        return it.value();
    if (parent)
        return parent->variable(name, step);
    return std::nullopt;
}

std::optional<Value> ParamContext::xAxis(int step)
{
    if (parent)
        return parent->xAxis(step);
    return std::nullopt;
}

// ---------------------------------------------------------------- Expression

Expression::Expression(const QString &text)
{
    parse(text);
}

bool Expression::parse(const QString &text)
{
    m_text = text;
    m_error.clear();
    Parser p(text);
    m_root = p.parse(&m_error);
    return m_root != nullptr;
}

std::optional<Value> Expression::evaluate(EvalContext &ctx) const
{
    m_error.clear();
    if (!m_root) {
        if (m_error.isEmpty())
            m_error = QStringLiteral("Invalid expression");
        return std::nullopt;
    }
    Evaluator ev(ctx, &m_error);
    return ev.eval(m_root);
}

double Expression::evaluateScalar(EvalContext &ctx, bool *ok) const
{
    auto v = evaluate(ctx);
    if (ok)
        *ok = v.has_value() && !v->re.isEmpty();
    return v && !v->re.isEmpty() ? v->re[0] : 0.0;
}

QStringList Expression::signalNames() const
{
    QStringList s;
    collect(m_root, &s, nullptr);
    return s;
}

QStringList Expression::identifiers() const
{
    QStringList s;
    collect(m_root, nullptr, &s);
    return s;
}

void Expression::defineFunction(const QString &name, const QStringList &params, const QString &body)
{
    UserFunc uf;
    uf.params = params;
    uf.body = body;
    QString err;
    Parser p(body);
    uf.ast = p.parse(&err);
    if (!uf.ast)
        return;
    QMutexLocker lock(&g_funcMutex);
    userFuncs().insert(name.toLower(), uf);
}

void Expression::clearFunctions()
{
    QMutexLocker lock(&g_funcMutex);
    userFuncs().clear();
}

bool Expression::hasFunction(const QString &name)
{
    QMutexLocker lock(&g_funcMutex);
    return userFuncs().contains(name.toLower());
}

bool Expression::defineFromDirective(const QString &line)
{
    static const QRegularExpression re(QStringLiteral("^\\s*\\.func\\s+([A-Za-z_][\\w$]*)\\s*\\(([^)]*)\\)\\s*=?\\s*(.+)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(line);
    if (!m.hasMatch())
        return false;
    QStringList params;
    for (const QString &p : m.captured(2).split(',', Qt::SkipEmptyParts))
        params << p.trimmed();
    QString body = m.captured(3).trimmed();
    if (body.startsWith('{') && body.endsWith('}'))
        body = body.mid(1, body.size() - 2);
    defineFunction(m.captured(1), params, body);
    return true;
}
