#include "WaveformView.h"

#include "core/AppSettings.h"
#include "core/Fft.h"
#include "core/Measure.h"
#include "core/PlotSettings.h"
#include "core/TextIO.h"
#include "core/Units.h"
#include "dialogs/FftDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPrinter>
#include <QPushButton>
#include <QSplitter>
#include <QSvgGenerator>
#include <QTextStream>
#include <QVBoxLayout>

#include <cmath>

WaveformView::WaveformView(QWidget *parent)
    : DocumentWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_split = new QSplitter(Qt::Vertical);
    m_split->setChildrenCollapsible(false);
    lay->addWidget(m_split);
    gridOn = AppSettings::instance().waveformGrid();
    markPoints = AppSettings::instance().markDataPoints();
    setFocusPolicy(Qt::StrongFocus);
    addPane(false);
}

WaveformView::~WaveformView()
{
    if (m_readout)
        m_readout->deleteLater();
}

QString WaveformView::displayName() const
{
    if (!m_title.isEmpty())
        return m_title;
    if (!m_rawPath.isEmpty())
        return QFileInfo(m_rawPath).fileName();
    return tr("Waveform");
}

bool WaveformView::isAc() const
{
    return m_data && m_data->complex;
}

QString WaveformView::xUnit() const
{
    if (!m_data)
        return QString();
    if (!xExpression.isEmpty())
        return QString();
    if (m_data->kind == Dataset::Kind::DCSweep) {
        const QString n = m_data->xName();
        if (n.startsWith('V', Qt::CaseInsensitive)) return QStringLiteral("V");
        if (n.startsWith('I', Qt::CaseInsensitive)) return QStringLiteral("A");
        if (n.compare(QLatin1String("temp"), Qt::CaseInsensitive) == 0) return QStringLiteral("°C");
        if (n.startsWith('R', Qt::CaseInsensitive)) return QStringLiteral("Ω");
    }
    return m_data->xUnit();
}

QString WaveformView::xLabel() const
{
    if (!xExpression.isEmpty())
        return xExpression;
    return m_data ? m_data->xName() : QString();
}

void WaveformView::colorFor(WaveTrace &t, int index) const
{
    if (!t.autoColor && t.color.isValid())
        return;
    // Colors are assigned across all panes so traces stay distinguishable.
    int global = index;
    for (WaveformPane *p : m_panes) {
        bool found = false;
        for (int i = 0; i < p->traces.size(); ++i) {
            if (&p->traces[i] == &t) {
                found = true;
                break;
            }
            ++global;
        }
        if (found)
            break;
    }
    const QList<QColor> colors = AppSettings::instance().traceColors();
    t.color = colors[global % colors.size()];
}

void WaveformView::setDataset(DatasetPtr ds, const QString &rawPath, const QString &title)
{
    m_data = ds;
    m_rawPath = rawPath;
    m_title = title;
    ensureStepVisibility();
    xAxis.log = ds && !ds->linearX && (ds->kind == Dataset::Kind::AC || ds->kind == Dataset::Kind::Noise || ds->logScaleX);
    if (ds && ds->kind == Dataset::Kind::FFT)
        xAxis.log = true;
    xAxis.autoRange = true;
    recomputeAll();
    autorangeX();
    emit titleChanged();
}

void WaveformView::updateDataset(DatasetPtr ds)
{
    const bool kindChanged = !m_data || !ds || m_data->kind != ds->kind || m_data->complex != ds->complex;
    m_data = ds;
    ensureStepVisibility();
    if (kindChanged) {
        xAxis.log = ds && !ds->linearX && (ds->kind == Dataset::Kind::AC || ds->kind == Dataset::Kind::Noise || ds->logScaleX);
        xAxis.autoRange = true;
        // Different analysis: traces may not make sense any more.
        for (WaveformPane *p : m_panes)
            p->traces.clear();
    }
    recomputeAll();
    if (xAxis.autoRange || kindChanged)
        autorangeX();
    for (WaveformPane *p : m_panes)
        if (p->yLeft.autoRange || p->yRight.autoRange || kindChanged) {
            p->yLeft.autoRange = p->yRight.autoRange = true;
            p->autorangeY();
        }
    updateReadout();
}

