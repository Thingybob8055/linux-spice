#pragma once

#include "Document.h"
#include "SchematicPainter.h"
#include "core/Netlister.h"
#include "core/Schematic.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>

class QMenu;

class SchematicView : public DocumentWidget
{
    Q_OBJECT
public:
    enum class Mode {
        Normal,
        Wire,
        Place,       // placing symbols (component, R, C, ...)
        PlaceFlag,   // net label / ground
        PlaceText,   // text or directive following the cursor
        Move,
        Drag,        // move with connected wires stretching
        Delete,
        Duplicate,
        DrawLine,
        DrawRect,
        DrawCircle,
        DrawArc,
        ZoomArea,
        PlaceDataFlag,
    };

    explicit SchematicView(QWidget *parent = nullptr);

    Kind kind() const override { return Kind::Schematic; }
    QString filePath() const override { return m_sch.filePath; }
    bool isModified() const override { return m_modified; }
    bool save() override;
    bool saveAs(const QString &path) override;
    QString fileFilter() const override { return tr("Schematics (*.asc)"); }
    void zoomIn() override;
    void zoomOut() override;
    void zoomFit() override;
    void zoomBack() override;
    void undo() override;
    void redo() override;
    bool canUndo() const override { return !m_undo.isEmpty() || m_moving; }
    bool canRedo() const override { return !m_redo.isEmpty(); }
    void print(QPrinter *printer) override;
    void copyImageToClipboard() override;
    bool exportImage(const QString &path) override;

    bool load(const QString &path, QString *error = nullptr);
    void setSchematic(const Schematic &sch);
    const Schematic &schematic() const { return m_sch; }
    Schematic &schematicRef() { return m_sch; }
    // Records an undo point and marks modified after external edits.
    void commitExternalChange();
    NetlistResult netlist() const;
    const Connectivity &connectivity() const;

    Mode mode() const { return m_mode; }
    void setMode(Mode m);
    void startPlaceSymbol(const QString &symbolName, const AttrList &presetAttrs = {});
    void startPlaceFlag(const QString &name, const QString &ioDirection = QString());
    void startPlaceText(const QString &text, bool directive, int fontSize = 2, const Justify &just = {});
    void rotateSelection();
    void mirrorSelection();
    void deleteSelection();
    void copySelection();
    void cutSelection();
    void paste();
    void selectAll();
    void clearSelection();
    bool hasSelection() const { return !m_selection.isEmpty(); }

    // Editing helpers (open the appropriate dialog).
    void editSymbol(int index, bool superExpert = false);
    void editText(int index);
    void editFlag(int index);
    void editShapeStyle(int index);

    // Simulation linkage.
    void setSimulationAvailable(bool available, bool hasTransient);
    void setOperatingPoint(const QHash<QString, double> &op);
    // Trace names of pins the simulation measured ("u1:3" -> "Ix(u1:3)"), from SimResult.
    void setPinProbes(const QHash<QString, QString> &probes);
    // The analysis of the last run and the names of its signals: in a noise analysis a click
    // on a device plots its noise contribution, V(R1), as in LTspice.
    void setProbeContext(const QString &analysis, const QSet<QString> &signalNames);
    // The current probe of every symbol pin, as (description, expression); for testing.
    QList<QPair<QString, QString>> allProbeExpressions() const;
    static QString shownName(const QString &spiceName);
    void setHighlightNets(const QSet<QString> &nets);
    void setInstancePath(const QString &path) { m_instancePath = path; } // e.g. "xu1" for a sub-sheet
    QString instancePath() const { return m_instancePath; }
    void showOperatingPointLabels(bool on);

    // Search for a component or net by name; selects and centers it.
    bool findAndSelect(const QString &name);

    void setGridVisible(bool on);
    bool gridVisible() const { return m_style.showGrid; }
    void setMarkUnconnected(bool on);
    void setShowAnchors(bool on);
    void reloadStyle();

    // Generates the pending text of a new schematic (empty sheet).
    static Schematic emptySchematic();

signals:
    void probe(const QString &expression, bool append);
    void probeHover(const QString &description);
    void openSubSheet(const QString &ascPath, const QString &instancePath);
    void openSymbolFile(const QString &asyPath);
    void requestComponentDialog();
    void requestAnalysisDialog(int textIndex);
    void requestRun();
    void modeChanged(Mode mode);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    // Coordinate helpers
    QPointF toWorld(QPointF widgetPos) const;
    QPointF toWidget(QPointF worldPos) const;
    QPoint snap(QPointF world) const;
    QTransform worldTransform() const;
    double hitTolerance() const { return 6.0 / m_scale; }

