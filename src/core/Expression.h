#pragma once

#include <QHash>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <complex>
#include <functional>
#include <optional>

// Physical unit as exponents of volt, ampere and second. Everything the
// waveform viewer needs (W, Ω, Hz, F, H, ...) is expressible this way.
struct Unit {
    qint8 v = 0, a = 0, s = 0;
    bool known = true;

    static Unit none() { return {}; }
    static Unit volt() { return {1, 0, 0, true}; }
    static Unit amp() { return {0, 1, 0, true}; }
    static Unit second() { return {0, 0, 1, true}; }
    static Unit hertz() { return {0, 0, -1, true}; }
    static Unit unknown() { Unit u; u.known = false; return u; }
    static Unit fromSymbol(const QString &sym);
    bool isDimensionless() const { return known && v == 0 && a == 0 && s == 0; }
    bool operator==(const Unit &o) const { return v == o.v && a == o.a && s == o.s && known == o.known; }
    Unit operator*(const Unit &o) const;
    Unit operator/(const Unit &o) const;
    Unit pow(int n) const;
    QString symbol() const; // "V", "A", "W", "Ω", "V/s", ...
};

// A real or complex vector value (size 1 = scalar, broadcast in operations).
struct Value {
    QVector<double> re;
    QVector<double> im; // only used when complex
    bool complex = false;
    Unit unit;

    static Value scalar(double x, Unit u = Unit()) { Value v; v.re = {x}; v.unit = u; return v; }
    static Value cscalar(std::complex<double> z, Unit u = Unit());
    int size() const { return re.size(); }
    bool isScalar() const { return re.size() == 1; }
    double r(int i) const { return re.size() == 1 ? re[0] : re[i]; }
    double i(int k) const { return !complex ? 0.0 : (im.size() == 1 ? im[0] : im[k]); }
    std::complex<double> c(int k) const { return {r(k), i(k)}; }
    double first() const { return re.isEmpty() ? 0.0 : re[0]; }
};

class ExprNode;
using ExprPtr = QSharedPointer<ExprNode>;

// Supplies signals, variables and the x-axis to the evaluator.
class EvalContext
{
public:
    virtual ~EvalContext() = default;
    // Signals such as "V(out)", "V(a,b)", "I(R1)", "Ic(Q1)", or any raw vector name.
    // `step` selects a specific run for the '@' operator (-1 = current run).
    virtual std::optional<Value> signal(const QString &name, int step) = 0;
    // Plain identifiers (parameters, .meas results, "time", "frequency", ...).
    virtual std::optional<Value> variable(const QString &name, int step) = 0;
    // The x axis (time/frequency/sweep) of the current run, used by d() and integrals.
    virtual std::optional<Value> xAxis(int step) = 0;
};

// Simple map-based context (parameters only, scalars).
class ParamContext : public EvalContext
{
public:
    QHash<QString, Value> vars; // lower-case names
    EvalContext *parent = nullptr;
    std::optional<Value> signal(const QString &name, int step) override;
    std::optional<Value> variable(const QString &name, int step) override;
    std::optional<Value> xAxis(int step) override;
};

class Expression
{
public:
    Expression() = default;
    explicit Expression(const QString &text);

    bool parse(const QString &text);
    bool isValid() const { return m_root != nullptr; }
    QString error() const { return m_error; }
    QString text() const { return m_text; }

    // Evaluates in the given context. On failure returns nullopt and sets error().
    std::optional<Value> evaluate(EvalContext &ctx) const;
    double evaluateScalar(EvalContext &ctx, bool *ok = nullptr) const;

    // Names of signals ("V(out)") and identifiers referenced by the expression.
    QStringList signalNames() const;
    QStringList identifiers() const;

    // User-defined functions shared by all expressions (from .func / plot.defs).
    static void defineFunction(const QString &name, const QStringList &params, const QString &body);
    static void clearFunctions();
    static bool hasFunction(const QString &name);
    // Parses ".func name(a,b) {body}" or ".func name(a,b)=body" lines.
    static bool defineFromDirective(const QString &line);

private:
    ExprPtr m_root;
    QString m_text;
    mutable QString m_error;
};

// True if the identifier introduces a raw signal name in parentheses
// ("V", "I", "Ic", "Ix", ...).
bool isSignalFunction(const QString &ident);