void WaveformView::ensureStepVisibility()
{
    const int runs = m_data ? m_data->runs.size() : 0;
    if (stepVisible.size() != runs)
        stepVisible = QVector<bool>(runs, true);
}

void WaveformView::recomputeAll()
{
    for (WaveformPane *p : m_panes)
        p->recompute();
}

void WaveformView::autorangeX()
{
    if (!m_data || m_data->isEmpty())
        return;
    double lo = INFINITY, hi = -INFINITY;
    bool any = false;
    for (WaveformPane *p : m_panes) {
        for (const WaveTrace &t : p->traces) {
            for (int r = 0; r < t.x.size(); ++r) {
                for (double x : t.x[r]) {
                    if (!std::isfinite(x) || (xAxis.log && x <= 0))
                        continue;
                    lo = std::min(lo, x);
                    hi = std::max(hi, x);
                    any = true;
                }
            }
        }
    }
    if (!any) {
        for (const auto &run : m_data->runs) {
            for (double x : run.re[0]) {
                if (xAxis.log && x <= 0)
                    continue;
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
    }
    if (!std::isfinite(lo) || !std::isfinite(hi))
        return;
    if (hi <= lo) {
        hi = lo + (lo != 0 ? std::fabs(lo) : 1.0);
    }
    xAxis.min = lo;
    xAxis.max = hi;
    xAxis.autoRange = true;
    for (WaveformPane *p : m_panes) {
        p->autorangeY();
        p->update();
    }
}

void WaveformView::pushZoom()
{
    QList<double> z{xAxis.min, xAxis.max, double(xAxis.autoRange)};
    for (WaveformPane *p : m_panes)
        z << p->yLeft.min << p->yLeft.max << p->yRight.min << p->yRight.max << double(p->yLeft.autoRange);
    m_zoomStack << z;
    while (m_zoomStack.size() > 100)
        m_zoomStack.removeFirst();
    emit undoStateChanged();
}

void WaveformView::zoomBack()
{
    if (m_zoomStack.isEmpty()) {
        zoomFit();
        return;
    }
    const QList<double> z = m_zoomStack.takeLast();
    emit undoStateChanged();
    xAxis.min = z[0];
    xAxis.max = z[1];
    xAxis.autoRange = z[2] != 0;
    int k = 3;
    for (WaveformPane *p : m_panes) {
        if (k + 4 >= z.size())
            break;
        p->yLeft.min = z[k];
        p->yLeft.max = z[k + 1];
        p->yRight.min = z[k + 2];
        p->yRight.max = z[k + 3];
        p->yLeft.autoRange = p->yRight.autoRange = z[k + 4] != 0;
        k += 5;
        p->update();
    }
}

void WaveformView::zoomFit()
{
    pushZoom();
    for (WaveformPane *p : m_panes)
        p->yLeft.autoRange = p->yRight.autoRange = true;
    autorangeX();
}

void WaveformView::zoomIn()
{
    pushZoom();
    const double c = (xAxis.min + xAxis.max) / 2, h = (xAxis.max - xAxis.min) / 4;
    if (xAxis.log && xAxis.min > 0) {
        const double lc = (std::log10(xAxis.min) + std::log10(xAxis.max)) / 2, lh = (std::log10(xAxis.max) - std::log10(xAxis.min)) / 4;
        xAxis.min = std::pow(10, lc - lh);
        xAxis.max = std::pow(10, lc + lh);
    } else {
        xAxis.min = c - h;
        xAxis.max = c + h;
    }
    xAxis.autoRange = false;
    for (WaveformPane *p : m_panes)
        p->autorangeY();
}

void WaveformView::zoomOut()
{
    pushZoom();
    if (xAxis.log && xAxis.min > 0) {
        const double lc = (std::log10(xAxis.min) + std::log10(xAxis.max)) / 2, lh = (std::log10(xAxis.max) - std::log10(xAxis.min));
        xAxis.min = std::pow(10, lc - lh);
        xAxis.max = std::pow(10, lc + lh);
    } else {
        const double c = (xAxis.min + xAxis.max) / 2, h = (xAxis.max - xAxis.min);
        xAxis.min = c - h;
        xAxis.max = c + h;
    }
    xAxis.autoRange = false;
    for (WaveformPane *p : m_panes)
        p->autorangeY();
}

// ---------------------------------------------------------------- panes and traces

WaveformPane *WaveformView::addPane(bool above)
{
    auto *pane = new WaveformPane(this);
    connect(pane, &WaveformPane::activated, this, &WaveformView::setActivePane);
    int idx = m_active ? m_panes.indexOf(m_active) : m_panes.size() - 1;
    if (!above)
        ++idx;
    idx = std::clamp(idx, 0, int(m_panes.size()));
    m_panes.insert(idx, pane);
    m_split->insertWidget(idx, pane);
    setActivePane(pane);
    return pane;
}

void WaveformView::deletePane(WaveformPane *pane)
{
    if (m_panes.size() <= 1)
        return;
    for (Cursor &c : cursors)
        if (c.pane == pane)
            c = Cursor();
    const int idx = m_panes.indexOf(pane);
    m_panes.removeAt(idx);
    pane->deleteLater();
    setActivePane(m_panes.value(std::max(0, idx - 1)));
    updateReadout();
}

void WaveformView::movePane(WaveformPane *pane, int delta)
{
    const int idx = m_panes.indexOf(pane);
    const int to = idx + delta;
    if (idx < 0 || to < 0 || to >= m_panes.size())
        return;
    m_panes.move(idx, to);
    m_split->insertWidget(to, pane);
}

void WaveformView::setActivePane(WaveformPane *pane)
{
    if (m_active == pane)
        return;
    m_active = pane;
    for (WaveformPane *p : m_panes)
        p->update();
}

void WaveformView::addTrace(const QString &exprIn, WaveformPane *pane)
{
    const QString expr = exprIn.trimmed();
    if (expr.isEmpty())
        return;
    if (!pane)
        pane = m_active ? m_active : m_panes.first();
    for (const WaveTrace &t : pane->traces)
        if (t.expr.compare(expr, Qt::CaseInsensitive) == 0)
            return;
    WaveTrace t;
    t.expr = expr;
    pane->traces << t;
    const bool firstTraces = std::all_of(m_panes.begin(), m_panes.end(), [](WaveformPane *p) { return p->traces.size() <= 1; });
    pane->recompute();
    if (firstTraces && xAxis.autoRange)
        autorangeX();
    if (pane->yLeft.autoRange || pane->yRight.autoRange)
        pane->autorangeY();
    pane->update();
}

void WaveformView::addTraces(const QStringList &exprs)
{
    for (const QString &e : exprs)
        addTrace(e);
}

void WaveformView::removeTrace(WaveformPane *pane, int index)
{
    if (!pane || index < 0 || index >= pane->traces.size())
        return;
    pane->traces.removeAt(index);
    for (Cursor &c : cursors) {
        if (c.pane == pane) {
            if (c.trace == index)
                c = Cursor();
            else if (c.trace > index)
                --c.trace;
        }
    }
    pane->recompute();
    if (pane->yLeft.autoRange)
        pane->autorangeY();
    updateReadout();
}

void WaveformView::clearTraces()
{
    for (WaveformPane *p : m_panes) {
        p->traces.clear();
        p->update();
    }
    clearCursors();
}

QStringList WaveformView::allTraceExpressions() const
{
    QStringList out;
    for (WaveformPane *p : m_panes)
        for (const WaveTrace &t : p->traces)
            out << t.expr;
    return out;
}

QStringList WaveformView::availableVectors() const
{
    QStringList out;
    if (!m_data)
        return out;
    for (const auto &v : m_data->vars)
        out << v.name;
    return out;
}

// ---------------------------------------------------------------- cursors

void WaveformView::attachCursor(int which, WaveformPane *pane, int trace)
{
    Cursor &c = cursors[which];
    const bool wasOn = c.on;
    c.on = true;
    c.pane = pane;
    c.trace = trace;
    if (!wasOn) {
        const double span = xAxis.max - xAxis.min;
        c.x = xAxis.min + span * (which == 0 ? 0.33 : 0.66);
        if (xAxis.log && xAxis.min > 0)
            c.x = std::pow(10, std::log10(xAxis.min) + (std::log10(xAxis.max) - std::log10(xAxis.min)) * (which == 0 ? 0.33 : 0.66));
    }
    c.run = 0;
    for (int r = 0; r < stepVisible.size(); ++r)
        if (stepVisible[r]) {
            c.run = r;
            break;
        }
    updateReadout();
    for (WaveformPane *p : m_panes)
        p->update();
}

void WaveformView::clearCursors()
{
    cursors[0] = Cursor();
    cursors[1] = Cursor();
    if (m_readout)
        m_readout->hide();
    for (WaveformPane *p : m_panes)
        p->update();
}

void WaveformView::moveCursorToPoint(int which, int direction, int stepCount)
{
    Cursor &c = cursors[which];
    if (!c.on || !c.pane || c.trace < 0 || c.trace >= c.pane->traces.size())
        return;
    const WaveTrace &t = c.pane->traces[c.trace];
    if (c.run >= t.x.size())
        return;
    const QVector<double> &xs = t.x[c.run];
    int k = int(std::lower_bound(xs.begin(), xs.end(), c.x) - xs.begin());
    if (direction > 0) {
        if (k < xs.size() && xs[k] <= c.x)
            ++k;
        k += stepCount - 1;
    } else {
        k -= stepCount;
    }
    k = std::clamp(k, 0, int(xs.size()) - 1);
    c.x = xs[k];
    updateReadout();
    c.pane->update();
}

void WaveformView::updateReadout()
{
    if (!cursors[0].on && !cursors[1].on) {
        if (m_readout)
            m_readout->hide();
        return;
    }
    if (!m_readout) {
        auto *w = new QWidget(window(), Qt::Tool);
        w->setWindowTitle(tr("Cursor Readout"));
        auto *l = new QVBoxLayout(w);
        m_readoutLabel = new QLabel;
        m_readoutLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_readoutLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(m_readoutLabel);
        m_readout = w;
        w->move(window()->geometry().topRight() - QPoint(360, -80));
    }
    QStringList lines;
    double xv[2] = {0, 0}, yv[2] = {0, 0}, iv[2] = {0, 0};
    bool have[2] = {false, false};
    const QString xu = xUnit();
    for (int c = 0; c < 2; ++c) {
        const Cursor &cur = cursors[c];
        if (!cur.on || !cur.pane)
            continue;
        const WaveTrace &t = cur.pane->traces.value(cur.trace);
        double re = 0, im = 0;
        if (!cur.pane->traceValue(cur.trace, cur.run, cur.x, &re, &im))
            continue;
        have[c] = true;
        xv[c] = cur.x;
        yv[c] = re;
        iv[c] = im;
        lines << tr("Cursor %1: %2%3").arg(c + 1).arg(t.expr,
                                                     m_data && m_data->runs.size() > 1 ? QStringLiteral("  (%1)").arg(m_data->runs[cur.run].label) : QString());
        lines << tr("  Horz: %1").arg(units::formatEng(cur.x, xu, 6));
        if (t.complex) {
            const double mag = std::hypot(re, im);
            const double ph = std::atan2(im, re) * 180 / M_PI;
            lines << tr("  Mag:  %1dB  (%2)").arg(QString::number(20 * std::log10(mag), 'g', 6), units::formatEng(mag, t.unitText, 6));
            lines << tr("  Phase: %1°").arg(QString::number(ph, 'f', 3));
            // Group delay from neighbouring points.
            double re1 = 0, im1 = 0;
            const double dx = std::max(std::fabs(cur.x) * 1e-4, 1e-12);
            if (cur.pane->traceValue(cur.trace, cur.run, cur.x + dx, &re1, &im1)) {
                double dph = std::atan2(im1, re1) - std::atan2(im, re);
                while (dph > M_PI) dph -= 2 * M_PI;
                while (dph < -M_PI) dph += 2 * M_PI;
                lines << tr("  Group Delay: %1").arg(units::formatEng(-dph / (2 * M_PI * dx), QStringLiteral("s"), 5));
            }
        } else {
            lines << tr("  Vert: %1").arg(units::formatEng(re, t.unitText, 6));
        }
    }
    if (have[0] && have[1]) {
        const double dx = xv[1] - xv[0];
        lines << tr("Diff (Cursor2 - Cursor1):");
        lines << tr("  Horz: %1").arg(units::formatDelta(dx, xu));
        const bool cx = cursors[0].pane && cursors[0].pane->traces.value(cursors[0].trace).complex;
        if (cx) {
            const double m0 = std::hypot(yv[0], iv[0]), m1 = std::hypot(yv[1], iv[1]);
            lines << tr("  Mag:  %1dB").arg(QString::number(20 * std::log10(m1 / m0), 'g', 6));
            lines << tr("  Phase: %1°").arg(QString::number((std::atan2(iv[1], yv[1]) - std::atan2(iv[0], yv[0])) * 180 / M_PI, 'f', 3));
        } else {
            lines << tr("  Vert: %1").arg(units::formatEng(yv[1] - yv[0], QString(), 6));
            if (dx != 0)
                lines << tr("  Slope: %1").arg(units::formatEng((yv[1] - yv[0]) / dx, QString(), 5));
        }
    }
    m_readoutLabel->setText(lines.join('\n'));
    m_readout->adjustSize();
    m_readout->show();
}

void WaveformView::labelCursorPosition()
{
    for (int c = 0; c < 2; ++c) {
        const Cursor &cur = cursors[c];
        if (!cur.on || !cur.pane)
            continue;
        double re = 0, im = 0;
        if (!cur.pane->traceValue(cur.trace, cur.run, cur.x, &re, &im))
            continue;
        const WaveTrace &t = cur.pane->traces[cur.trace];
        WaveAnnotation a;
        a.kind = WaveAnnotation::Text;
        double y = re;
        if (t.complex && cur.pane->acMode == WaveformPane::AcMode::Bode)
            y = 20 * std::log10(std::hypot(re, im));
        a.a = QPointF(cur.x, y);
        a.text = QStringLiteral("(%1, %2)").arg(units::formatEng(cur.x, xUnit(), 4),
                                                t.complex ? QStringLiteral("%1dB").arg(QString::number(y, 'g', 4)) : units::formatEng(re, t.unitText, 4));
        cur.pane->annotations << a;
        cur.pane->update();
    }
}

void WaveformView::setAnnotationMode(WaveAnnotation::Kind kind)
{
    m_annotationMode = kind;
    emit statusMessage(tr("Click (and drag) in a plot pane to place the annotation"), 4000);
}

void WaveformView::keyPressEvent(QKeyEvent *e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const int which = cursors[1].on && !cursors[0].on ? 1 : (e->modifiers() & Qt::AltModifier ? 1 : 0);
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const int dir = e->key() == Qt::Key_Left ? -1 : 1;
        if (!cursors[0].on && !cursors[1].on) {
            if (shift)
                zoomIn();
            return;
        }
        const int n = (ctrl && shift) ? 100 : ((ctrl || shift) ? 10 : 1);
        moveCursorToPoint(which, dir, n);
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        // Up/Down cycle the cursor through steps (or traces).
        Cursor &c = cursors[which];
        if (!c.on || !m_data)
            return;
        const int runs = m_data->runs.size();
        if (runs > 1) {
            int r = c.run;
            for (int i = 0; i < runs; ++i) {
                r = (r + (e->key() == Qt::Key_Up ? 1 : runs - 1)) % runs;
                if (r < stepVisible.size() && stepVisible[r])
                    break;
            }
            c.run = r;
        } else if (c.pane && c.pane->traces.size() > 1) {
            c.trace = (c.trace + (e->key() == Qt::Key_Up ? 1 : int(c.pane->traces.size()) - 1)) % int(c.pane->traces.size());
        }
        updateReadout();
        for (WaveformPane *p : m_panes)
            p->update();
        return;
    }
    case Qt::Key_Escape:
        if (m_annotationMode) {
            m_annotationMode.reset();
            return;
        }
        clearCursors();
        return;
    default:
        break;
    }
    DocumentWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------- steps / FFT / export

void WaveformView::showStepSelector()
{
    if (!m_data || m_data->runs.size() < 2)
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Select Steps"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *list = new QListWidget;
    for (int r = 0; r < m_data->runs.size(); ++r) {
        auto *it = new QListWidgetItem(QStringLiteral("%1: %2").arg(r + 1).arg(m_data->runs[r].label), list);
        it->setCheckState(r < stepVisible.size() && stepVisible[r] ? Qt::Checked : Qt::Unchecked);
    }
    lay->addWidget(list);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *all = bb->addButton(tr("Select All"), QDialogButtonBox::ActionRole);
    auto *none = bb->addButton(tr("Select None"), QDialogButtonBox::ActionRole);
    connect(all, &QPushButton::clicked, list, [list]() {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Checked);
    });
    connect(none, &QPushButton::clicked, list, [list]() {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Unchecked);
    });
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;
    for (int i = 0; i < list->count() && i < stepVisible.size(); ++i)
        stepVisible[i] = list->item(i)->checkState() == Qt::Checked;
    for (WaveformPane *p : m_panes)
        if (p->yLeft.autoRange)
            p->autorangeY();
        else
            p->update();
}