    // Hit testing
    SchItem itemAt(QPointF world, bool includeAttrs = true) const;
    QList<SchItem> itemsIn(const QRectF &worldRect) const;
    QRectF itemRect(const SchItem &it) const;
    int wireAt(QPointF world) const;
    bool isConnectionPoint(QPoint p, QPoint exclude = QPoint(INT_MIN, INT_MIN)) const;
    bool isUnconnectedPin(QPoint p) const;

    // Undo
    void pushUndo();
    void setModified(bool m);
    void invalidateConnectivity();
    void afterEdit(); // normalize wires, invalidate caches, repaint

    // Editing operations
    void commitWireSegment(QPoint from, QPoint to);
    void cutWiresThroughComponents();
    void placeCurrentSymbol(QPoint at);
    void beginMove(const QSet<SchItem> &items, QPointF worldStart, bool stretch);
    void updateMove(QPointF world);
    void finishMove();
    void cancelMove();
    QSet<SchItem> expandWithAttrs(const QSet<SchItem> &items) const;
    void applyRotate(const QSet<SchItem> &items, bool mirror);
    QString serializeItems(const QSet<SchItem> &items) const;
    void pasteText(const QString &text, QPoint at, bool follow);
    void deleteItems(QSet<SchItem> items);
    void editAttributeText(int symbolIndex, int windowId);
    void showContextMenu(QPoint globalPos, QPointF world);

    // Probing
    QString probeExpressionAt(QPointF world, bool alt, QString *description) const;
    QString nodeExpr(const QString &net) const;
    QString noiseProbe(const QString &shown, QString *description) const;
    QString elementExpr(const QString &fn, const QString &elem) const;

    void updateCursorShape(QPointF world, Qt::KeyboardModifiers mods);
    void ensureVisibleWorld(QPointF world);

    Schematic m_sch;
    bool m_modified = false;
    QStringList m_undo, m_redo;
    SchematicStyle m_style;
    double m_scale = 1.5;
    QPointF m_origin; // world coordinate at widget (0,0)
    QList<QPair<double, QPointF>> m_zoomHistory;

    Mode m_mode = Mode::Normal;
    QSet<SchItem> m_selection;

    // Placement
    SymbolInstance m_placeSym;
    QList<SymbolInstance> m_placeHistory;
    NetFlag m_placeFlag;
    SchText m_placeText;
    QString m_floatingPaste; // serialized items following the cursor (duplicate/paste)
    Schematic m_floatingSch;
    QPoint m_floatingAnchor;

    // Wiring
    bool m_wireActive = false;
    QPoint m_wireStart;
    bool m_wireHorizontalFirst = true;
    bool m_wireDirectionDecided = false;

    // Drawing shapes
    bool m_shapeActive = false;
    int m_shapeStage = 0;
    Shape m_shape;

    // Move/drag
    bool m_moving = false;
    bool m_moveStretch = false;
    QSet<SchItem> m_moveItems;
    QPoint m_moveStart;
    QPoint m_moveDelta;
    Schematic m_moveBackup;
    QList<QPair<int, int>> m_stretchEnds; // (wire index, 0=a / 1=b)

    // Mouse state
    bool m_leftDown = false;
    bool m_panning = false;
    bool m_rubber = false;
    QPointF m_pressWidget;
    QPointF m_lastWidget;
    QPointF m_mouseWorld;
    SchItem m_pressItem;
    bool m_pressStartedProbe = false;
    QString m_probeStartNet;

    // Simulation linkage
    bool m_simAvailable = false;
    bool m_simTransient = false;
    QHash<QString, double> m_op;
    QHash<QString, QString> m_pinProbes;
    QString m_probeAnalysis;
    QSet<QString> m_probeSignals; // lower case
    bool m_showOpLabels = false;
    QSet<QString> m_highlightNets;
    QString m_instancePath;

    mutable bool m_connValid = false;
    mutable Connectivity m_conn;
};
