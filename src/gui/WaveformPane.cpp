#include "WaveformView.h"

#include "core/AppSettings.h"
#include "core/Measure.h"
#include "core/Units.h"
#include "dialogs/AxisDialog.h"

#include <QApplication>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDrag>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>

namespace {

struct Ticks {
    QVector<double> major;
    QVector<double> minor;
};

Ticks linearTicks(double lo, double hi, int target)
{
    Ticks t;
    if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi))
        return t;
    const double raw = (hi - lo) / std::max(1, target);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double step = mag;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (mag * m >= raw) {
            step = mag * m;
            break;
        }
    }
    const double start = std::ceil(lo / step - 1e-9) * step;
    for (double v = start; v <= hi + step * 1e-9 && t.major.size() < 200; v += step)
        t.major << (std::fabs(v) < step * 1e-9 ? 0.0 : v);
    return t;
}

Ticks logTicks(double lo, double hi)
{
    Ticks t;
    if (lo <= 0 || hi <= lo)
        return t;
    const int d0 = int(std::floor(std::log10(lo)));
    const int d1 = int(std::ceil(std::log10(hi)));
    const int decades = d1 - d0;
    if (hi / lo < 10) {
        // Less than a decade: linearly spaced values (positioned logarithmically).
        Ticks lin = linearTicks(lo, hi, 10);
        return lin;
    }
    const int every = decades > 12 ? int(std::ceil(decades / 12.0)) : 1;
    for (int d = d0; d <= d1; ++d) {
        const double base = std::pow(10.0, d);
        if (base >= lo * (1 - 1e-12) && base <= hi * (1 + 1e-12) && (d - d0) % every == 0)
            t.major << base;
        if (decades <= 6)
            for (int m = 2; m <= 9; ++m) {
                const double v = base * m;
                if (v > lo && v < hi)
                    t.minor << v;
            }
    }
    if (t.major.size() < 2) {
        // Narrow log range: label some minor ticks too.
        for (double v : std::as_const(t.minor))
            t.major << v;
        std::sort(t.major.begin(), t.major.end());
        t.minor.clear();
    }
    return t;
}

QString axisLabel(double v, const QString &unit, double span)
{
    if (unit == QLatin1String("dB"))
        return QStringLiteral("%1dB").arg(QString::number(v, 'g', span < 0.1 ? 6 : 4));
    if (unit == QStringLiteral("°"))
        return QStringLiteral("%1°").arg(QString::number(v, 'g', span < 1 ? 6 : 4));
    int sig = 4;
    if (span > 0 && std::fabs(v) > 0)
        sig = std::clamp(int(std::ceil(std::log10(std::fabs(v) / span))) + 2, 3, 9);
    return units::formatEng(v, unit, sig);
}

double unwrapStep(double prev, double cur)
{
    while (cur - prev > 180)
        cur -= 360;
    while (cur - prev < -180)
        cur += 360;
    return cur;
}

} // namespace

WaveformPane::WaveformPane(WaveformView *view)
    : QWidget(view)
    , m_view(view)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(60);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAcceptDrops(false);
}

bool WaveformPane::hasRightAxis() const
{
    if (m_view->isAc())
        return (acMode == AcMode::Bode || acMode == AcMode::LinearMag) && (showPhase || groupDelay) && !traces.isEmpty();
    for (const WaveTrace &t : traces)
        if (t.axis == 1)
            return true;
    return false;
}

QString WaveformPane::leftUnit() const
{
    if (m_view->isAc()) {
        if (acMode == AcMode::Bode)
            return QStringLiteral("dB");
        if (!traces.isEmpty())
            return traces[0].unitText;
        return QString();
    }
    for (const WaveTrace &t : traces)
        if (t.axis == 0)
            return t.unitText;
    return QString();
}

QString WaveformPane::rightUnit() const
{
    if (m_view->isAc())
        return groupDelay ? QStringLiteral("s") : QStringLiteral("°");
    for (const WaveTrace &t : traces)
        if (t.axis == 1)
            return t.unitText;
    return QString();
}

QRectF WaveformPane::plotRect() const
{
    const int top = 6 + int(std::max<qsizetype>(1, m_labelRects.isEmpty() ? 1 : 1)) * (fontMetrics().height() + 4);
    const double left = 72;
    const double right = hasRightAxis() ? 72 : 20;
    const double bottom = fontMetrics().height() + 10;
    return QRectF(left, top, std::max(10.0, width() - left - right), std::max(10.0, height() - top - bottom));
}

double WaveformPane::xToPixel(double x) const
{
    const QRectF pr = plotRect();
    const AxisRange &xa = m_view->xAxis;
    if (xa.log) {
        if (x <= 0 || xa.min <= 0)
            return pr.left() - 1e6;
        return pr.left() + (std::log10(x) - std::log10(xa.min)) / (std::log10(xa.max) - std::log10(xa.min)) * pr.width();
    }
    return pr.left() + (x - xa.min) / (xa.max - xa.min) * pr.width();
}

double WaveformPane::pixelToX(double px) const
{
    const QRectF pr = plotRect();
    const AxisRange &xa = m_view->xAxis;
    const double f = (px - pr.left()) / pr.width();
    if (xa.log && xa.min > 0)
        return std::pow(10.0, std::log10(xa.min) + f * (std::log10(xa.max) - std::log10(xa.min)));
    return xa.min + f * (xa.max - xa.min);
}

double WaveformPane::yToPixel(double y, bool right) const
{
    const QRectF pr = plotRect();
    const AxisRange &ya = right ? yRight : yLeft;
    if (ya.log) {
        if (y <= 0 || ya.min <= 0)
            return pr.bottom() + 1e6;
        return pr.bottom() - (std::log10(y) - std::log10(ya.min)) / (std::log10(ya.max) - std::log10(ya.min)) * pr.height();
    }
    return pr.bottom() - (y - ya.min) / (ya.max - ya.min) * pr.height();
}