void WaveformView::openFftDialog()
{
    if (!m_data || m_data->kind != Dataset::Kind::Transient) {
        emit statusMessage(tr("FFT requires transient analysis data"), 3000);
        return;
    }
    FftDialog dlg(availableVectors(), allTraceExpressions(), m_data->runs[0].re[0].isEmpty() ? 0 : m_data->runs[0].re[0].first(),
                  m_data->runs[0].re[0].isEmpty() ? 1 : m_data->runs[0].re[0].last(), xAxis.min, xAxis.max, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList exprs = dlg.expressions();
    if (exprs.isEmpty())
        return;
    auto out = DatasetPtr::create();
    out->kind = Dataset::Kind::FFT;
    out->complex = true;
    out->plotName = QStringLiteral("FFT");
    out->title = tr("FFT of %1").arg(displayName());
    out->vars.append({QStringLiteral("frequency"), QStringLiteral("frequency")});
    QStringList names;
    for (int i = 0; i < exprs.size(); ++i) {
        const QString &e = exprs[i];
        static const QRegularExpression simple(QStringLiteral("^[VI]\\w*\\([^()]*\\)$"), QRegularExpression::CaseInsensitiveOption);
        names << (simple.match(e).hasMatch() ? e : QStringLiteral("fft%1").arg(i + 1));
        out->vars.append({names.last(), e.startsWith('I', Qt::CaseInsensitive) ? QStringLiteral("current") : QStringLiteral("voltage")});
    }
    const int runs = dlg.allSteps() ? m_data->runs.size() : 1;
    for (int r = 0; r < runs; ++r) {
        Dataset::Run run;
        run.label = m_data->runs[r].label;
        DatasetContext ctx(*m_data, r);
        QVector<double> freq;
        for (int i = 0; i < exprs.size(); ++i) {
            Expression ex(exprs[i]);
            auto v = ex.evaluate(ctx);
            QVector<double> re, im;
            if (v) {
                const fft::Spectrum sp = fft::spectrum(m_data->runs[r].re[0], v->re, dlg.startTime(), dlg.endTime(), dlg.points(),
                                                       dlg.window(), dlg.removeDc(), dlg.binsAveraged());
                if (freq.isEmpty())
                    freq = sp.freq;
                for (const auto &z : sp.value) {
                    re << z.real();
                    im << z.imag();
                }
            }
            re.resize(freq.size());
            im.resize(freq.size());
            if (run.re.isEmpty()) {
                run.re << freq;
                run.im << QVector<double>(freq.size(), 0.0);
            }
            run.re << re;
            run.im << im;
        }
        // Skip DC bin for log axes (keep it but the log axis will hide f=0).
        out->runs << run;
    }
    out->rebuildIndex();
    emit requestNewWaveform(out, names, out->title);
}

bool WaveformView::exportData(const QString &path, const QStringList &exprs)
{
    if (!m_data)
        return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts.setRealNumberPrecision(15);
    QStringList header{m_data->xName()};
    for (const QString &e : exprs)
        header << e;
    ts << header.join('\t') << '\n';
    for (int r = 0; r < m_data->runs.size(); ++r) {
        if (m_data->runs.size() > 1)
            ts << "Step Information: " << m_data->runs[r].label << "  (Run: " << (r + 1) << '/' << m_data->runs.size() << ")\n";
        DatasetContext ctx(*m_data, r);
        QList<std::optional<Value>> cols;
        for (const QString &e : exprs)
            cols << Expression(e).evaluate(ctx);
        const QVector<double> &xs = m_data->runs[r].re[0];
        for (int k = 0; k < xs.size(); ++k) {
            ts << xs[k];
            for (const auto &c : cols) {
                ts << '\t';
                if (!c) {
                    ts << "NaN";
                    continue;
                }
                const int kk = c->isScalar() ? 0 : k;
                if (c->complex) {
                    const double re = c->r(kk), im = c->i(kk);
                    ts << "(" << 20 * std::log10(std::hypot(re, im)) << "dB," << std::atan2(im, re) * 180 / M_PI << "°)";
                } else {
                    ts << c->r(kk);
                }
            }
            ts << '\n';
        }
    }
    return true;
}

QString WaveformView::defaultPlotSettingsPath() const
{
    QString base = !m_rawPath.isEmpty() ? m_rawPath : m_ascPath;
    if (base.isEmpty())
        return QString();
    const QFileInfo fi(base);
    return fi.absolutePath() + '/' + fi.completeBaseName() + QStringLiteral(".plt");
}

bool WaveformView::savePlotSettings(const QString &path)
{
    if (!m_data)
        return false;
    PlotSettingsFile file;
    file.load(path); // keep other analyses' sections
    PlotSection &sec = file.sectionFor(m_data->plotName.isEmpty() ? QStringLiteral("Transient Analysis") : m_data->plotName);
    sec.panes.clear();
    for (WaveformPane *p : m_panes) {
        PlotPaneSettings ps;
        for (const WaveTrace &t : p->traces) {
            PlotTrace pt;
            pt.expr = t.expr;
            pt.color = t.autoColor ? QColor() : t.color;
            pt.axis = t.axis;
            ps.traces << pt;
        }
        ps.x.valid = true;
        ps.x.min = xAxis.min;
        ps.x.max = xAxis.max;
        ps.x.log = xAxis.log;
        ps.y[0].valid = true;
        ps.y[0].min = p->yLeft.min;
        ps.y[0].max = p->yLeft.max;
        ps.y[0].log = p->yLeft.log;
        if (m_data->complex && p->acMode == WaveformPane::AcMode::Bode) {
            // LTspice stores a dB axis as linear magnitudes.
            ps.yMode = 2;
            ps.y[0].min = std::pow(10.0, p->yLeft.min / 20);
            ps.y[0].max = std::pow(10.0, p->yLeft.max / 20);
            ps.y[0].log = false;
        }
        if (p->hasRightAxis()) {
            ps.y[1].valid = true;
            ps.y[1].min = p->yRight.min;
            ps.y[1].max = p->yRight.max;
        }
        ps.gridOn = gridOn;
        ps.plotPhase = p->showPhase;
        sec.panes.prepend(ps); // LTspice lists panes from the bottom up
    }
    return file.save(path);
}

bool WaveformView::loadPlotSettings(const QString &path)
{
    if (!m_data)
        return false;
    PlotSettingsFile file;
    if (!file.load(path))
        return false;
    const PlotSection *sec = file.section(m_data->plotName);
    if (!sec)
        return false;
    // Rebuild panes.
    while (m_panes.size() > 1)
        deletePane(m_panes.last());
    m_panes.first()->traces.clear();
    for (int i = 0; i < sec->panes.size(); ++i) {
        WaveformPane *pane = i == 0 ? m_panes.first() : addPane(false);
        const PlotPaneSettings &ps = sec->panes[sec->panes.size() - 1 - i]; // stored bottom pane first
        for (const PlotTrace &pt : ps.traces) {
            WaveTrace t;
            t.expr = pt.expr;
            if (pt.color.isValid()) {
                t.color = pt.color;
                t.autoColor = false;
            }
            pane->traces << t;
        }
        pane->showPhase = ps.plotPhase;
        pane->recompute();
        if (ps.y[0].valid && ps.y[0].max > ps.y[0].min) {
            double lo = ps.y[0].min, hi = ps.y[0].max;
            // A Bode magnitude axis is drawn in dB; the file holds linear magnitudes.
            if (ps.yMode == 2 && m_data->complex && lo > 0) {
                lo = 20 * std::log10(lo);
                hi = 20 * std::log10(hi);
            }
            pane->yLeft.min = lo;
            pane->yLeft.max = hi;
            pane->yLeft.autoRange = false;
        }
        if (ps.y[1].valid && ps.y[1].max > ps.y[1].min) {
            pane->yRight.min = ps.y[1].min;
            pane->yRight.max = ps.y[1].max;
            pane->yRight.autoRange = false;
        }
        if (i == 0 && ps.x.valid && ps.x.max > ps.x.min) {
            xAxis.min = ps.x.min;
            xAxis.max = ps.x.max;
            xAxis.log = ps.x.log || xAxis.log;
            xAxis.autoRange = false;
        }
    }
    for (WaveformPane *p : m_panes) {
        if (p->yLeft.autoRange)
            p->autorangeY();
        p->update();
    }
    return true;
}

void WaveformView::executeMeasScript(const QString &path, QString *log)
{
    bool ok = false;
    const QString text = textio::readText(path, &ok);
    if (!ok || !m_data) {
        if (log)
            *log = tr("Cannot read %1").arg(path);
        return;
    }
    const QStringList stmts = MeasureEngine::extractMeasures(textio::splitLines(text));
    MeasureEngine me;
    QList<QList<MeasResult>> perRun;
    for (int r = 0; r < m_data->runs.size(); ++r)
        perRun << me.evaluate(*m_data, r, stmts);
    QStringList labels;
    for (const auto &run : m_data->runs)
        labels << run.label;
    if (log)
        *log = tr("Executing .MEAS script %1\n").arg(QFileInfo(path).fileName()) + MeasureEngine::formatLog(perRun, labels);
}

// ---------------------------------------------------------------- output

static void renderPanes(QPainter &p, const QList<WaveformPane *> &panes, QSize size)
{
    int y = 0;
    int total = 0;
    for (WaveformPane *pane : panes)
        total += pane->height();
    for (WaveformPane *pane : panes) {
        const int h = total > 0 ? size.height() * pane->height() / total : size.height() / int(panes.size());
        p.save();
        p.translate(0, y);
        const double sx = double(size.width()) / pane->width();
        const double sy = double(h) / pane->height();
        p.scale(sx, sy);
        pane->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
        p.restore();
        y += h;
    }
}

void WaveformView::print(QPrinter *printer)
{
    QPainter p(printer);
    const QRect page = printer->pageRect(QPrinter::DevicePixel).toRect();
    renderPanes(p, m_panes, page.size());
}

void WaveformView::copyImageToClipboard()
{
    QImage img(size() * 2, QImage::Format_ARGB32);
    img.fill(Qt::black);
    QPainter p(&img);
    renderPanes(p, m_panes, img.size());
    p.end();
    QApplication::clipboard()->setImage(img);
    emit statusMessage(tr("Plot image copied to clipboard"), 2000);
}

bool WaveformView::exportImage(const QString &path)
{
    if (path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QSvgGenerator gen;
        gen.setFileName(path);
        gen.setSize(size());
        gen.setViewBox(rect());
        QPainter p(&gen);
        renderPanes(p, m_panes, size());
        return true;
    }
    QImage img(size() * 2, QImage::Format_ARGB32);
    QPainter p(&img);
    renderPanes(p, m_panes, img.size());
    p.end();
    return img.save(path);
}
