#pragma once

#include "Expression.h"
#include "RawFile.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <complex>

// Evaluation context bound to one run of a dataset. Resolves V(a), V(a,b),
// I(R1), raw vector names, the sweep variable, parameters and measurement results.
class DatasetContext : public EvalContext
{
public:
    DatasetContext(const Dataset &ds, int run);

    std::optional<Value> signal(const QString &name, int step) override;
    std::optional<Value> variable(const QString &name, int step) override;
    std::optional<Value> xAxis(int step) override;

    QHash<QString, Value> params; // lower-case name -> value (scalars)
    int run() const { return m_run; }

private:
    std::optional<Value> vector(int var, int run) const;
    std::optional<Value> nodeVoltage(const QString &node, int run) const;
    const Dataset &m_ds;
    int m_run;
};

struct MeasResult {
    QString name;        // lower case
    QString analysis;    // TRAN, AC, ...
    bool ok = false;
    std::complex<double> value;
    bool complex = false;
    double at = qQNaN();
    double from = qQNaN();
    double to = qQNaN();
    QString logLine;     // formatted like LTspice's log
    QString exprText;    // what was measured (for step tables)
};

class MeasureEngine
{
public:
    enum class ComplexFormat { DbDegrees, Polar, Cartesian };

    // Returns the .meas statements (joined continuation lines) found in a netlist.
    static QStringList extractMeasures(const QStringList &lines);
    // Analysis keyword of a .meas statement ("TRAN", "AC", ... or "" if omitted).
    static QString measAnalysis(const QString &stmt);
    static bool appliesTo(const QString &stmt, Dataset::Kind kind);

    // Evaluate all statements for one run, in order (later ones may use earlier results).
    QList<MeasResult> evaluate(const Dataset &ds, int run, const QStringList &statements,
                               const QHash<QString, double> &params = {});

    // Formats the results of all runs as the log section LTspice produces.
    static QString formatLog(const QList<QList<MeasResult>> &perRun, const QStringList &stepLabels);

    ComplexFormat complexFormat = ComplexFormat::DbDegrees;

private:
    MeasResult evaluateOne(const Dataset &ds, DatasetContext &ctx, const QString &stmt);
};

QString formatMeasNumber(double v);
