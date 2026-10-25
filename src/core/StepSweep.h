#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// Expansion of LTspice .step (and multi-value .temp) directives into a list of
// individual simulation runs. Each run is produced by editing the netlist.
struct StepAxis {
    enum class Type { Param, Element, Temp, ModelParam };
    Type type = Type::Param;
    QString name;       // parameter / element name ("R1") / "temp"
    QString model;      // for ModelParam: model name
    QString modelParam; // for ModelParam: parameter name
    QVector<double> values;

    QString label() const; // "R", "temp", "2N2222(VAF)"
};

class StepPlan
{
public:
    // Parses all .step lines (and .temp with several values). Returns false and
    // sets error on a syntax problem. Lines that are not .step/.temp are ignored.
    bool parse(const QStringList &netlistLines, QString *error = nullptr);

    bool isEmpty() const { return m_axes.isEmpty(); }
    int runCount() const;
    const QList<StepAxis> &axes() const { return m_axes; }
    // Value of each axis for a run (first axis varies fastest).
    QVector<double> valuesForRun(int run) const;
    QString runLabel(int run) const; // "R=1K C=10n"

    // Returns a copy of the netlist with the .step lines removed and the
    // values of `run` applied.
    QStringList apply(const QStringList &netlistLines, int run) const;
    // Removes .step lines (used when there is nothing to step).
    static QStringList stripStepLines(const QStringList &lines);

private:
    QList<StepAxis> m_axes;
};

// Helper: replace or add "name=value" inside a parameter list string.
QString setNamedParam(const QString &text, const QString &name, const QString &value);