double WaveformPane::pixelToY(double py, bool right) const
{
    const QRectF pr = plotRect();
    const AxisRange &ya = right ? yRight : yLeft;
    const double f = (pr.bottom() - py) / pr.height();
    if (ya.log && ya.min > 0)
        return std::pow(10.0, std::log10(ya.min) + f * (std::log10(ya.max) - std::log10(ya.min)));
    return ya.min + f * (ya.max - ya.min);
}

void WaveformPane::recompute()
{
    const DatasetPtr ds = m_view->dataset();
    for (int i = 0; i < traces.size(); ++i) {
        WaveTrace &t = traces[i];
        t.x.clear();
        t.re.clear();
        t.im.clear();
        t.error.clear();
        if (!ds || ds->isEmpty())
            continue;
        Expression e(t.expr);
        if (!e.isValid()) {
            t.error = e.error();
            continue;
        }
        Expression xe(m_view->xExpression);
        for (int r = 0; r < ds->runs.size(); ++r) {
            DatasetContext ctx(*ds, r);
            auto v = e.evaluate(ctx);
            if (!v) {
                t.error = e.error();
                break;
            }
            QVector<double> xs = ds->runs[r].re[0];
            if (!m_view->xExpression.isEmpty() && xe.isValid()) {
                auto xv = xe.evaluate(ctx);
                if (xv && xv->size() == xs.size())
                    xs = xv->re;
            }
            const int n = xs.size();
            QVector<double> re(n), im;
            if (v->complex)
                im.resize(n);
            for (int k = 0; k < n; ++k) {
                const int kk = v->isScalar() ? 0 : std::min(k, v->size() - 1);
                re[k] = v->r(kk);
                if (v->complex)
                    im[k] = v->i(kk);
            }
            t.x << xs;
            t.re << re;
            if (v->complex)
                t.im << im;
            t.complex = v->complex;
            t.unit = v->unit;
        }
        t.unitText = t.unit.symbol();
        m_view->colorFor(t, i);
    }
    // Axis assignment for real data: first unit -> left, second unit -> right.
    if (!m_view->isAc()) {
        QString first, second;
        for (WaveTrace &t : traces) {
            if (first.isNull() || t.unitText == first) {
                if (first.isNull())
                    first = t.unitText;
                t.axis = 0;
            } else if (second.isNull() || t.unitText == second) {
                if (second.isNull())
                    second = t.unitText;
                t.axis = 1;
            } else {
                t.axis = 0;
            }
        }
    }
    update();
}

bool WaveformPane::displayValues(const WaveTrace &t, int run, QVector<double> &x, QVector<double> &y, QVector<double> &y2) const
{
    if (run >= t.x.size())
        return false;
    x = t.x[run];
    y.clear();
    y2.clear();
    if (!t.complex || t.im.size() <= run) {
        y = t.re[run];
        return true;
    }
    const QVector<double> &re = t.re[run];
    const QVector<double> &im = t.im[run];
    const int n = re.size();
    y.resize(n);
    switch (acMode) {
    case AcMode::Bode:
    case AcMode::LinearMag: {
        y2.resize(n);
        double prev = 0;
        for (int k = 0; k < n; ++k) {
            const double mag = std::hypot(re[k], im[k]);
            y[k] = acMode == AcMode::Bode ? 20 * std::log10(mag) : mag;
            double ph = std::atan2(im[k], re[k]) * 180 / M_PI;
            if (k > 0)
                ph = unwrapStep(prev, ph);
            prev = ph;
            y2[k] = ph;
        }
        if (groupDelay) {
            QVector<double> gd(n);
            for (int k = 0; k < n; ++k) {
                const int a = std::max(0, k - 1), b = std::min(n - 1, k + 1);
                const double dw = 2 * M_PI * (x[b] - x[a]);
                gd[k] = dw != 0 ? -((y2[b] - y2[a]) * M_PI / 180) / dw : 0;
            }
            y2 = gd;
        }
        break;
    }
    case AcMode::Nyquist:
        x = re;
        y = im;
        break;
    case AcMode::Cartesian:
        y = re;
        y2 = im;
        break;
    }
    return true;
}

