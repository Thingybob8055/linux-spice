#include "PlotSettings.h"

#include "TextIO.h"

#include <QRegularExpression>

namespace {

// Parses "(' ',0,0.01,0,3)" -> unit, ?, min/step/max layout used by LTspice:
// X: ('unit', log?, min, step, max)  Y[i]: ('unit', log?, min, step, max)
PlotAxis parseAxis(const QString &text)
{
    PlotAxis a;
    static const QRegularExpression re(QStringLiteral("\\(\\s*'(.?)'\\s*,\\s*([^,]+),\\s*([^,]+),\\s*([^,]+),\\s*([^)]+)\\)"));
    const auto m = re.match(text);
    if (!m.hasMatch())
        return a;
    a.valid = true;
    a.unitChar = m.captured(1);
    a.log = m.captured(2).trimmed().toInt() != 0;
    a.min = m.captured(3).trimmed().toDouble();
    a.step = m.captured(4).trimmed().toDouble();
    a.max = m.captured(5).trimmed().toDouble();
    return a;
}

QString axisText(const PlotAxis &a)
{
    return QStringLiteral("('%1',%2,%3,%4,%5)")
        .arg(a.unitChar.isEmpty() ? QStringLiteral(" ") : a.unitChar)
        .arg(a.log ? 1 : 0)
        .arg(a.min, 0, 'g', 12)
        .arg(a.step, 0, 'g', 12)
        .arg(a.max, 0, 'g', 12);
}

} // namespace

bool PlotSettingsFile::load(const QString &path, QString *error)
{
    bool ok = false;
    const QString text = textio::readText(path, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(path);
        return false;
    }
    sections.clear();
    const QStringList lines = textio::splitLines(text);
    PlotSection *sec = nullptr;
    PlotPaneSettings *pane = nullptr;
    int depth = 0;
    static const QRegularExpression traceRe(QStringLiteral("\\{(\\d+),(\\d+),\"([^\"]*)\"\\}"));
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        if (l.startsWith('[') && l.endsWith(']')) {
            sections.append(PlotSection{l.mid(1, l.size() - 2), {}});
            sec = &sections.last();
            pane = nullptr;
            depth = 0;
            continue;
        }
        if (!sec)
            continue;
        if (l == QLatin1String("{")) {
            ++depth;
            if (depth == 2) {
                sec->panes.append(PlotPaneSettings());
                pane = &sec->panes.last();
            }
            continue;
        }
        if (l == QLatin1String("}") || l == QLatin1String("},")) { // panes are comma-separated
            if (depth == 2)
                pane = nullptr;
            --depth;
            continue;
        }
        if (!pane)
            continue;
        pane->rawText += raw + '\n';
        if (l.startsWith(QLatin1String("traces:"))) {
            auto it = traceRe.globalMatch(l);
            while (it.hasNext()) {
                const auto m = it.next();
                PlotTrace t;
                const quint32 code = m.captured(1).toUInt();
                t.expr = m.captured(3);
                // Low 24 bits encode the color index in LTspice files; keep automatic.
                Q_UNUSED(code);
                t.axis = m.captured(2).toInt();
                pane->traces.append(t);
            }
        } else if (l.startsWith(QLatin1String("X:"))) {
            pane->x = parseAxis(l.mid(2));
        } else if (l.startsWith(QLatin1String("Y["))) {
            const int idx = l.mid(2, 1).toInt();
            if (idx >= 0 && idx < 3)
                pane->y[idx] = parseAxis(l.mid(l.indexOf(':') + 1));
        } else if (l.startsWith(QLatin1String("Log:"))) {
            const QStringList t = l.mid(4).split(' ', Qt::SkipEmptyParts);
            pane->x.log = t.value(0).toInt() != 0;
            pane->yMode = t.value(1).toInt();
            pane->y[0].log = pane->yMode == 1;
        } else if (l.startsWith(QLatin1String("GridStyle:"))) {
            pane->gridOn = l.mid(10).trimmed().toInt() != 0;
        } else if (l.startsWith(QLatin1String("PltMag:"))) {
            pane->plotMag = l.mid(7).trimmed().toInt() != 0;
        } else if (l.startsWith(QLatin1String("PltPhi:"))) {
            pane->plotPhase = l.mid(7).trimmed().section(' ', 0, 0).toInt() != 0;
        }
    }
    return true;
}

bool PlotSettingsFile::save(const QString &path, QString *error) const
{
    QStringList out;
    for (const PlotSection &s : sections) {
        out << QStringLiteral("[%1]").arg(s.analysis);
        out << QStringLiteral("{");
        out << QStringLiteral("   Npanes: %1").arg(s.panes.size());
        for (const PlotPaneSettings &p : s.panes) {
            out << QStringLiteral("   {");
            QString traces = QStringLiteral("      traces: %1").arg(p.traces.size());
            int code = 524290;
            for (const PlotTrace &t : p.traces)
                traces += QStringLiteral(" {%1,%2,\"%3\"}").arg(code++).arg(t.axis).arg(t.expr);
            out << traces;
            if (p.x.valid)
                out << QStringLiteral("      X: %1").arg(axisText(p.x));
            for (int i = 0; i < 3; ++i)
                if (p.y[i].valid)
                    out << QStringLiteral("      Y[%1]: %2").arg(i).arg(axisText(p.y[i]));
            out << QStringLiteral("      Log: %1 %2 0").arg(p.x.log ? 1 : 0).arg(p.yMode != 0 ? p.yMode : (p.y[0].log ? 1 : 0));
            out << QStringLiteral("      GridStyle: %1").arg(p.gridOn ? 1 : 0);
            if (s.analysis.contains(QLatin1String("AC"), Qt::CaseInsensitive)) {
                out << QStringLiteral("      PltMag: %1").arg(p.plotMag ? 1 : 0);
                out << QStringLiteral("      PltPhi: %1 0").arg(p.plotPhase ? 1 : 0);
            }
            out << QStringLiteral("   }");
        }
        out << QStringLiteral("}");
    }
    textio::TextFile tf;
    tf.text = out.join('\n') + '\n';
    tf.encoding = textio::Encoding::Latin1;
    return textio::writeFile(path, tf, error);
}

const PlotSection *PlotSettingsFile::section(const QString &analysis) const
{
    for (const PlotSection &s : sections)
        if (s.analysis.compare(analysis, Qt::CaseInsensitive) == 0)
            return &s;
    return nullptr;
}

PlotSection &PlotSettingsFile::sectionFor(const QString &analysis)
{
    for (PlotSection &s : sections)
        if (s.analysis.compare(analysis, Qt::CaseInsensitive) == 0)
            return s;
    sections.append(PlotSection{analysis, {}});
    return sections.last();
}
