#include "Laplace.h"

#include "Units.h"

#include <cmath>

namespace laplace {

namespace {

using Poly = QVector<double>;

Poly trim(Poly p)
{
    while (p.size() > 1 && std::fabs(p.last()) < 1e-300)
        p.removeLast();
    if (p.isEmpty())
        p << 0.0;
    return p;
}

Poly add(const Poly &a, const Poly &b, double sign = 1.0)
{
    Poly r(std::max(a.size(), b.size()), 0.0);
    for (int i = 0; i < a.size(); ++i)
        r[i] += a[i];
    for (int i = 0; i < b.size(); ++i)
        r[i] += sign * b[i];
    return trim(r);
}

Poly mul(const Poly &a, const Poly &b)
{
    Poly r(a.size() + b.size() - 1, 0.0);
    for (int i = 0; i < a.size(); ++i)
        for (int j = 0; j < b.size(); ++j)
            r[i + j] += a[i] * b[j];
    return trim(r);
}

struct R {
    Poly n{1.0}, d{1.0};
    double delay = 0;
    QVector<QPair<double, double>> secs; // (1+s/z)/(1+s/p) factors
    static R constant(double c) { R r; r.n = {c}; return r; }
    bool isConstant() const { return n.size() == 1 && d.size() == 1 && secs.isEmpty(); }
    double value() const { return n[0] / d[0]; }
};

R radd(const R &a, const R &b, double sign = 1.0)
{
    R r;
    r.n = add(mul(a.n, b.d), mul(b.n, a.d), sign);
    r.d = mul(a.d, b.d);
    r.delay = a.delay; // callers check that delays match and that there are no sections
    return r;
}

R rmul(const R &a, const R &b)
{
    R r;
    r.n = mul(a.n, b.n);
    r.d = mul(a.d, b.d);
    r.delay = a.delay + b.delay;
    r.secs = a.secs + b.secs;
    return r;
}

R rdiv(const R &a, const R &b)
{
    R r;
    r.n = mul(a.n, b.d);
    r.d = mul(a.d, b.n);
    r.delay = a.delay - b.delay;
    r.secs = a.secs;
    for (const auto &zp : b.secs)
        r.secs.append({zp.second, zp.first});
    return r;
}

class Parser
{
public:
    Parser(const QString &s, const std::function<std::optional<double>(const QString &)> &lookup)
        : m_s(s)
        , m_lookup(lookup)
    {
    }

    std::optional<R> run(QString *err)
    {
        auto r = expr();
        skip();
        if (!r || m_i < m_s.size()) {
            if (err)
                *err = m_err.isEmpty() ? QStringLiteral("cannot parse Laplace expression near '%1'").arg(m_s.mid(m_i, 10)) : m_err;
            return std::nullopt;
        }
        return r;
    }

private:
    QString m_s;
    int m_i = 0;
    QString m_err;
    std::function<std::optional<double>(const QString &)> m_lookup;

    void skip()
    {
        while (m_i < m_s.size() && m_s[m_i].isSpace())
            ++m_i;
    }
    QChar peek()
    {
        skip();
        return m_i < m_s.size() ? m_s[m_i] : QChar();
    }