bool WaveformPane::traceValue(int traceIndex, int run, double xv, double *outRe, double *outIm) const
{
    if (traceIndex < 0 || traceIndex >= traces.size())
        return false;
    const WaveTrace &t = traces[traceIndex];
    if (run >= t.x.size())
        return false;
    const QVector<double> &xs = t.x[run];
    const int n = xs.size();
    if (n == 0)
        return false;
    int k = int(std::lower_bound(xs.begin(), xs.end(), xv) - xs.begin());
    k = std::clamp(k, 1, std::max(1, n - 1));
    if (n == 1)
        k = 0;
    const double x0 = xs[std::max(0, k - 1)], x1 = xs[k];
    const double f = x1 != x0 ? std::clamp((xv - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
    *outRe = t.re[run][std::max(0, k - 1)] + f * (t.re[run][k] - t.re[run][std::max(0, k - 1)]);
    if (outIm)
        *outIm = t.complex && run < t.im.size() ? t.im[run][std::max(0, k - 1)] + f * (t.im[run][k] - t.im[run][std::max(0, k - 1)]) : 0.0;
    return true;
}

void WaveformPane::autorangeY()
{
    double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    const AxisRange &xa = m_view->xAxis;
    for (const WaveTrace &t : traces) {
        for (int r = 0; r < t.x.size(); ++r) {
            if (r < m_view->stepVisible.size() && !m_view->stepVisible[r])
                continue;
            QVector<double> x, y, y2;
            if (!displayValues(t, r, x, y, y2))
                continue;
            const bool nyq = m_view->isAc() && acMode == AcMode::Nyquist;
            for (int k = 0; k < y.size(); ++k) {
                if (!nyq && (x[k] < xa.min || x[k] > xa.max))
                    continue;
                const int ax = m_view->isAc() ? 0 : t.axis;
                if (std::isfinite(y[k])) {
                    lo[ax] = std::min(lo[ax], y[k]);
                    hi[ax] = std::max(hi[ax], y[k]);
                }
                if (!y2.isEmpty() && std::isfinite(y2[k])) {
                    const int ax2 = (acMode == AcMode::Cartesian) ? 0 : 1;
                    lo[ax2] = std::min(lo[ax2], y2[k]);
                    hi[ax2] = std::max(hi[ax2], y2[k]);
                }
            }
        }
    }
    for (int a = 0; a < 2; ++a) {
        AxisRange &ya = a == 0 ? yLeft : yRight;
        if (!ya.autoRange)
            continue;
        if (!std::isfinite(lo[a])) {
            ya.min = 0;
            ya.max = 1;
            continue;
        }
        double span = hi[a] - lo[a];
        if (span <= std::fabs(hi[a]) * 1e-12 || span == 0) {
            const double c = lo[a];
            span = std::fabs(c) > 0 ? std::fabs(c) * 0.1 : 1.0;
            lo[a] = c - span;
            hi[a] = c + span;
        }
        if (ya.log) {
            ya.min = std::max(lo[a], hi[a] * 1e-12);
            ya.max = hi[a];
            if (ya.min <= 0)
                ya.min = 1e-15;
            continue;
        }
        // Round to nice ticks.
        const Ticks tk = linearTicks(lo[a], hi[a], 8);
        double step = tk.major.size() >= 2 ? tk.major[1] - tk.major[0] : (hi[a] - lo[a]) / 8;
        ya.min = std::floor(lo[a] / step - 1e-9) * step;
        ya.max = std::ceil(hi[a] / step + 1e-9) * step;
        if (m_view->isAc() && a == 1 && !groupDelay) {
            // Phase axis in multiples of 30°.
            ya.min = std::floor(lo[a] / 30) * 30;
            ya.max = std::ceil(hi[a] / 30) * 30;
            if (ya.max - ya.min < 60)
                ya.max = ya.min + 60;
        }
    }
    update();
}

// ---------------------------------------------------------------- painting

void WaveformPane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const AppSettings &s = AppSettings::instance();
    const QColor bg = s.color(QStringLiteral("wave.background"));
    p.fillRect(rect(), bg);
    p.setRenderHint(QPainter::Antialiasing);
    drawLegend(p);
    const QRectF pr = plotRect();
    drawAxes(p, pr);
    p.save();
    p.setClipRect(pr.adjusted(-1, -1, 1, 1));
    drawTraces(p, pr);
    drawAnnotations(p, pr);
    drawCursors(p, pr);
    p.restore();
    if (m_zoomDrag) {
        p.setPen(QPen(s.color(QStringLiteral("wave.cursor")), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(m_pressPos, m_mousePos).normalized());
    }
    if (m_view->activePane() == this && m_view->panes().size() > 1) {
        p.setPen(QPen(s.color(QStringLiteral("wave.cursor")), 1, Qt::DotLine));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void WaveformPane::drawLegend(QPainter &p)
{
    m_labelRects.clear();
    const QFontMetrics fm = fontMetrics();
    int x = 72;
    const int y = 4;
    for (int i = 0; i < traces.size(); ++i) {
        const WaveTrace &t = traces[i];
        QString label = t.expr;
        if (!t.error.isEmpty())
            label += QStringLiteral("  [%1]").arg(t.error);
        for (int c = 1; c >= 0; --c)
            if (m_view->cursors[c].on && m_view->cursors[c].pane == this && m_view->cursors[c].trace == i)
                label = QStringLiteral("[%1] ").arg(c + 1) + label;
        const int w = fm.horizontalAdvance(label) + 12;
        const QRect r(x, y, w, fm.height() + 2);
        m_labelRects << r;
        p.setPen(t.error.isEmpty() ? t.color : QColor(255, 80, 80));
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, label);
        x += w + 8;
    }
}

void WaveformPane::drawAxes(QPainter &p, const QRectF &pr)
{
    const AppSettings &s = AppSettings::instance();
    const QColor axisColor = s.color(QStringLiteral("wave.axis"));
    const QColor gridColor = s.color(QStringLiteral("wave.grid"));
    const QColor textColor = s.color(QStringLiteral("wave.text"));
    const bool nyq = m_view->isAc() && acMode == AcMode::Nyquist;
    const QFontMetrics fm = fontMetrics();

    // X axis
    const AxisRange &xa = m_view->xAxis;
    const Ticks xt = xa.log ? logTicks(xa.min, xa.max) : linearTicks(xa.min, xa.max, std::max(2, int(pr.width() / 90)));
    p.setPen(QPen(gridColor, 1, Qt::DotLine));
    if (m_view->gridOn) {
        for (double v : xt.minor) {
            const double px = xToPixel(v);
            p.drawLine(QPointF(px, pr.top()), QPointF(px, pr.bottom()));
        }
        p.setPen(QPen(gridColor, 1, Qt::DashLine));
        for (double v : xt.major) {
            const double px = xToPixel(v);
            p.drawLine(QPointF(px, pr.top()), QPointF(px, pr.bottom()));
        }
    }
    p.setPen(textColor);
    const QString xu = nyq ? leftUnit() : m_view->xUnit();
    for (double v : xt.major) {
        const double px = xToPixel(v);
        const QString lab = axisLabel(v, xu, xa.max - xa.min);
        const double w = fm.horizontalAdvance(lab) + 4;
        // Keep labels inside the pane horizontally.
        const double left = std::clamp(px - w / 2, pr.left() - 4, double(width()) - w);
        p.drawText(QRectF(left, pr.bottom() + 2, w, fm.height()), Qt::AlignHCenter | Qt::AlignTop, lab);
    }

    // Left Y axis
    const Ticks yt = yLeft.log ? logTicks(yLeft.min, yLeft.max) : linearTicks(yLeft.min, yLeft.max, std::max(2, int(pr.height() / 40)));
    if (m_view->gridOn) {
        p.setPen(QPen(gridColor, 1, Qt::DotLine));
        for (double v : yt.minor) {
            const double py = yToPixel(v);
            p.drawLine(QPointF(pr.left(), py), QPointF(pr.right(), py));
        }
        p.setPen(QPen(gridColor, 1, Qt::DashLine));
        for (double v : yt.major) {
            const double py = yToPixel(v);
            p.drawLine(QPointF(pr.left(), py), QPointF(pr.right(), py));
        }
    }
    p.setPen(textColor);
    const QString lu = leftUnit();
    for (double v : yt.major) {
        const double py = yToPixel(v);
        p.drawText(QRectF(0, py - fm.height() / 2.0, pr.left() - 4, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   axisLabel(v, lu, yLeft.max - yLeft.min));
    }
    // Right Y axis
    if (hasRightAxis()) {
        const Ticks rt = yRight.log ? logTicks(yRight.min, yRight.max) : linearTicks(yRight.min, yRight.max, std::max(2, int(pr.height() / 40)));
        const QString ru = rightUnit();
        for (double v : rt.major) {
            const double py = yToPixel(v, true);
            p.drawText(QRectF(pr.right() + 4, py - fm.height() / 2.0, 70, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       axisLabel(v, ru, yRight.max - yRight.min));
        }
    }
    p.setPen(QPen(axisColor, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(pr);
}

void WaveformPane::drawTraces(QPainter &p, const QRectF &pr)
{
    const bool thick = m_view->thickLines || AppSettings::instance().waveformThickLines();
    const bool nyq = m_view->isAc() && acMode == AcMode::Nyquist;
    AxisRange nyqX;
    if (nyq) {
        // Nyquist uses its own x range from the data (real part).
        nyqX = yLeft;
    }
    for (const WaveTrace &t : traces) {
        if (!t.error.isEmpty())
            continue;
        for (int r = 0; r < t.x.size(); ++r) {
            if (r < m_view->stepVisible.size() && !m_view->stepVisible[r])
                continue;
            QVector<double> xs, ys, y2;
            if (!displayValues(t, r, xs, ys, y2))
                continue;
            auto mapX = [&](double x) {
                if (nyq)
                    return pr.left() + (x - nyqX.min) / (nyqX.max - nyqX.min) * pr.width();
                return xToPixel(x);
            };
            // A single trace over several steps: each step gets its own palette color (as LTspice).
            QColor stepColor = t.color;
            if (traces.size() == 1 && t.x.size() > 1) {
                const QList<QColor> pal = AppSettings::instance().traceColors();
                if (!pal.isEmpty())
                    stepColor = pal[r % pal.size()];
            }
            auto drawSeries = [&](const QVector<double> &yv, bool right, Qt::PenStyle style) {
                QPen pen(stepColor, thick ? 2.2 : 1.2, style);
                p.setPen(pen);
                const int n = std::min(xs.size(), yv.size());
                if (n == 0)
                    return;
                QPainterPath path;
                bool started = false;
                // Decimate: one min/max pair per pixel column when dense.
                const bool dense = n > pr.width() * 3 && !nyq;
                if (dense) {
                    int col = INT_MIN;
                    double ymin = 0, ymax = 0, yFirst = 0, yLast = 0;
                    auto flush = [&](int c) {
                        if (c == INT_MIN)
                            return;
                        const double pxv = c;
                        const double a = yToPixel(yFirst, right), lo = yToPixel(ymin, right), hi = yToPixel(ymax, right),
                                     b = yToPixel(yLast, right);
                        if (!started) {
                            path.moveTo(pxv, a);
                            started = true;
                        } else {
                            path.lineTo(pxv, a);
                        }
                        path.lineTo(pxv, lo);
                        path.lineTo(pxv, hi);
                        path.lineTo(pxv, b);
                    };
                    for (int k = 0; k < n; ++k) {
                        const double px = mapX(xs[k]);
                        if (px < pr.left() - 2 || px > pr.right() + 2) {
                            if (px > pr.right() + 2 && col != INT_MIN) {
                                flush(col);
                                col = INT_MIN;
                                path.lineTo(px, yToPixel(yv[k], right));
                                break;
                            }
                            continue;
                        }
                        const int c = int(px);
                        const double v = yv[k];
                        if (!std::isfinite(v))
                            continue;
                        if (c != col) {
                            flush(col);
                            col = c;
                            ymin = ymax = yFirst = yLast = v;
                        } else {
                            ymin = std::min(ymin, v);
                            ymax = std::max(ymax, v);
                            yLast = v;
                        }
                    }
                    flush(col);
                } else {
                    for (int k = 0; k < n; ++k) {
                        if (!std::isfinite(yv[k])) {
                            started = false;
                            continue;
                        }
                        const QPointF pt(mapX(xs[k]), yToPixel(yv[k], right));
                        if (!started) {
                            path.moveTo(pt);
                            started = true;
                        } else {
                            path.lineTo(pt);
                        }
                    }
                }
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
                if (m_view->markPoints && !dense) {
                    p.setPen(QPen(t.color, 1));
                    for (int k = 0; k < n; ++k)
                        p.drawRect(QRectF(mapX(xs[k]) - 1.5, yToPixel(yv[k], right) - 1.5, 3, 3));
                }
            };
            const bool right = !m_view->isAc() && t.axis == 1;
            drawSeries(ys, right, Qt::SolidLine);
            if (!y2.isEmpty())
                drawSeries(y2, acMode != AcMode::Cartesian, Qt::DashLine);
        }
    }
}

void WaveformPane::drawCursors(QPainter &p, const QRectF &pr)
{
    const QColor cc = AppSettings::instance().color(QStringLiteral("wave.cursor"));
    for (int c = 0; c < 2; ++c) {
        const WaveformView::Cursor &cur = m_view->cursors[c];
        if (!cur.on || cur.pane != this)
            continue;
        const double px = xToPixel(cur.x);
        p.setPen(QPen(cc, 1, c == 0 ? Qt::DashLine : Qt::DashDotLine));
        p.drawLine(QPointF(px, pr.top()), QPointF(px, pr.bottom()));
        double re = 0, im = 0;
        if (traceValue(cur.trace, cur.run, cur.x, &re, &im)) {
            double y = re;
            const WaveTrace &t = traces[cur.trace];
            bool right = !m_view->isAc() && t.axis == 1;
            if (t.complex) {
                const double mag = std::hypot(re, im);
                y = acMode == AcMode::Bode ? 20 * std::log10(mag) : (acMode == AcMode::LinearMag ? mag : re);
            }
            const double py = yToPixel(y, right);
            p.drawLine(QPointF(pr.left(), py), QPointF(pr.right(), py));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(px - 5, py - 5, 10, 10));
            p.drawText(QPointF(px + 7, py - 7), QString::number(c + 1));
        }
    }
}

void WaveformPane::drawAnnotations(QPainter &p, const QRectF &pr)
{
    Q_UNUSED(pr);
    for (const WaveAnnotation &a : annotations) {
        const QPointF pa(xToPixel(a.a.x()), yToPixel(a.a.y()));
        const QPointF pb(xToPixel(a.b.x()), yToPixel(a.b.y()));
        p.setPen(QPen(a.color.isValid() ? a.color : AppSettings::instance().color(QStringLiteral("wave.text")), 1.2));
        p.setBrush(Qt::NoBrush);
        switch (a.kind) {
        case WaveAnnotation::Text:
            p.drawText(pa, a.text);
            break;
        case WaveAnnotation::Line:
            p.drawLine(pa, pb);
            break;
        case WaveAnnotation::Arrow: {
            p.drawLine(pa, pb);
            const QLineF l(pb, pa);
            QLineF h1 = l, h2 = l;
            h1.setLength(10);
            h2.setLength(10);
            h1.setAngle(l.angle() + 25);
            h2.setAngle(l.angle() - 25);
            p.drawLine(h1);
            p.drawLine(h2);
            break;
        }
        case WaveAnnotation::Box:
            p.drawRect(QRectF(pa, pb).normalized());
            break;
        case WaveAnnotation::Ellipse:
            p.drawEllipse(QRectF(pa, pb).normalized());
            break;
        }
    }
}

// ---------------------------------------------------------------- interaction

int WaveformPane::traceAtLabel(QPoint pos) const
{
    for (int i = 0; i < m_labelRects.size(); ++i)
        if (m_labelRects[i].contains(pos))
            return i;
    return -1;
}

void WaveformPane::mousePressEvent(QMouseEvent *e)
{
    emit activated(this);
    m_pressPos = e->pos();
    m_mousePos = e->pos();
    const QRectF pr = plotRect();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier) && pr.contains(e->pos())
                                            && traceAtLabel(e->pos()) < 0)) {
        m_panDrag = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (m_view->annotationMode()) {
        m_annotating = true;
        return;
    }
    const int label = traceAtLabel(e->pos());
    if (label >= 0) {
        if (e->modifiers() & Qt::ControlModifier) {
            integrate(label);
            return;
        }
        m_dragLabel = label;
        emit m_view->traceSelected(traces[label].expr);
        return;
    }
    // Grab a cursor near the mouse.
    for (int c = 0; c < 2; ++c) {
        const auto &cur = m_view->cursors[c];
        if (cur.on && cur.pane == this && std::fabs(xToPixel(cur.x) - e->pos().x()) < 5) {
            m_dragCursor = c;
            setCursor(Qt::SizeHorCursor);
            return;
        }
    }
    // Axis areas open the axis dialogs.
    if (e->pos().x() < pr.left() && e->pos().y() > pr.top()) {
        axisDialog(1);
        return;
    }
    if (e->pos().x() > pr.right() && hasRightAxis()) {
        axisDialog(2);
        return;
    }
    if (e->pos().y() > pr.bottom()) {
        axisDialog(0);
        return;
    }
    if (pr.contains(e->pos()))
        m_zoomDrag = true;
}

void WaveformPane::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint delta = e->pos() - m_mousePos;
    m_mousePos = e->pos();
    const QRectF pr = plotRect();
    if (m_panDrag) {
        AxisRange &xa = m_view->xAxis;
        if (xa.log && xa.min > 0) {
            const double f = std::pow(xa.max / xa.min, -double(delta.x()) / pr.width());
            xa.min *= f;
            xa.max *= f;
        } else {
            const double dx = -delta.x() * (xa.max - xa.min) / pr.width();
            xa.min += dx;
            xa.max += dx;
        }
        xa.autoRange = false;
        if (!(e->modifiers() & Qt::ShiftModifier)) {
            for (AxisRange *ya : {&yLeft, &yRight}) {
                if (ya->log)
                    continue;
                const double dy = delta.y() * (ya->max - ya->min) / pr.height();
                ya->min += dy;
                ya->max += dy;
                ya->autoRange = false;
            }
        }
        for (WaveformPane *pn : m_view->panes())
            pn->update();
        return;
    }
    if (m_dragCursor >= 0) {
        m_view->cursors[m_dragCursor].x = std::clamp(pixelToX(e->pos().x()), m_view->xAxis.min, m_view->xAxis.max);
        m_view->updateReadout();
        update();
        return;
    }
    if (m_dragLabel >= 0 && (e->pos() - m_pressPos).manhattanLength() > 10) {
        // Drag the trace label to another pane.
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData;
        mime->setData(QStringLiteral("application/x-linspice-trace"), traces[m_dragLabel].expr.toUtf8());
        drag->setMimeData(mime);
        const int idx = m_dragLabel;
        m_dragLabel = -1;
        const Qt::DropAction act = drag->exec(Qt::MoveAction | Qt::CopyAction);
        Q_UNUSED(act);
        // Drop handling: find pane under cursor.
        QWidget *w = QApplication::widgetAt(QCursor::pos());
        while (w && !qobject_cast<WaveformPane *>(w))
            w = w->parentWidget();
        auto *target = qobject_cast<WaveformPane *>(w);
        if (target && target != this && target->m_view == m_view && idx < traces.size()) {
            const QString expr = traces[idx].expr;
            m_view->addTrace(expr, target);
            if (!(QApplication::keyboardModifiers() & Qt::ControlModifier))
                m_view->removeTrace(this, idx);
        }
        return;
    }
    if (m_zoomDrag || m_annotating) {
        update();
    }
    // Status readout
    if (pr.contains(e->pos())) {
        const double x = pixelToX(e->pos().x());
        const double y = pixelToY(e->pos().y());
        QString msg = QStringLiteral("x = %1   y = %2").arg(units::formatEng(x, m_view->xUnit(), 5), axisLabel(y, leftUnit(), 0));
        if (hasRightAxis())
            msg += QStringLiteral("   y2 = %1").arg(axisLabel(pixelToY(e->pos().y(), true), rightUnit(), 0));
        if (m_zoomDrag) {
            const double x0 = pixelToX(m_pressPos.x());
            msg += QStringLiteral("   Δx = %1").arg(units::formatDelta(x - x0, m_view->xUnit()));
            msg += QStringLiteral("   Δy = %1").arg(axisLabel(y - pixelToY(m_pressPos.y()), leftUnit(), 0));
        }
        emit m_view->statusMessage(msg);
        bool nearCursor = false;
        for (int c = 0; c < 2; ++c)
            if (m_view->cursors[c].on && m_view->cursors[c].pane == this && std::fabs(xToPixel(m_view->cursors[c].x) - e->pos().x()) < 5)
                nearCursor = true;
        setCursor(nearCursor ? Qt::SizeHorCursor : Qt::CrossCursor);
    } else if (traceAtLabel(e->pos()) >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else if (e->pos().x() < pr.left() || e->pos().y() > pr.bottom() || e->pos().x() > pr.right()) {
        // Ruler cursor over the axes (click to edit the axis).
        setCursor(Qt::SplitHCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void WaveformPane::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_panDrag) {
        m_panDrag = false;
        setCursor(Qt::CrossCursor);
        return;
    }
    if (m_dragCursor >= 0) {
        m_dragCursor = -1;
        return;
    }
    if (m_dragLabel >= 0) {
        // Click on a label: attach cursor 1 (or 2 with Shift, both with Alt).
        const int idx = m_dragLabel;
        m_dragLabel = -1;
        if (e->modifiers() & Qt::AltModifier) {
            m_view->attachCursor(0, this, idx);
            m_view->attachCursor(1, this, idx);
        } else if (e->modifiers() & Qt::ShiftModifier) {
            m_view->attachCursor(1, this, idx);
        } else {
            m_view->attachCursor(0, this, idx);
        }
        return;
    }
    if (m_annotating) {
        m_annotating = false;
        const auto kind = *m_view->annotationMode();
        WaveAnnotation a;
        a.kind = kind;
        a.a = QPointF(pixelToX(m_pressPos.x()), pixelToY(m_pressPos.y()));
        a.b = QPointF(pixelToX(e->pos().x()), pixelToY(e->pos().y()));
        if (kind == WaveAnnotation::Text) {
            bool ok = false;
            a.text = QInputDialog::getText(this, tr("Place Text"), tr("Text:"), QLineEdit::Normal, QString(), &ok);
            if (!ok || a.text.isEmpty())
                return;
        }
        annotations << a;
        m_view->clearAnnotationMode();
        update();
        return;
    }
    if (m_zoomDrag) {
        m_zoomDrag = false;
        const QRect r = QRect(m_pressPos, e->pos()).normalized();
        if (r.width() > 4 && r.height() > 4) {
            m_view->pushZoom();
            const bool nyq = m_view->isAc() && acMode == AcMode::Nyquist;
            if (!nyq) {
                const double x0 = pixelToX(r.left()), x1 = pixelToX(r.right());
                m_view->xAxis.min = x0;
                m_view->xAxis.max = x1;
                m_view->xAxis.autoRange = false;
            }
            const double yl0 = pixelToY(r.bottom()), yl1 = pixelToY(r.top());
            const double yr0 = pixelToY(r.bottom(), true), yr1 = pixelToY(r.top(), true);
            yLeft.min = yl0;
            yLeft.max = yl1;
            yLeft.autoRange = false;
            yRight.min = yr0;
            yRight.max = yr1;
            yRight.autoRange = false;
            for (WaveformPane *pn : m_view->panes()) {
                if (pn != this && m_view->syncHorizontal)
                    pn->autorangeY();
                pn->update();
            }
        } else if (r.width() > 4) {
            // Horizontal-only drag zooms X.
            m_view->pushZoom();
            m_view->xAxis.min = pixelToX(r.left());
            m_view->xAxis.max = pixelToX(r.right());
            m_view->xAxis.autoRange = false;
            for (WaveformPane *pn : m_view->panes())
                pn->autorangeY();
        }
        update();
    }
}

void WaveformPane::mouseDoubleClickEvent(QMouseEvent *e)
{
    const int label = traceAtLabel(e->pos());
    if (label >= 0) {
        bool ok = false;
        const QString expr = QInputDialog::getText(this, tr("Expression Editor"), tr("Enter an algebraic expression to plot:"),
                                                   QLineEdit::Normal, traces[label].expr, &ok);
        if (ok && !expr.trimmed().isEmpty()) {
            traces[label].expr = expr.trimmed();
            recompute();
            autorangeY();
        }
        return;
    }
    m_view->zoomFit();
}

void WaveformPane::wheelEvent(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    const double f = std::pow(0.8, steps);
    const QPointF pos = e->position();
    m_view->pushZoom();
    if (e->modifiers() & Qt::ControlModifier) {
        for (AxisRange *ya : {&yLeft, &yRight}) {
            const double c = pixelToY(pos.y(), ya == &yRight);
            if (ya->log && ya->min > 0) {
                ya->min = std::pow(10, std::log10(c) - (std::log10(c) - std::log10(ya->min)) * f);
                ya->max = std::pow(10, std::log10(c) + (std::log10(ya->max) - std::log10(c)) * f);
            } else {
                ya->min = c - (c - ya->min) * f;
                ya->max = c + (ya->max - c) * f;
            }
            ya->autoRange = false;
        }
        update();
        return;
    }
    AxisRange &xa = m_view->xAxis;
    if (e->modifiers() & Qt::ShiftModifier) {
        const double span = xa.max - xa.min;
        xa.min -= steps * span * 0.1;
        xa.max -= steps * span * 0.1;
    } else {
        const double c = pixelToX(pos.x());
        if (xa.log && xa.min > 0) {
            xa.min = std::pow(10, std::log10(c) - (std::log10(c) - std::log10(xa.min)) * f);
            xa.max = std::pow(10, std::log10(c) + (std::log10(xa.max) - std::log10(c)) * f);
        } else {
            xa.min = c - (c - xa.min) * f;
            xa.max = c + (xa.max - c) * f;
        }
    }
    xa.autoRange = false;
    for (WaveformPane *pn : m_view->panes()) {
        if (pn->yLeft.autoRange || pn->yRight.autoRange)
            pn->autorangeY();
        pn->update();
    }
}

void WaveformPane::leaveEvent(QEvent *)
{
    emit m_view->statusMessage(QString());
}

void WaveformPane::contextMenuEvent(QContextMenuEvent *e)
{
    emit activated(this);
    const int label = traceAtLabel(e->pos());
    if (label >= 0) {
        traceMenu(label, e->globalPos());
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Add Traces..."), m_view, [this]() {
        emit activated(this);
        emit m_view->addTracesRequested();
    });
    menu.addAction(tr("Autorange Y-axis"), this, [this]() {
        yLeft.autoRange = yRight.autoRange = true;
        autorangeY();
    });
    menu.addAction(tr("Zoom to Fit"), m_view, &WaveformView::zoomFit);
    menu.addAction(tr("Zoom Back"), m_view, &WaveformView::zoomBack);
    menu.addSeparator();
    menu.addAction(tr("Add Plot Pane Above"), m_view, [this]() { m_view->addPane(true); });
    menu.addAction(tr("Add Plot Pane Below"), m_view, [this]() { m_view->addPane(false); });
    QAction *del = menu.addAction(tr("Delete Active Pane"), m_view, [this]() { m_view->deletePane(this); });
    del->setEnabled(m_view->panes().size() > 1);
    menu.addSeparator();
    QAction *grid = menu.addAction(tr("Grid"));
    grid->setCheckable(true);
    grid->setChecked(m_view->gridOn);
    connect(grid, &QAction::toggled, m_view, [this](bool on) {
        m_view->gridOn = on;
        for (WaveformPane *p : m_view->panes())
            p->update();
    });
    if (m_view->isAc()) {
        QMenu *mode = menu.addMenu(tr("Representation"));
        const QList<QPair<QString, AcMode>> modes = {{tr("Bode"), AcMode::Bode}, {tr("Linear magnitude"), AcMode::LinearMag},
                                                     {tr("Nyquist"), AcMode::Nyquist}, {tr("Cartesian"), AcMode::Cartesian}};
        for (const auto &m : modes) {
            QAction *a = mode->addAction(m.first);
            a->setCheckable(true);
            a->setChecked(acMode == m.second);
            connect(a, &QAction::triggered, this, [this, m]() {
                acMode = m.second;
                yLeft.autoRange = yRight.autoRange = true;
                autorangeY();
            });
        }
        QAction *ph = menu.addAction(tr("Show Phase"));
        ph->setCheckable(true);
        ph->setChecked(showPhase && !groupDelay);
        connect(ph, &QAction::toggled, this, [this](bool on) {
            showPhase = on;
            groupDelay = false;
            yRight.autoRange = true;
            autorangeY();
        });
        QAction *gd = menu.addAction(tr("Show Group Delay"));
        gd->setCheckable(true);
        gd->setChecked(groupDelay);
        connect(gd, &QAction::toggled, this, [this](bool on) {
            groupDelay = on;
            yRight.autoRange = true;
            autorangeY();
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Select Steps..."), m_view, &WaveformView::showStepSelector)->setEnabled(m_view->dataset() && m_view->dataset()->runs.size() > 1);
    menu.addAction(tr("FFT..."), m_view, &WaveformView::openFftDialog);
    menu.addAction(tr("Clear Cursors"), m_view, &WaveformView::clearCursors);
    QMenu *notes = menu.addMenu(tr("Notes && Annotations"));
    notes->addAction(tr("Label Cursor Position"), m_view, &WaveformView::labelCursorPosition);
    notes->addAction(tr("Place Text"), this, [this]() { m_view->setAnnotationMode(WaveAnnotation::Text); });
    notes->addAction(tr("Draw Arrow"), this, [this]() { m_view->setAnnotationMode(WaveAnnotation::Arrow); });
    notes->addAction(tr("Draw Line"), this, [this]() { m_view->setAnnotationMode(WaveAnnotation::Line); });
    notes->addAction(tr("Draw Box"), this, [this]() { m_view->setAnnotationMode(WaveAnnotation::Box); });
    notes->addAction(tr("Draw Ellipse"), this, [this]() { m_view->setAnnotationMode(WaveAnnotation::Ellipse); });
    notes->addAction(tr("Delete All Annotations"), this, [this]() {
        annotations.clear();
        update();
    });
    menu.addAction(tr("Copy Image to Clipboard"), m_view, &WaveformView::copyImageToClipboard);
    menu.exec(e->globalPos());
}

void WaveformPane::traceMenu(int index, QPoint globalPos)
{
    QMenu menu(this);
    menu.addSection(traces[index].expr);
    menu.addAction(tr("Attach Cursor 1"), this, [this, index]() { m_view->attachCursor(0, this, index); });
    menu.addAction(tr("Attach Cursor 2"), this, [this, index]() { m_view->attachCursor(1, this, index); });
    menu.addAction(tr("Attach Cursors 1 && 2"), this, [this, index]() {
        m_view->attachCursor(0, this, index);
        m_view->attachCursor(1, this, index);
    });
    menu.addSeparator();
    menu.addAction(tr("Edit Expression..."), this, [this, index]() {
        bool ok = false;
        const QString expr = QInputDialog::getText(this, tr("Expression Editor"), tr("Enter an algebraic expression to plot:"),
                                                   QLineEdit::Normal, traces[index].expr, &ok);
        if (ok && !expr.trimmed().isEmpty()) {
            traces[index].expr = expr.trimmed();
            recompute();
            autorangeY();
        }
    });
    menu.addAction(tr("Color..."), this, [this, index]() {
        const QColor c = QColorDialog::getColor(traces[index].color, this, tr("Trace color"));
        if (c.isValid()) {
            traces[index].color = c;
            traces[index].autoColor = false;
            update();
        }
    });
    menu.addAction(tr("Average / RMS (visible range)"), this, [this, index]() { integrate(index); });
    menu.addSeparator();
    menu.addAction(tr("Delete Trace"), this, [this, index]() { m_view->removeTrace(this, index); });
    menu.exec(globalPos);
}

void WaveformPane::integrate(int index)
{
    const WaveTrace &t = traces[index];
    if (t.complex || t.x.isEmpty()) {
        QMessageBox::information(this, tr("Integrate"), tr("Average/RMS is available for real (time-domain) data only."));
        return;
    }
    const double x0 = m_view->xAxis.min, x1 = m_view->xAxis.max;
    QStringList lines;
    for (int r = 0; r < t.x.size(); ++r) {
        if (r < m_view->stepVisible.size() && !m_view->stepVisible[r])
            continue;
        const QVector<double> &xs = t.x[r];
        const QVector<double> &ys = t.re[r];
        double integ = 0, integSq = 0;
        for (int k = 1; k < xs.size(); ++k) {
            const double a = std::max(xs[k - 1], x0), b = std::min(xs[k], x1);
            if (b <= a)
                continue;
            auto at = [&](double x) {
                const double f = (x - xs[k - 1]) / (xs[k] - xs[k - 1]);
                return ys[k - 1] + f * (ys[k] - ys[k - 1]);
            };
            const double ya = at(a), yb = at(b);
            integ += 0.5 * (ya + yb) * (b - a);
            integSq += (b - a) * (ya * ya + ya * yb + yb * yb) / 3.0;
        }
        const double span = x1 - x0;
        QString line;
        if (t.x.size() > 1)
            line = tr("Step %1: ").arg(r + 1);
        line += tr("Interval: %1 to %2\nAverage: %3\nRMS: %4\nIntegral: %5")
                    .arg(units::formatEng(x0, m_view->xUnit()), units::formatEng(x1, m_view->xUnit()),
                         units::formatEng(integ / span, t.unitText), units::formatEng(std::sqrt(std::max(0.0, integSq / span)), t.unitText),
                         units::formatEng(integ, t.unitText + QStringLiteral("·") + m_view->xUnit()));
        if (t.unitText == QLatin1String("W"))
            line += tr("\nEnergy: %1").arg(units::formatEng(integ, QStringLiteral("J")));
        lines << line;
    }
    QMessageBox::information(this, t.expr, lines.join(QStringLiteral("\n\n")));
}

void WaveformPane::axisDialog(int which)
{
    AxisDialog dlg(this);
    const QString unit = which == 0 ? m_view->xUnit() : (which == 1 ? leftUnit() : rightUnit());
    AxisRange &ax = which == 0 ? m_view->xAxis : (which == 1 ? yLeft : yRight);
    dlg.setRange(ax.min, ax.max, ax.log, unit, which == 0 ? m_view->xLabel() : (which == 1 ? tr("Left Vertical Axis") : tr("Right Vertical Axis")));
    if (which == 0)
        dlg.setExpression(m_view->xExpression);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_view->pushZoom();
    ax.min = dlg.minimum();
    ax.max = dlg.maximum();
    ax.log = dlg.isLog();
    ax.autoRange = false;
    if (which == 0 && dlg.expression() != m_view->xExpression) {
        m_view->xExpression = dlg.expression();
        for (WaveformPane *p : m_view->panes())
            p->recompute();
        if (dlg.autoRequested())
            m_view->autorangeX();
    }
    if (dlg.autoRequested()) {
        ax.autoRange = true;
        if (which == 0)
            m_view->autorangeX();
        else
            autorangeY();
    }
    for (WaveformPane *p : m_view->panes())
        p->update();
}
