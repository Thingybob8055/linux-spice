#include "Fra.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace fra {

namespace {

// key -> values (some parameters take lists: flist, pp, nmax).
QHash<QString, QStringList> parseParams(const QStringList &tokens, int from, QStringList *flags)
{
    QHash<QString, QStringList> p;
    QString key;
    for (int i = from; i < tokens.size(); ++i) {
        const QString &t = tokens[i];
        const int eq = t.indexOf('=');
        if (eq > 0) {
            key = t.left(eq).toLower();
            p[key].clear();
            if (eq + 1 < t.size())
                p[key] << t.mid(eq + 1);
        } else if (!key.isEmpty()) {
            p[key] << t;
        } else if (flags) {
            *flags << t.toLower();
        }
    }
    return p;
}

// Log-log interpolation through (f, value) pairs, clamped at the ends.
double logInterp(const QList<QPair<double, double>> &pts, double f)
{
    if (pts.isEmpty())
        return 0;
    if (f <= pts.first().first)
        return pts.first().second;
    if (f >= pts.last().first)
        return pts.last().second;
    for (int i = 1; i < pts.size(); ++i) {
        if (f <= pts[i].first) {
            const double x = std::log(f / pts[i - 1].first) / std::log(pts[i].first / pts[i - 1].first);
            return pts[i - 1].second * std::pow(pts[i].second / pts[i - 1].second, x);
        }
    }
    return pts.last().second;
}

QString num(double v)
{
    return QString::number(v, 'g', 15);
}

QString eng(double v)
{
    static const char *pre[] = {"", "k", "M", "G"};
    int i = 0;
    while (std::fabs(v) >= 1000 && i < 3) {
        v /= 1000;
        ++i;
    }
    return QStringLiteral("%1 %2Hz").arg(v, 0, 'f', 3).arg(QLatin1String(pre[i]));
}

// Complex amplitude of the component at `f` over [t0, t1] (trapezoidal integration
// on the simulator's own, non-uniform time points; the window spans whole periods).
std::complex<double> fourier(const QVector<double> &t, const QVector<double> &y, double f, double t0, double t1)
{
    const double w = 2 * M_PI * f;
    auto at = [&](double x) {
        const auto it = std::lower_bound(t.begin(), t.end(), x);
        if (it == t.begin())
            return y.first();
        if (it == t.end())
            return y.last();
        const int k = int(it - t.begin());
        const double a = t[k - 1], b = t[k];
        return b > a ? y[k - 1] + (y[k] - y[k - 1]) * (x - a) / (b - a) : y[k];
    };
    std::complex<double> acc = 0;
    double prevT = t0;
    std::complex<double> prev = at(t0) * std::polar(1.0, -w * t0);
    int k = int(std::upper_bound(t.begin(), t.end(), t0) - t.begin());
    for (;; ++k) {
        const bool last = k >= t.size() || t[k] >= t1;
        const double tc = last ? t1 : t[k];
        const std::complex<double> cur = (last ? at(t1) : y[k]) * std::polar(1.0, -w * tc);
        acc += 0.5 * (prev + cur) * (tc - prevT);
        prev = cur;
        prevT = tc;
        if (last)
            break;
    }
    return acc * (2.0 / (t1 - t0));
}

// The stimulus of one segment at time t (see stimulusExpression).
double stimulusValue(const Segment &s, double t)
{
    if (t < s.start || t >= s.end)
        return 0;
    const double tau = t - s.start, x = std::sin(2 * M_PI * s.freq * tau + s.phase);
    return s.amplitude / 2 * x * (s.onset && tau < 0.25 / s.freq ? x : 1.0);
}

double unwrapTo(double ref, double deg)
{
    while (deg - ref > 180)
        deg -= 360;
    while (deg - ref < -180)
        deg += 360;
    return deg;
}

} // namespace

