#pragma once

#include <QHash>
#include <QList>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <complex>

// One simulation result set (one analysis), possibly with several stepped runs.
class Dataset
{
public:
    enum class Kind { Unknown, Transient, AC, DCSweep, OperatingPoint, Noise, TransferFunction, FFT };

    struct Variable {
        QString name; // display name, e.g. "V(out)", "I(R1)", "time"
        QString type; // "voltage", "current", "time", "frequency", ...
    };
    struct Run {
        QVector<QVector<double>> re; // [variable][point]
        QVector<QVector<double>> im; // empty when real
        QString label;               // step description ("R=1k")
    };

    QString title;
    QString plotName;
    QString date;
    QString command;
    Kind kind = Kind::Unknown;
    bool complex = false;
    bool logScaleX = false;
    bool linearX = false; // "linear" flag: a linear frequency sweep (AC/noise default to a log axis)
    QList<Variable> vars;
    QList<Run> runs;
    QString sourceFile;
    QHash<QString, int> index; // lower-case name -> variable index

    int varIndex(const QString &name) const;
    int pointCount(int run = 0) const;
    const QVector<double> &x(int run) const { return runs[run].re[0]; }
    QString xName() const { return vars.isEmpty() ? QString() : vars[0].name; }
    QString xUnit() const;
    static QString unitForType(const QString &type);
    QString unitFor(int var) const { return unitForType(vars.value(var).type); }
    void rebuildIndex();
    bool isEmpty() const { return vars.isEmpty() || runs.isEmpty(); }
    // Splits a single run into several where the sweep variable restarts
    // (stepped LTspice raw files store all steps back to back).
    void splitSteps();
};

using DatasetPtr = QSharedPointer<Dataset>;

namespace rawfile {

// Reads every plot contained in a raw file (ngspice can write several).
QList<DatasetPtr> read(const QString &path, QString *error = nullptr);

// Writes a dataset as an ASCII or binary raw file (ngspice/LTspice compatible).
bool write(const QString &path, const Dataset &ds, bool binary, QString *error = nullptr);

Dataset::Kind kindFromPlotName(const QString &plotName);

} // namespace rawfile
