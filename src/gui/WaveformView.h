#pragma once

#include "Document.h"
#include "core/Expression.h"
#include "core/RawFile.h"

#include <QColor>
#include <QList>
#include <QPointer>
#include <QWidget>

class QLabel;
class QSplitter;
class WaveformView;

struct WaveTrace {
    QString expr;
    QColor color;
    bool autoColor = true;
    // Computed data per run (x values are the dataset's x axis or a parametric x expression).
    QVector<QVector<double>> x;
    QVector<QVector<double>> re;
    QVector<QVector<double>> im;
    bool complex = false;
    Unit unit;
    QString unitText;
    QString error;
    int axis = 0; // 0 left, 1 right (real data with a second unit)
};

struct AxisRange {
    double min = 0, max = 1;
    bool log = false;
    bool autoRange = true;
};

struct WaveAnnotation {
    enum Kind { Text, Line, Arrow, Box, Ellipse };
    Kind kind = Text;
    QPointF a, b; // data coordinates (x, left-axis y)
    QString text;
    QColor color;
};

// One plot pane: a set of traces sharing vertical axes.
class WaveformPane : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformPane(WaveformView *view);

    QList<WaveTrace> traces;
    AxisRange yLeft, yRight;
    QList<WaveAnnotation> annotations;
    enum class AcMode { Bode, LinearMag, Nyquist, Cartesian };
    AcMode acMode = AcMode::Bode;
    bool showPhase = true;
    bool groupDelay = false;

    void recompute();
    void autorangeY();
    QRectF plotRect() const;
    bool hasRightAxis() const;
    int traceAtLabel(QPoint pos) const;
    QString rightUnit() const;
    QString leftUnit() const;
    // Value of a trace at x for run (interpolated). For AC returns complex.
    bool traceValue(int traceIndex, int run, double x, double *re, double *im) const;

    double xToPixel(double x) const;
    double pixelToX(double px) const;
    double yToPixel(double y, bool right = false) const;
    double pixelToY(double py, bool right = false) const;

signals:
    void activated(WaveformPane *pane);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    friend class WaveformView;
    void drawAxes(QPainter &p, const QRectF &pr);
    void drawTraces(QPainter &p, const QRectF &pr);
    void drawLegend(QPainter &p);
    void drawCursors(QPainter &p, const QRectF &pr);
    void drawAnnotations(QPainter &p, const QRectF &pr);
    void traceMenu(int index, QPoint globalPos);
    void integrate(int index);
    void axisDialog(int which); // 0 x, 1 left, 2 right
    bool displayValues(const WaveTrace &t, int run, QVector<double> &x, QVector<double> &yMain, QVector<double> &ySecond) const;

    WaveformView *m_view;
    QList<QRect> m_labelRects;
    bool m_zoomDrag = false;
    bool m_panDrag = false;
    int m_dragCursor = -1;
    int m_dragLabel = -1;
    QPoint m_pressPos;
    QPoint m_mousePos;
    bool m_annotating = false;
};

class WaveformView : public DocumentWidget
{
    Q_OBJECT
public:
    explicit WaveformView(QWidget *parent = nullptr);
    ~WaveformView() override;

    Kind kind() const override { return Kind::Waveform; }
    QString filePath() const override { return m_rawPath; }
    QString displayName() const override;
    void zoomIn() override;
    void zoomOut() override;
    void zoomFit() override;
    void zoomBack() override;
    void undo() override { zoomBack(); }
    void print(QPrinter *printer) override;
    void copyImageToClipboard() override;
    bool exportImage(const QString &path) override;

    void setDataset(DatasetPtr ds, const QString &rawPath = QString(), const QString &title = QString());
    DatasetPtr dataset() const { return m_data; }
    // Called when a simulation is re-run: keep traces and recompute.
    void updateDataset(DatasetPtr ds);
    void setSourceSchematic(const QString &ascPath) { m_ascPath = ascPath; }
    QString sourceSchematic() const { return m_ascPath; }

    void addTrace(const QString &expr, WaveformPane *pane = nullptr);
    void addTraces(const QStringList &exprs);
    void removeTrace(WaveformPane *pane, int index);
    void clearTraces();
    WaveformPane *activePane() const { return m_active; }
    QList<WaveformPane *> panes() const { return m_panes; }
    WaveformPane *addPane(bool above);
    void deletePane(WaveformPane *pane);
    void movePane(WaveformPane *pane, int delta);
    void setActivePane(WaveformPane *pane);

    AxisRange xAxis;
    QString xExpression; // parametric x (empty = dataset sweep variable)
    bool gridOn = true;
    bool markPoints = false;
    bool thickLines = false;
    bool syncHorizontal = true;
    QVector<bool> stepVisible;

    void autorangeX();
    void pushZoom();
    QString xUnit() const;
    QString xLabel() const;
    bool isAc() const;

    // Cursors
    struct Cursor {
        bool on = false;
        WaveformPane *pane = nullptr;
        int trace = -1;
        int run = 0;
        double x = 0;
    };
    Cursor cursors[2];
    void attachCursor(int which, WaveformPane *pane, int trace);
    void clearCursors();
    void moveCursorToPoint(int which, int direction, int stepCount = 1);
    void updateReadout();

    // Plot settings (.plt)
    bool savePlotSettings(const QString &path);
    bool loadPlotSettings(const QString &path);
    QString defaultPlotSettingsPath() const;

    // Data export (tab separated text)
    bool exportData(const QString &path, const QStringList &exprs);
    QStringList allTraceExpressions() const;
    QStringList availableVectors() const;

    void showStepSelector();
    void openFftDialog();
    void executeMeasScript(const QString &path, QString *log);
    void setAnnotationMode(WaveAnnotation::Kind kind);
    std::optional<WaveAnnotation::Kind> annotationMode() const { return m_annotationMode; }
    void clearAnnotationMode() { m_annotationMode.reset(); }
    void labelCursorPosition();
    void colorFor(WaveTrace &t, int index) const;

signals:
    void traceSelected(const QString &expr);
    void requestNewWaveform(DatasetPtr data, const QStringList &traces, const QString &title);
    void logMessage(const QString &text);
    void addTracesRequested();

protected:
    void keyPressEvent(QKeyEvent *) override;

private:
    void recomputeAll();
    void ensureStepVisibility();

    DatasetPtr m_data;
    QString m_rawPath;
    QString m_ascPath;
    QString m_title;
    QSplitter *m_split = nullptr;
    QList<WaveformPane *> m_panes;
    WaveformPane *m_active = nullptr;
    QList<QList<double>> m_zoomStack; // x min/max + per pane y ranges
    QPointer<QWidget> m_readout;
    QLabel *m_readoutLabel = nullptr;
    std::optional<WaveAnnotation::Kind> m_annotationMode;
};