std::optional<Analyzer> parseAnalyzer(const QStringList &tokens, const std::function<double(const QString &)> &eval, QString *error)
{
    if (tokens.size() < 3) {
        if (error)
            *error = QStringLiteral("%1: an FRA device needs two nodes").arg(tokens.value(0));
        return std::nullopt;
    }
    Analyzer a;
    a.name = tokens[0].mid(1);
    a.in = tokens[1];
    a.out = tokens[2];
    QStringList flags;
    const QHash<QString, QStringList> p = parseParams(tokens, 3, &flags);
    a.impedance = flags.contains(QLatin1String("zm"));
    auto value = [&](const char *k, double def) {
        const QStringList v = p.value(QLatin1String(k));
        return v.isEmpty() ? def : eval(v.first());
    };
    a.enabled = value("enabled", 1) >= 0.5;
    a.rpar = value("rpar", a.impedance ? 1e12 : 1e-3);
    a.refNode = p.value(QStringLiteral("refnode")).value(0, QStringLiteral("0"));
    a.intNode = p.value(QStringLiteral("intnode")).value(0);
    const double fstart = value("fstart", 0), fend = value("fend", 0);
    QList<double> freqs;
    if (p.contains(QStringLiteral("flist"))) {
        for (const QString &v : p.value(QStringLiteral("flist")))
            if (const double f = eval(v); f > 0)
                freqs << f;
    } else {
        if (!(fstart > 0) || !(fend > fstart)) {
            if (error)
                *error = QStringLiteral("@%1: fstart and fend (fend > fstart > 0) or flist are required").arg(a.name);
            return std::nullopt;
        }
        // Grid fstart*2^(k/oct); below fcoarse at most one point per octave; fend always included.
        const double oct = std::max(0.25, value("oct", 4));
        const double fcoarse = value("fcoarse", 0);
        const int coarseStep = oct > 1 ? int(std::lround(oct)) : 1;
        for (int k = 0; k < 100000; ++k) {
            const double f = fstart * std::pow(2.0, k / oct);
            if (f > fend * (1 + 1e-9))
                break;
            if (f < fcoarse && k % coarseStep != 0)
                continue;
            freqs << f;
        }
        if (freqs.isEmpty() || freqs.last() < fend * (1 - 1e-9))
            freqs << fend;
        std::reverse(freqs.begin(), freqs.end()); // LTspice sweeps downwards from fend
    }
    if (freqs.isEmpty()) {
        if (error)
            *error = QStringLiteral("@%1: no analysis frequencies").arg(a.name);
        return std::nullopt;
    }
    // Stimulus amplitude (peak-to-peak) versus frequency.
    QList<QPair<double, double>> amp;
    const double pp0def = a.impedance ? 10e-3 : 1e-3;
    if (p.contains(QStringLiteral("pp"))) {
        const QStringList v = p.value(QStringLiteral("pp"));
        for (int i = 0; i + 1 < v.size(); i += 2)
            amp << qMakePair(eval(v[i]), eval(v[i + 1]));
        std::sort(amp.begin(), amp.end());
    } else if (p.contains(QStringLiteral("f0")) && p.contains(QStringLiteral("f1")) && p.contains(QStringLiteral("pp1"))) {
        amp << qMakePair(value("f0", 0), value("pp0", pp0def)) << qMakePair(value("f1", 0), value("pp1", pp0def));
        std::sort(amp.begin(), amp.end());
    } else {
        amp << qMakePair(1.0, p.contains(QStringLiteral("pp0")) ? value("pp0", pp0def) : value("pp1", pp0def));
    }
    const double tsettle = value("tsettle", 10.0 / std::max(fend, freqs.first()));
    const double tavgmin = value("tavgmin", 0);
    double t = value("delay", 0);
    double phase = 0;
    for (double f : freqs) {
        Segment s;
        s.freq = f;
        s.amplitude = logInterp(amp, f);
        s.start = t;
        s.phase = phase;
        s.onset = a.segments.isEmpty();
        s.analyze = t + tsettle;
        const int periods = std::max(1, int(std::ceil(tavgmin * f - 1e-9)));
        s.analyzeEnd = s.analyze + periods / f;
        // Carry on to the next zero crossing, so the next frequency starts from zero without a
        // DC kick (which would excite slow loop dynamics); the phase stays continuous.
        const double phEnd = phase + 2 * M_PI * f * (s.analyzeEnd - s.start);
        const double next = M_PI * std::ceil(phEnd / M_PI - 1e-9);
        s.end = s.analyzeEnd + (next - phEnd) / (2 * M_PI * f);
        t = s.end;
        phase = std::fmod(next, 2 * M_PI);
        a.segments << s;
    }
    return a;
}

