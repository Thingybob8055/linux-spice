#include "StepSweep.h"

#include "Expression.h"
#include "Units.h"

#include <QRegularExpression>

#include <cmath>

namespace {

QStringList splitWs(const QString &s)
{
    return s.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

bool evalNumber(const QString &text, double *out)
{
    QString t = text.trimmed();
    if (units::isBraced(t))
        t = t.mid(1, t.size() - 2);
    if (auto v = units::parseSpiceNumber(t); v && QRegularExpression(QStringLiteral("^[-+]?[0-9.]")).match(t).hasMatch()) {
        // Plain numbers (possibly with suffix); make sure no operators follow.
        static const QRegularExpression plain(QStringLiteral("^[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?[A-Za-zµ]*$"));
        if (plain.match(t).hasMatch()) {
            *out = *v;
            return true;
        }
    }
    Expression e(t);
    ParamContext ctx;
    bool ok = false;
    *out = e.evaluateScalar(ctx, &ok);
    return ok;
}

QString numberText(double v)
{
    return QString::number(v, 'g', 15);
}

} // namespace

QString StepAxis::label() const
{
    switch (type) {
    case Type::Temp: return QStringLiteral("temp");
    case Type::ModelParam: return QStringLiteral("%1(%2)").arg(model, modelParam);
    default: return name;
    }
}

QString setNamedParam(const QString &text, const QString &name, const QString &value)
{
    const QRegularExpression re(QStringLiteral("(\\b%1\\s*=\\s*)(\\{[^}]*\\}|[^\\s,)]+)").arg(QRegularExpression::escape(name)),
                                QRegularExpression::CaseInsensitiveOption);
    QString out = text;
    if (re.match(out).hasMatch())
        return out.replace(re, QStringLiteral("\\1") + value);
    return out;
}

bool StepPlan::parse(const QStringList &lines, QString *error)
{
    m_axes.clear();
    for (const QString &raw : lines) {
        const QString line = raw.section(';', 0, 0).trimmed(); // ';' starts a comment
        const QString low = line.toLower();
        if (low.startsWith(QLatin1String(".temp")) && !low.startsWith(QLatin1String(".temp="))) {
            const QStringList t = splitWs(line.mid(5).replace('=', ' '));
            if (t.size() > 1) {
                StepAxis ax;
                ax.type = StepAxis::Type::Temp;
                ax.name = QStringLiteral("temp");
                for (const QString &v : t) {
                    double d;
                    if (evalNumber(v, &d))
                        ax.values << d;
                }
                m_axes << ax;
            }
            continue;
        }
        if (!low.startsWith(QLatin1String(".step")))
            continue;
        QStringList t = splitWs(line.mid(5));
        // Join "NAME (PARAM)" forms written with a space.
        for (int i = 0; i + 1 < t.size(); ++i) {
            if (t[i + 1].startsWith('(') && !t[i].contains('(')) {
                t[i] += t[i + 1];
                t.removeAt(i + 1);
            }
        }
        if (t.isEmpty()) {
            if (error)
                *error = QStringLiteral("Empty .step");
            return false;
        }
        QString mode = QStringLiteral("lin");
        if (QStringList{QStringLiteral("lin"), QStringLiteral("oct"), QStringLiteral("dec")}.contains(t[0].toLower()))
            mode = t.takeFirst().toLower();
        // ".step NPN 2N2222(Vaf) ...": the device type before a model parameter is optional.
        static const QStringList modelTypes = {QStringLiteral("npn"), QStringLiteral("pnp"), QStringLiteral("lpnp"), QStringLiteral("nmos"),
                                               QStringLiteral("pmos"), QStringLiteral("vdmos"), QStringLiteral("njf"), QStringLiteral("pjf"),
                                               QStringLiteral("d"), QStringLiteral("nmf"), QStringLiteral("pmf"), QStringLiteral("sw"),
                                               QStringLiteral("csw")};
        if (t.size() > 1 && modelTypes.contains(t[0].toLower()) && t[1].contains('('))
            t.removeFirst();
        StepAxis ax;
        if (!t.isEmpty() && t[0].compare(QLatin1String("param"), Qt::CaseInsensitive) == 0) {
            t.removeFirst();
            ax.type = StepAxis::Type::Param;
        } else if (!t.isEmpty() && t[0].compare(QLatin1String("temp"), Qt::CaseInsensitive) == 0) {
            ax.type = StepAxis::Type::Temp;
        } else if (!t.isEmpty() && t[0].contains('(')) {
            ax.type = StepAxis::Type::ModelParam;
            const QString spec = t[0];
            ax.model = spec.section('(', 0, 0);
            ax.modelParam = spec.section('(', 1).remove(')');
        } else {
            ax.type = StepAxis::Type::Element;
        }
        if (t.isEmpty()) {
            if (error)
                *error = QStringLiteral("Bad .step: %1").arg(line);
            return false;
        }
        ax.name = t.takeFirst();
        if (!t.isEmpty() && t[0].compare(QLatin1String("list"), Qt::CaseInsensitive) == 0) {
            t.removeFirst();
            for (const QString &v : t) {
                double d;
                if (!evalNumber(v, &d)) {
                    if (error)
                        *error = QStringLiteral("Bad value '%1' in .step").arg(v);
                    return false;
                }
                ax.values << d;
            }
        } else {
            if (t.size() < 3) {
                if (error)
                    *error = QStringLiteral(".step needs start, stop and increment: %1").arg(line);
                return false;
            }
            double start, stop, inc;
            if (!evalNumber(t[0], &start) || !evalNumber(t[1], &stop) || !evalNumber(t[2], &inc)) {
                if (error)
                    *error = QStringLiteral("Bad numbers in .step: %1").arg(line);
                return false;
            }
            if (mode == QLatin1String("lin")) {
                if (inc == 0) {
                    if (error)
                        *error = QStringLiteral("Zero increment in .step");
                    return false;
                }
                const double dir = stop >= start ? 1 : -1;
                inc = std::fabs(inc) * dir;
                const int n = int(std::floor((stop - start) / inc + 1e-9)) + 1;
                for (int i = 0; i < n && i < 100000; ++i)
                    ax.values << start + i * inc;
            } else {
                // inc = points per octave/decade
                if (start <= 0 || stop <= 0 || inc <= 0) {
                    if (error)
                        *error = QStringLiteral("oct/dec .step needs positive values");
                    return false;
                }
                const double base = mode == QLatin1String("oct") ? 2.0 : 10.0;
                const double ratio = std::pow(base, 1.0 / inc);
                double v = start;
                for (int i = 0; i < 100000 && v <= stop * (1 + 1e-9); ++i) {
                    ax.values << v;
                    v *= ratio;
                }
            }
        }
        if (ax.values.isEmpty()) {
            if (error)
                *error = QStringLiteral("No values in .step: %1").arg(line);
            return false;
        }
        m_axes << ax;
    }
    if (m_axes.size() > 3) {
        if (error)
            *error = QStringLiteral("At most three nested .step sweeps are supported");
        return false;
    }
    return true;
}

int StepPlan::runCount() const
{
    if (m_axes.isEmpty())
        return 1;
    int n = 1;
    for (const StepAxis &a : m_axes)
        n *= a.values.size();
    return n;
}

QVector<double> StepPlan::valuesForRun(int run) const
{
    QVector<double> out;
    for (const StepAxis &a : m_axes) {
        out << a.values[run % a.values.size()];
        run /= a.values.size();
    }
    return out;
}

QString StepPlan::runLabel(int run) const
{
    const QVector<double> v = valuesForRun(run);
    QStringList parts;
    for (int i = 0; i < m_axes.size(); ++i)
        parts << QStringLiteral("%1=%2").arg(m_axes[i].label(), units::formatSpice(v[i]));
    return parts.join(' ');
}

QStringList StepPlan::stripStepLines(const QStringList &lines)
{
    QStringList out;
    for (const QString &l : lines) {
        const QString low = l.trimmed().toLower();
        if (low.startsWith(QLatin1String(".step")))
            continue;
        out << l;
    }
    return out;
}

QStringList StepPlan::apply(const QStringList &lines, int run) const
{
    QStringList out;
    const QVector<double> vals = valuesForRun(run);
    // Drop .step lines and multi-value .temp lines.
    for (const QString &l : lines) {
        const QString low = l.trimmed().toLower();
        if (low.startsWith(QLatin1String(".step")))
            continue;
        if (low.startsWith(QLatin1String(".temp"))) {
            bool stepped = false;
            for (const StepAxis &a : m_axes)
                if (a.type == StepAxis::Type::Temp)
                    stepped = true;
            if (stepped)
                continue;
        }
        out << l;
    }

    QStringList extra;
    for (int i = 0; i < m_axes.size(); ++i) {
        const StepAxis &a = m_axes[i];
        const QString value = numberText(vals[i]);
        switch (a.type) {
        case StepAxis::Type::Temp:
            extra << QStringLiteral(".temp %1").arg(value);
            break;
        case StepAxis::Type::Param: {
            // Remove existing definitions of this parameter from .param lines.
            const QRegularExpression defRe(QStringLiteral("(^|\\s)%1\\s*=\\s*(\\{[^}]*\\}|\\S+)").arg(QRegularExpression::escape(a.name)),
                                           QRegularExpression::CaseInsensitiveOption);
            for (QString &l : out) {
                if (!l.trimmed().toLower().startsWith(QLatin1String(".param")))
                    continue;
                l.replace(defRe, QStringLiteral("\\1"));
                if (l.trimmed().compare(QLatin1String(".param"), Qt::CaseInsensitive) == 0)
                    l = QStringLiteral("* (stepped parameter)");
            }
            extra << QStringLiteral(".param %1=%2").arg(a.name, value);
            break;
        }
        case StepAxis::Type::Element: {
            const QChar kind = a.name.isEmpty() ? QChar() : a.name[0].toUpper();
            for (QString &l : out) {
                QStringList t = splitWs(l);
                if (t.isEmpty() || t[0].compare(a.name, Qt::CaseInsensitive) != 0)
                    continue;
                if (kind == 'R' || kind == 'C' || kind == 'L') {
                    if (t.size() >= 4) {
                        // Value may be written "R=..." / "C=..."
                        if (t[3].contains('='))
                            t[3] = t[3].section('=', 0, 0) + '=' + value;
                        else
                            t[3] = value;
                    }
                } else if (kind == 'V' || kind == 'I') {
                    if (t.size() >= 4) {
                        int idx = 3;
                        if (t[3].compare(QLatin1String("DC"), Qt::CaseInsensitive) == 0)
                            idx = 4;
                        double dummy;
                        if (idx < t.size() && evalNumber(t[idx], &dummy) && !t[idx].contains('('))
                            t[idx] = value;
                        else
                            t.insert(3, QStringLiteral("DC %1").arg(value));
                    } else {
                        t << value;
                    }
                } else {
                    // Generic: replace the last token that looks like a value.
                    if (t.size() >= 4)
                        t.last() = value;
                }
                l = t.join(' ');
            }
            break;
        }
        case StepAxis::Type::ModelParam: {
            bool found = false;
            for (QString &l : out) {
                const QString low = l.trimmed().toLower();
                if (!low.startsWith(QLatin1String(".model")))
                    continue;
                const QStringList t = splitWs(l);
                if (t.size() < 3 || t[1].compare(a.model, Qt::CaseInsensitive) != 0)
                    continue;
                found = true;
                const QString updated = setNamedParam(l, a.modelParam, value);
                if (updated != l) {
                    l = updated;
                } else {
                    const int close = l.lastIndexOf(')');
                    if (close > 0)
                        l.insert(close, QStringLiteral(" %1=%2").arg(a.modelParam, value));
                    else
                        l += QStringLiteral(" %1=%2").arg(a.modelParam, value);
                }
            }
            if (!found)
                extra << QStringLiteral("*@stepmodel %1 %2 %3").arg(a.model, a.modelParam, value); // applied by the translator to library models
            break;
        }
        }
    }
    // Insert the extra lines before ".end".
    int endIdx = out.size();
    for (int i = out.size() - 1; i >= 0; --i) {
        if (out[i].trimmed().compare(QLatin1String(".end"), Qt::CaseInsensitive) == 0) {
            endIdx = i;
            break;
        }
    }
    for (int i = extra.size() - 1; i >= 0; --i)
        out.insert(endIdx, extra[i]);
    return out;
}
