#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <complex>

namespace fft {

// In-place FFT of any length (radix-2 when possible, Bluestein otherwise).
void transform(QVector<std::complex<double>> &data, bool inverse = false);

enum class Window { None, Hann, Hamming, Blackman, BlackmanHarris, FlatTop, Bartlett, Kaiser };
QStringList windowNames();
Window windowFromName(const QString &name);
double windowValue(Window w, int i, int n, double beta = 6.0);
// Coherent gain of a window (sum(w)/n), used to normalize amplitudes.
double windowGain(Window w, int n, double beta = 6.0);

struct Spectrum {
    QVector<double> freq;
    QVector<std::complex<double>> value; // amplitude spectrum (peak), one-sided
};

// Resamples (t, y) on a uniform grid of `points` samples between t0 and t1,
// applies the window and returns the one-sided amplitude spectrum.
Spectrum spectrum(const QVector<double> &t, const QVector<double> &y, double t0, double t1, int points, Window w,
                  bool removeDc = false, int binsAveraged = 1);

} // namespace fft