std::optional<Probe> parseProbe(const QStringList &tokens, QString *error)
{
    if (tokens.size() < 5) {
        if (error)
            *error = QStringLiteral("%1: an FRA probe needs four nodes (o+ o- i+ i-)").arg(tokens.value(0));
        return std::nullopt;
    }
    return Probe{tokens[0].mid(1), tokens[1], tokens[2], tokens[3], tokens[4]};
}

QString stimulusExpression(const Analyzer &a, bool normalized)
{
    QString expr = QStringLiteral("0");
    for (int i = a.segments.size() - 1; i >= 0; --i) {
        const Segment &s = a.segments[i];
        const double pk = normalized ? 1.0 : s.amplitude / 2;
        // As LTspice: the sweep starts as A*sin^2 for a quarter period (zero initial slope,
        // matching value and slope at T/4), then continues as A*sin with the phase carried
        // across frequency changes, so neither the start nor a boundary kicks the circuit.
        const QString w = num(2 * M_PI * s.freq);
        const QString arg = QStringLiteral("(%1*(time-%2)+%3)").arg(w, num(s.start), num(s.phase));
        const QString onset = s.onset ? QStringLiteral("*(time-%1<%2 ? sin(%3) : 1)").arg(num(s.start), num(0.25 / s.freq), arg) : QString();
        expr = QStringLiteral("(time>=%1 && time<%2 ? %3*sin(%4)%5 : %6)").arg(num(s.start), num(s.end), num(pk), arg, onset, expr);
    }
    return expr;
}

