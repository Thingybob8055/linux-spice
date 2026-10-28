#include "Units.h"

#include <cmath>

namespace units {

std::optional<double> parseSpiceNumber(const QString &input)
{
    QString text = input.trimmed();
    if (text.isEmpty())
        return std::nullopt;

    int i = 0;
    const int n = text.size();
    auto isDigit = [&](int k) { return k < n && text[k].isDigit(); };

    if (i < n && (text[i] == '+' || text[i] == '-'))
        ++i;
    const int mantissaStart = i;
    while (isDigit(i))
        ++i;
    if (i < n && text[i] == '.') {
        ++i;
        while (isDigit(i))
            ++i;
    }
    if (i == mantissaStart || (i == mantissaStart + 1 && text[mantissaStart] == '.'))
        return std::nullopt;
    // Exponent: only if followed by a digit (so "1e" stays invalid, but "1meg" is not an exponent).
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        int j = i + 1;
        if (j < n && (text[j] == '+' || text[j] == '-'))
            ++j;
        if (isDigit(j)) {
            i = j;
            while (isDigit(i))
                ++i;
        }
    }
    bool ok = false;
    double value = text.left(i).toDouble(&ok);
    if (!ok)
        return std::nullopt;

    const QString rest = text.mid(i).toLower();
    double mult = 1.0;
    if (rest.startsWith("meg"))
        mult = 1e6;
    else if (rest.startsWith("mil"))
        mult = 25.4e-6;
    else if (!rest.isEmpty()) {
        switch (rest[0].unicode()) {
        case 't': mult = 1e12; break;
        case 'g': mult = 1e9; break;
        case 'k': mult = 1e3; break;
        case 'm': mult = 1e-3; break;
        case 'u': case 0x00B5: case 0x03BC: mult = 1e-6; break;
        case 'n': mult = 1e-9; break;
        case 'p': mult = 1e-12; break;
        case 'f': mult = 1e-15; break;
        case 'a': mult = 1e-18; break;
        default: break;
        }
    }
    // The unit letter may stand for the decimal point, as LTspice reads it: "4k7" = 4.7k,
    // "7m999" = 7.999m, "2meg2" = 2.2meg. Only after a whole-number mantissa.
    if (mult != 1.0 && !text.left(i).contains('.') && !text.left(i).contains('e', Qt::CaseInsensitive)) {
        const int unitLen = rest.startsWith("meg") || rest.startsWith("mil") ? 3 : 1;
        int k = unitLen;
        while (k < rest.size() && rest[k].isDigit())
            ++k;
        if (k > unitLen) {
            const QString frac = rest.mid(unitLen, k - unitLen);
            const double f = (QStringLiteral("0.") + frac).toDouble();
            value = value < 0 || text.startsWith('-') ? value - f : value + f;
        }
    }
    return value * mult;
}

namespace {

struct Prefix {
    int exp;
    const char16_t *display;
    const char *spice;
};

constexpr Prefix kPrefixes[] = {
    {12, u"T", "T"}, {9, u"G", "G"}, {6, u"M", "Meg"}, {3, u"K", "K"}, {0, u"", ""},
    {-3, u"m", "m"}, {-6, u"µ", "u"}, {-9, u"n", "n"}, {-12, u"p", "p"}, {-15, u"f", "f"}, {-18, u"a", "a"},
};

QString trimZeros(QString s)
{
    if (s.contains('.') && !s.contains('e')) {
        while (s.endsWith('0'))
            s.chop(1);
        if (s.endsWith('.'))
            s.chop(1);
    }
    return s;
}

QString formatWith(double value, int significant, bool spice, const QString &unit)
{
    if (std::isnan(value))
        return QStringLiteral("NaN");
    if (std::isinf(value))
        return value > 0 ? QStringLiteral("Inf") : QStringLiteral("-Inf");
    if (value == 0.0)
        return QStringLiteral("0") + unit;

    const double mag = std::fabs(value);
    int exp3 = static_cast<int>(std::floor(std::log10(mag) / 3.0)) * 3;
    // Rounding can push 999.99 to 1000; renormalise after rounding.
    for (int pass = 0; pass < 2; ++pass) {
        if (exp3 > 12 || exp3 < -18)
            break;
        const double scaled = value / std::pow(10.0, exp3);
        int digitsBeforePoint = std::fabs(scaled) >= 100 ? 3 : (std::fabs(scaled) >= 10 ? 2 : 1);
        int decimals = std::max(0, significant - digitsBeforePoint);
        QString s = QString::number(scaled, 'f', decimals);
        if (std::fabs(s.toDouble()) >= 1000.0 && pass == 0) {
            exp3 += 3;
            continue;
        }
        for (const auto &p : kPrefixes) {
            if (p.exp == exp3)
                return trimZeros(s) + (spice ? QString::fromLatin1(p.spice) : QString::fromUtf16(p.display)) + unit;
        }
        break;
    }
    return QString::number(value, 'g', significant) + unit;
}

} // namespace

QString formatEng(double value, const QString &unit, int significant)
{
    return formatWith(value, significant, false, unit);
}

QString formatSpice(double value, int significant)
{
    return formatWith(value, significant, true, QString());
}

QString formatDelta(double dx, const QString &unit)
{
    QString s = QStringLiteral("%1").arg(formatEng(dx, unit));
    if (unit == QLatin1String("s") && dx != 0.0)
        s += QStringLiteral(" (%1)").arg(formatEng(1.0 / std::fabs(dx), QStringLiteral("Hz")));
    return s;
}

bool isBraced(const QString &s)
{
    if (s.size() < 2 || s.front() != '{' || s.back() != '}')
        return false;
    int depth = 0;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '{')
            ++depth;
        else if (s[i] == '}' && --depth == 0)
            return i == s.size() - 1;
    }
    return false;
}

} // namespace units
