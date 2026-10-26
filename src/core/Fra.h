#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <complex>
#include <functional>
#include <optional>

class Dataset;

// LTspice's frequency response analysis: an "@" analyzer injects a sequence of
// sinusoids during a transient run (".fra"), and the response at each frequency
// is extracted by Fourier analysis afterwards. "&" probes add differential
// measurement points.
namespace fra {

struct Segment {
    double freq = 0;
    double amplitude = 0; // peak-to-peak, as LTspice's pp parameters
    double start = 0;     // stimulus applied from here
    double analyze = 0;   // analysis window [analyze, analyzeEnd): whole periods
    double analyzeEnd = 0;
    double end = 0;       // stimulus switches to the next frequency here (a zero crossing)
    double phase = 0;     // stimulus phase at `start` (radians): frequency changes are phase-continuous
    bool onset = false;   // first segment: the stimulus starts as A*sin^2 for a quarter period
};

struct Analyzer {
    QString name;      // instance name without the '@' ("1" for "@1")
    QString in, out;   // nodes as written in the netlist
    QString refNode;   // gain mode: voltages measured relative to this node ("0" = ground)
    QString intNode;   // optional intermediate node
    bool impedance = false; // "zm": current stimulus, result is V(in,out)/I
    double rpar = 1e-3;
    bool enabled = true;
    QList<Segment> segments; // in time order (LTspice sweeps from fend downwards)
    double stopTime() const { return segments.isEmpty() ? 0 : segments.last().end; }
};

struct Probe {
    QString name; // without the '&'
    QString op, om, ip, im;
};

// Parses "@name in out [zm] fstart=.. fend=.. ..." (tokens as split by the translator).
// `eval` evaluates a numeric token (with units/parameters).
std::optional<Analyzer> parseAnalyzer(const QStringList &tokens, const std::function<double(const QString &)> &eval, QString *error);
// Parses "&name o+ o- i+ i-".
std::optional<Probe> parseProbe(const QStringList &tokens, QString *error);

// Stimulus waveform as a behavioral expression of `time` (peak value pp/2), zero
// outside the sweep. `normalized` gives the same timing with unit amplitude.
QString stimulusExpression(const Analyzer &a, bool normalized = false);

struct Result {
    QList<double> freq;                       // ascending
    QList<std::complex<double>> gain;         // V(in)/V(out), or impedance
    QList<double> stimulus;                   // applied pp amplitude
    QList<QPair<QString, QList<std::complex<double>>>> extra; // probes / intnode
    QString summary;                          // "F(0dB) = ..., Phase Margin = ..., Gain Margin = ..."
};

// Analyzes a transient dataset (run `run`) for one analyzer. `voltage` returns the
// waveform of V(node) or V(a,b) expressions; `current` the stimulus current for
// impedance mode.
Result analyze(const Analyzer &a, const QList<Probe> &probes, const QVector<double> &time,
               const std::function<QVector<double>(const QString &expr)> &signal);

} // namespace fra