    std::optional<R> expr()
    {
        auto a = term();
        while (a) {
            const QChar c = peek();
            if (c != '+' && c != '-')
                break;
            ++m_i;
            auto b = term();
            if (!b)
                return std::nullopt;
            if (!a->secs.isEmpty() || !b->secs.isEmpty()) {
                m_err = QStringLiteral("sums of fractional-order terms are not supported");
                return std::nullopt;
            }
            if (std::fabs(a->delay - b->delay) > 1e-30) {
                m_err = QStringLiteral("sums of terms with different exp(-s*T) delays are not supported");
                return std::nullopt;
            }
            a = radd(*a, *b, c == '+' ? 1.0 : -1.0);
        }
        return a;
    }
    std::optional<R> term()
    {
        auto a = power();
        while (a) {
            const QChar c = peek();
            if (c == '*' && (m_i + 1 >= m_s.size() || m_s[m_i + 1] != '*')) {
                ++m_i;
                auto b = power();
                if (!b)
                    return std::nullopt;
                a = rmul(*a, *b);
            } else if (c == '/') {
                ++m_i;
                auto b = power();
                if (!b)
                    return std::nullopt;
                a = rdiv(*a, *b);
            } else {
                break;
            }
        }
        return a;
    }
    std::optional<R> power()
    {
        auto a = unary();
        if (!a)
            return a;
        skip();
        bool isPow = false;
        if (m_i + 1 < m_s.size() && m_s[m_i] == '*' && m_s[m_i + 1] == '*') {
            m_i += 2;
            isPow = true;
        } else if (m_i < m_s.size() && m_s[m_i] == '^') {
            ++m_i;
            isPow = true;
        }
        if (!isPow)
            return a;
        auto e = unary();
        if (!e || !e->isConstant()) {
            m_err = QStringLiteral("exponent must be a constant");
            return std::nullopt;
        }
        const double ev = e->value();
        if (a->isConstant())
            return R::constant(std::pow(a->value(), ev));
        if (std::floor(ev) != ev)
            return fractionalPower(*a, ev);
        if (std::fabs(ev) > 20) {
            m_err = QStringLiteral("powers of s above 20 are not supported");
            return std::nullopt;
        }
        const int n = int(ev);
        R r = R::constant(1.0);
        for (int i = 0; i < std::abs(n); ++i)
            r = rmul(r, *a);
        return n >= 0 ? r : rdiv(R::constant(1.0), r);
    }
    // (c0 + c1*s)^e for a non-integer e: the integer part is exact, the
    // fractional part f is approximated by lead-lag sections spaced three per
    // decade over six decades above the corner (|error| < 0.1 dB, 0.5 deg).
    std::optional<R> firstOrderPower(const Poly &c, double e)
    {
        if (c.size() == 1)
            return R::constant(std::pow(c[0], e));
        if (c.size() != 2 || c[0] == 0 || c[1] / c[0] <= 0) {
            m_err = QStringLiteral("fractional powers are only supported for (a+b*s) with a, b of the same sign");
            return std::nullopt;
        }
        const double w = c[0] / c[1];
        const double ip = std::floor(e), f = e - ip;
        R r = R::constant(std::pow(c[0], e));
        R lin;
        lin.n = {1.0, 1.0 / w};
        for (int i = 0; i < std::abs(int(ip)); ++i)
            r = ip > 0 ? rmul(r, lin) : rdiv(r, lin);
        const int perDecade = 3, decades = 6;
        const double ratio = std::pow(10.0, 1.0 / perDecade), off = (1.0 - f) / 2;
        for (int k = 0; k < perDecade * decades; ++k) {
            const double z = w * std::pow(ratio, k + off);
            r.secs.append({z, z * std::pow(ratio, f)});
        }
        return r;
    }
    std::optional<R> fractionalPower(const R &a, double e)
    {
        if (!a.secs.isEmpty() || a.delay != 0) {
            m_err = QStringLiteral("fractional power of a delayed or fractional term is not supported");
            return std::nullopt;
        }
        if (a.n.size() > 2 || a.d.size() > 2) {
            m_err = QStringLiteral("fractional powers are only supported for first-order factors");
            return std::nullopt;
        }
        auto num = firstOrderPower(a.n, e);
        auto den = num ? firstOrderPower(a.d, e) : std::nullopt;
        if (!num || !den)
            return std::nullopt;
        return rdiv(*num, *den);
    }
    std::optional<R> unary()
    {
        const QChar c = peek();
        if (c == '-') {
            ++m_i;
            auto a = unary();
            if (!a)
                return a;
            return rmul(R::constant(-1.0), *a);
        }
        if (c == '+') {
            ++m_i;
            return unary();
        }
        return primary();
    }
    std::optional<R> primary()
    {
        const QChar c = peek();
        if (c == '(' || c == '{') {
            const QChar close = c == '(' ? ')' : '}';
            ++m_i;
            auto a = expr();
            if (peek() != close) {
                m_err = QStringLiteral("missing '%1'").arg(close);
                return std::nullopt;
            }
            ++m_i;
            return a;
        }
        if (c.isDigit() || c == '.') {
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
            while (m_i < m_s.size() && (m_s[m_i].isLetter() || m_s[m_i] == QChar(0xB5)))
                ++m_i;
            const auto v = units::parseSpiceNumber(m_s.mid(start, m_i - start));
            if (!v) {
                m_err = QStringLiteral("bad number");
                return std::nullopt;
            }
            return R::constant(*v);
        }
        if (c.isLetter() || c == '_') {
            const int start = m_i;
            while (m_i < m_s.size() && (m_s[m_i].isLetterOrNumber() || m_s[m_i] == '_'))
                ++m_i;
            const QString id = m_s.mid(start, m_i - start);
            const QString low = id.toLower();
            if (low == QLatin1String("s")) {
                R r;
                r.n = {0.0, 1.0};
                return r;
            }
            if (peek() == '(') {
                // Numeric function of constants (sqrt, exp, ...).
                ++m_i;
                auto a = expr();
                // exp(-s*T): a pure delay.
                if (a && peek() == ')' && low == QLatin1String("exp") && a->d.size() == 1 && a->n.size() == 2
                    && std::fabs(a->n[0]) < 1e-300 && a->delay == 0) {
                    ++m_i;
                    const double k = a->n[1] / a->d[0];
                    if (k > 0) {
                        m_err = QStringLiteral("exp(+s*T) (a time advance) is not realizable");
                        return std::nullopt;
                    }
                    R r = R::constant(1.0);
                    r.delay = -k;
                    return r;
                }
                if (a && peek() == ')' && low == QLatin1String("sqrt") && !a->isConstant()) {
                    ++m_i;
                    return fractionalPower(*a, 0.5);
                }
                if (peek() != ')' || !a || !a->isConstant()) {
                    m_err = QStringLiteral("function %1() needs a constant argument").arg(id);
                    return std::nullopt;
                }
                ++m_i;
                const double x = a->value();
                double v;
                if (low == QLatin1String("sqrt")) v = std::sqrt(x);
                else if (low == QLatin1String("exp")) v = std::exp(x);
                else if (low == QLatin1String("log") || low == QLatin1String("ln")) v = std::log(x);
                else if (low == QLatin1String("log10")) v = std::log10(x);
                else if (low == QLatin1String("abs")) v = std::fabs(x);
                else {
                    m_err = QStringLiteral("unsupported function %1() in Laplace expression").arg(id);
                    return std::nullopt;
                }
                return R::constant(v);
            }
            if (low == QLatin1String("pi"))
                return R::constant(M_PI);
            if (auto v = m_lookup(id))
                return R::constant(*v);
            m_err = QStringLiteral("unknown parameter '%1' in Laplace expression").arg(id);
            return std::nullopt;
        }
        m_err = QStringLiteral("unexpected '%1' in Laplace expression").arg(c);
        return std::nullopt;
    }
};

} // namespace

std::optional<Rational> parse(const QString &expr, const std::function<std::optional<double>(const QString &)> &lookup, QString *error)
{
    Parser p(expr, lookup);
    auto r = p.run(error);
    if (!r)
        return std::nullopt;
    Rational out;
    out.num = trim(r->n);
    out.den = trim(r->d);
    out.delay = r->delay;
    out.sections = r->secs;
    if (out.delay < 0) {
        if (error)
            *error = QStringLiteral("negative delay (time advance) is not realizable");
        return std::nullopt;
    }
    // Normalize so the highest denominator coefficient is 1.
    const double lead = out.den.last();
    if (lead == 0) {
        if (error)
            *error = QStringLiteral("denominator is zero");
        return std::nullopt;
    }
    for (double &c : out.num)
        c /= lead;
    for (double &c : out.den)
        c /= lead;
    return out;
}

QString coeffList(const QVector<double> &ascending)
{
    QStringList parts;
    for (int i = ascending.size() - 1; i >= 0; --i)
        parts << QString::number(ascending[i], 'g', 15);
    return QStringLiteral("[%1]").arg(parts.join(' '));
}

} // namespace laplace
