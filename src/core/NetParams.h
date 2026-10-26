#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <complex>
#include <optional>

// LTspice's ".net" two-port network parameters for .ac analyses:
//   .net V(out[,ref]) <source> [Rin=<val>] [Rout=<val>]
//   .net I(<resistor>) <source> [Rin=<val>] [Rout=<val>]
// Port 1 is the source's terminals, port 2 the output node pair (or the resistor's
// terminals). The parameters are computed from two extra AC solves: port 1 driven
// through Rin with port 2 terminated in Rout, and the reverse. All other AC sources
// are zeroed for these solves, so the normal .ac results are unaffected.
namespace netparams {

struct Spec {
    QString source;         // input source name as written (e.g. "V4"); names the results
    QString outP, outM;     // port 2 nodes
    QString outResistor;    // for I(R): the resistor that terminates port 2
    QString inP, inM;       // port 1 nodes (the source's terminals)
    double rin = 1, rout = 1;
};

// Parses every .net statement of an LTspice netlist and resolves ports and default
// terminations (the source's Rser, the output resistor's value, else 1 ohm).
QList<Spec> parse(const QStringList &ltLines, QStringList *errors);

// The netlist for excitation `port` (1 or 2) of `spec`.
QStringList excitationNetlist(const QStringList &ltLines, const Spec &spec, int port);

// Port voltages/currents (currents flowing into the network) of one excitation.
struct PortData {
    QVector<std::complex<double>> v1, i1, v2, i2;
};
// The port voltage expressions to read from an excitation run: V(port 1), V(port 2).
// The drive is an ideal 1V AC source behind the port's termination resistance.
QStringList measuredSignals(const Spec &spec);
PortData portData(const Spec &spec, int port, const QVector<std::complex<double>> &v1, const QVector<std::complex<double>> &v2);

struct Result {
    QStringList names; // "S11(V4)", ...
    QStringList types; // "S-parameter", "impedance", ...
    QList<QVector<std::complex<double>>> values;
};
Result compute(const Spec &spec, const PortData &a, const PortData &b);

} // namespace netparams