Result analyze(const Analyzer &a, const QList<Probe> &probes, const QVector<double> &time,
               const std::function<QVector<double>(const QString &expr)> &signal)
{
    Result r;
    if (time.size() < 2)
        return r;
    auto diff = [](const QString &p, const QString &m) {
        return m.isEmpty() || m == QLatin1String("0") ? QStringLiteral("V(%1)").arg(p) : QStringLiteral("V(%1,%2)").arg(p, m);
    };
    const QVector<double> vin = signal(a.impedance ? diff(a.in, a.out) : diff(a.in, a.refNode));
    const QVector<double> vout = a.impedance ? QVector<double>() : signal(diff(a.out, a.refNode));
    QVector<double> vint;
    if (!a.intNode.isEmpty() && !a.impedance)
        vint = signal(diff(a.intNode, a.refNode));
    struct ProbeSig {
        QString name;
        QVector<double> o, i;
    };
    QList<ProbeSig> ps;
    for (const Probe &p : probes)
        ps << ProbeSig{p.name, signal(diff(p.op, p.om)), signal(diff(p.ip, p.im))};
    QList<int> order;
    for (int i = 0; i < a.segments.size(); ++i)
        order << i;
    std::sort(order.begin(), order.end(), [&](int x, int y) { return a.segments[x].freq < a.segments[y].freq; });
    QList<std::complex<double>> intGain;
    QList<QList<std::complex<double>>> probeGain(ps.size());
    for (int idx : order) {
        const Segment &s = a.segments[idx];
        if (s.analyzeEnd > time.last() + 1e-15)
            continue; // run stopped early
        const std::complex<double> xin = fourier(time, vin, s.freq, s.analyze, s.analyzeEnd);
        std::complex<double> g;
        if (a.impedance) {
            // The stimulus current's component over the same window, from the waveform itself
            // (the sin^2 onset can reach into the window at low frequencies).
            const int n = int(std::clamp((s.analyzeEnd - s.analyze) * s.freq * 64, 256.0, 400000.0));
            QVector<double> ts(n + 1), is(n + 1);
            for (int k = 0; k <= n; ++k) {
                ts[k] = s.analyze + (s.analyzeEnd - s.analyze) * k / n;
                is[k] = stimulusValue(s, ts[k]);
            }
            g = xin / fourier(ts, is, s.freq, s.analyze, s.analyzeEnd);
        } else {
            const std::complex<double> xout = fourier(time, vout, s.freq, s.analyze, s.analyzeEnd);
            g = xin / xout;
            if (!vint.isEmpty())
                intGain << fourier(time, vint, s.freq, s.analyze, s.analyzeEnd) / xout;
        }
        r.freq << s.freq;
        r.gain << g;
        r.stimulus << s.amplitude;
        for (int k = 0; k < ps.size(); ++k)
            probeGain[k] << fourier(time, ps[k].o, s.freq, s.analyze, s.analyzeEnd) / fourier(time, ps[k].i, s.freq, s.analyze, s.analyzeEnd);
    }
    if (!vint.isEmpty())
        r.extra << qMakePair(QStringLiteral("gain_%1_%2").arg(a.name, a.intNode), intGain);
    for (int k = 0; k < ps.size(); ++k)
        r.extra << qMakePair(QStringLiteral("probe_%1").arg(ps[k].name), probeGain[k]);

    // Crossover, phase margin and gain margin (gain analysis only).
    if (!a.impedance && r.freq.size() > 1) {
        QStringList parts;
        QList<double> db, ph;
        for (const auto &g : r.gain) {
            db << 20 * std::log10(std::max(std::abs(g), 1e-300));
            ph << (ph.isEmpty() ? std::arg(g) * 180 / M_PI : unwrapTo(ph.last(), std::arg(g) * 180 / M_PI));
        }
        auto cross = [&](const QList<double> &y, double level, double *fx, double *other, const QList<double> &oy) {
            for (int i = 1; i < y.size(); ++i) {
                if ((y[i - 1] - level) * (y[i] - level) <= 0 && y[i - 1] != y[i] && y[i - 1] > y[i]) {
                    const double u = (y[i - 1] - level) / (y[i - 1] - y[i]);
                    *fx = r.freq[i - 1] * std::pow(r.freq[i] / r.freq[i - 1], u);
                    *other = oy[i - 1] + u * (oy[i] - oy[i - 1]);
                    return true;
                }
            }
            return false;
        };
        double f0 = 0, pm = 0;
        if (cross(db, 0, &f0, &pm, ph)) {
            // Report the margin in (-180, 180].
            while (pm > 180)
                pm -= 360;
            while (pm <= -180)
                pm += 360;
            parts << QStringLiteral("F(0dB) = %1, Phase Margin = %2 degrees").arg(eng(f0)).arg(pm, 0, 'f', 1);
        }
        // Gain margin where the phase falls through 0 (mod 360).
        QList<double> phw;
        for (double v : ph) {
            double x = std::fmod(v, 360.0);
            if (x > 180)
                x -= 360;
            if (x <= -180)
                x += 360;
            phw << x;
        }
        double fg = 0, gdb = 0;
        if (cross(phw, 0, &fg, &gdb, db))
            parts << QStringLiteral("Gain Margin = %1 dB at %2").arg(-gdb, 0, 'f', 1).arg(eng(fg));
        if (!parts.isEmpty())
            r.summary = QStringLiteral("FRA Instance @%1: %2").arg(a.name, parts.join(QStringLiteral(", ")));
    }
    return r;
}

} // namespace fra
