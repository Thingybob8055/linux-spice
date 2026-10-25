#include "Fft.h"

#include <cmath>

namespace fft {

namespace {

using C = std::complex<double>;

bool isPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

void radix2(QVector<C> &a, bool inverse)
{
    const int n = a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / len * (inverse ? 1 : -1);
        const C wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            C w(1);
            for (int j = 0; j < len / 2; ++j) {
                const C u = a[i + j];
                const C v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (C &x : a)
            x /= double(n);
}

void bluestein(QVector<C> &a, bool inverse)
{
    const int n = a.size();
    int m = 1;
    while (m < 2 * n - 1)
        m <<= 1;
    QVector<C> w(n);
    const double sign = inverse ? 1.0 : -1.0;
    for (int k = 0; k < n; ++k) {
        // k*k can overflow int for large n; reduce modulo 2n.
        const long long kk = (static_cast<long long>(k) * k) % (2LL * n);
        const double ang = sign * M_PI * double(kk) / n;
        w[k] = C(std::cos(ang), std::sin(ang));
    }
    QVector<C> A(m, C(0)), B(m, C(0));
    for (int k = 0; k < n; ++k)
        A[k] = a[k] * w[k];
    B[0] = std::conj(w[0]);
    for (int k = 1; k < n; ++k)
        B[k] = B[m - k] = std::conj(w[k]);
    radix2(A, false);
    radix2(B, false);
    for (int i = 0; i < m; ++i)
        A[i] *= B[i];
    radix2(A, true);
    for (int k = 0; k < n; ++k) {
        a[k] = A[k] * w[k];
        if (inverse)
            a[k] /= double(n);
    }
}

double besselI0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

} // namespace

void transform(QVector<std::complex<double>> &data, bool inverse)
{
    if (data.size() <= 1)
        return;
    if (isPow2(data.size()))
        radix2(data, inverse);
    else
        bluestein(data, inverse);
}

QStringList windowNames()
{
    return {QStringLiteral("(none)"), QStringLiteral("Hann"), QStringLiteral("Hamming"), QStringLiteral("Blackman"),
            QStringLiteral("Blackman-Harris"), QStringLiteral("Flat Top"), QStringLiteral("Bartlett"), QStringLiteral("Kaiser")};
}

Window windowFromName(const QString &name)
{
    const int i = windowNames().indexOf(name);
    return i < 0 ? Window::None : Window(i);
}

double windowValue(Window w, int i, int n, double beta)
{
    if (n <= 1)
        return 1.0;
    const double x = double(i) / (n - 1);
    switch (w) {
    case Window::None: return 1.0;
    case Window::Hann: return 0.5 - 0.5 * std::cos(2 * M_PI * x);
    case Window::Hamming: return 0.54 - 0.46 * std::cos(2 * M_PI * x);
    case Window::Blackman: return 0.42 - 0.5 * std::cos(2 * M_PI * x) + 0.08 * std::cos(4 * M_PI * x);
    case Window::BlackmanHarris:
        return 0.35875 - 0.48829 * std::cos(2 * M_PI * x) + 0.14128 * std::cos(4 * M_PI * x) - 0.01168 * std::cos(6 * M_PI * x);
    case Window::FlatTop:
        return 0.21557895 - 0.41663158 * std::cos(2 * M_PI * x) + 0.277263158 * std::cos(4 * M_PI * x)
            - 0.083578947 * std::cos(6 * M_PI * x) + 0.006947368 * std::cos(8 * M_PI * x);
    case Window::Bartlett: return 1.0 - std::fabs(2 * x - 1);
    case Window::Kaiser: {
        const double r = 2 * x - 1;
        return besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
    }
    }
    return 1.0;
}

double windowGain(Window w, int n, double beta)
{
    double s = 0;
    for (int i = 0; i < n; ++i)
        s += windowValue(w, i, n, beta);
    return n > 0 ? s / n : 1.0;
}

Spectrum spectrum(const QVector<double> &t, const QVector<double> &y, double t0, double t1, int points, Window w, bool removeDc,
                  int binsAveraged)
{
    Spectrum out;
    if (t.size() < 2 || points < 2 || t1 <= t0)
        return out;
    // Uniform resampling with linear interpolation.
    QVector<C> data(points);
    const double dt = (t1 - t0) / points;
    int k = 0;
    double mean = 0;
    for (int i = 0; i < points; ++i) {
        const double ti = t0 + i * dt;
        while (k + 1 < t.size() - 1 && t[k + 1] < ti)
            ++k;
        double v;
        if (ti <= t.first())
            v = y.first();
        else if (ti >= t.last())
            v = y.last();
        else {
            const double ta = t[k], tb = t[k + 1];
            const double f = tb > ta ? (ti - ta) / (tb - ta) : 0.0;
            v = y[k] + f * (y[k + 1] - y[k]);
        }
        data[i] = v;
        mean += v;
    }
    mean /= points;
    const double gain = windowGain(w, points);
    for (int i = 0; i < points; ++i) {
        double v = data[i].real();
        if (removeDc)
            v -= mean;
        data[i] = v * windowValue(w, i, points);
    }
    transform(data, false);
    const int half = points / 2 + 1;
    const double df = 1.0 / (t1 - t0);
    out.freq.resize(half);
    out.value.resize(half);
    for (int i = 0; i < half; ++i) {
        out.freq[i] = i * df;
        C v = data[i] / (points * gain);
        if (i > 0 && !(points % 2 == 0 && i == half - 1))
            v *= 2.0; // one-sided amplitude
        out.value[i] = v;
    }
    if (binsAveraged > 1) {
        Spectrum avg;
        for (int i = 0; i + binsAveraged <= half; i += binsAveraged) {
            double p = 0, f = 0;
            for (int j = 0; j < binsAveraged; ++j) {
                p += std::norm(out.value[i + j]);
                f += out.freq[i + j];
            }
            avg.freq << f / binsAveraged;
            avg.value << C(std::sqrt(p / binsAveraged), 0);
        }
        return avg;
    }
    return out;
}

} // namespace fft
