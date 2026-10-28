#include "Fourier.h"

#include "Expression.h"
#include "Measure.h"

#include <QRegularExpression>

#include <cmath>

namespace fourier {

namespace {

// Splits on whitespace outside parentheses and braces, so "V(a, b)" stays one token.
QStringList splitTokens(const QString &s)
{
    QStringList out;
    QString cur;
    int depth = 0;
    for (const QChar c : s) {
        if (c == '(' || c == '{')
            ++depth;
        else if ((c == ')' || c == '}') && depth > 0)
            --depth;
        if (c.isSpace() && depth == 0) {
            if (!cur.isEmpty())
                out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty())
        out << cur;
    return out;
}

double interp(const QVector<double> &t, const QVector<double> &y, double x)
{
    if (x <= t.first())
        return y.first();
    if (x >= t.last())
        return y.last();
    const int i = int(std::upper_bound(t.begin(), t.end(), x) - t.begin());
    const double t0 = t[i - 1], t1 = t[i];
    return t1 > t0 ? y[i - 1] + (y[i] - y[i - 1]) * (x - t0) / (t1 - t0) : y[i];
}

QString degrees(double d)
{
    return QString::asprintf("%8.2f", d) + QChar(0xB0);
}

} // namespace

QString evaluate(const Dataset &ds, int run, const QStringList &statements, const QHash<QString, double> &params)
{
    if (ds.kind != Dataset::Kind::Transient || run < 0 || run >= ds.runs.size())
        return QString();
    DatasetContext ctx(ds, run);
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        ctx.params.insert(it.key().toLower(), Value::scalar(it.value()));
    auto number = [&](QString text, bool *ok) {
        if (text.startsWith('{') && text.endsWith('}'))
            text = text.mid(1, text.size() - 2);
        Expression e(text);
        auto v = e.isValid() ? e.evaluate(ctx) : std::nullopt;
        *ok = v.has_value() && v->size() == 1;
        return *ok ? v->r(0) : 0.0;
    };
    const QVector<double> &time = ds.x(run);
    QString log;
    for (const QString &stmt : statements) {
        QStringList tok = splitTokens(stmt.trimmed());
        tok.removeFirst(); // .four
        if (tok.isEmpty())
            continue;
        bool ok = false;
        const double freq = number(tok.takeFirst(), &ok);
        if (!ok || freq <= 0) {
            log += QStringLiteral("Bad .four frequency in: %1\n").arg(stmt);
            continue;
        }
        int nharm = 9;
        double nper = 1;
        static const QRegularExpression numRe(QStringLiteral("^[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?$|^\\{.*\\}$"));
        if (!tok.isEmpty() && numRe.match(tok.first()).hasMatch())
            nharm = std::max(1, int(number(tok.takeFirst(), &ok)));
        if (!tok.isEmpty() && numRe.match(tok.first()).hasMatch())
            nper = number(tok.takeFirst(), &ok);
        const double tEnd = time.last();
        double tBeg = nper < 0 ? time.first() : tEnd - nper / freq;
        if (tBeg < time.first())
            tBeg = time.first();
        const double span = tEnd - tBeg;
        if (span <= 0)
            continue;
        for (const QString &trace : std::as_const(tok)) {
            Expression e(trace);
            auto v = e.isValid() ? e.evaluate(ctx) : std::nullopt;
            if (!v || v->size() != time.size()) {
                log += QStringLiteral("Cannot evaluate .four trace %1\n").arg(trace);
                continue;
            }
            // Uniform resampling of the piecewise-linear waveform, then trapezoidal integrals.
            const int n = std::max(8192, 256 * nharm * int(std::ceil(span * freq)));
            const double dt = span / n;
            double dc = 0, sq = 0;
            QVector<double> a(nharm + 1, 0.0), b(nharm + 1, 0.0);
            for (int k = 0; k <= n; ++k) {
                const double t = tBeg + k * dt;
                const double y = interp(time, v->re, t);
                const double w = (k == 0 || k == n) ? 0.5 : 1.0;
                dc += w * y;
                sq += w * y * y;
                for (int h = 1; h <= nharm; ++h) {
                    const double ph = 2 * M_PI * h * freq * t;
                    a[h] += w * y * std::cos(ph);
                    b[h] += w * y * std::sin(ph);
                }
            }
            dc /= n;
            sq /= n;
            QVector<double> amp(nharm + 1), phase(nharm + 1);
            for (int h = 1; h <= nharm; ++h) {
                a[h] *= 2.0 / n;
                b[h] *= 2.0 / n;
                amp[h] = std::hypot(a[h], b[h]);
                phase[h] = std::atan2(b[h], a[h]) * 180.0 / M_PI;
            }
            log += QStringLiteral("\nFourier components of %1\n").arg(trace);
            log += QStringLiteral("N-Period=%1\n").arg(nper < 0 ? QStringLiteral("all") : QString::number(nper));
            log += QStringLiteral("DC component:%1\n\n").arg(dc, 0, 'g', 6);
            log += QStringLiteral("Harmonic\tFrequency\t Fourier \tNormalized\t Phase  \tNormalized\n");
            log += QStringLiteral(" Number \t  [Hz]   \tComponent\t Component\t[degree]\tPhase [deg]\n");
            double partial = 0;
            for (int h = 1; h <= nharm; ++h) {
                const double norm = amp[1] > 0 ? amp[h] / amp[1] : 0.0;
                log += QString::asprintf("%5d   \t %.3e\t %.3e\t %.3e\t", h, h * freq, amp[h], norm) + degrees(phase[h]) + '\t'
                    + degrees(phase[h] - phase[1]) + '\n';
                if (h > 1)
                    partial += amp[h] * amp[h];
            }
            // Total: everything in the waveform except DC and the fundamental.
            const double rest = std::max(0.0, sq - dc * dc - amp[1] * amp[1] / 2);
            const double thd = amp[1] > 0 ? 100.0 * std::sqrt(rest) / (amp[1] / std::sqrt(2.0)) : 0.0;
            log += QString::asprintf("Partial Harmonic Distortion: %f%%\n", amp[1] > 0 ? 100.0 * std::sqrt(partial) / amp[1] : 0.0);
            log += QString::asprintf("Total Harmonic Distortion:   %f%%\n", thd);
        }
    }
    return log;
}

} // namespace fourier
