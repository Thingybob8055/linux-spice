#include "MainWindow.h"

#include "Icons.h"
#include "NetlistEditor.h"
#include "SchematicView.h"
#include "SymbolEditor.h"
#include "WaveformView.h"
#include "core/AppSettings.h"
#include "core/ModelLibrary.h"
#include "core/SymbolLibrary.h"
#include "core/TextIO.h"
#include "core/Units.h"
#include "dialogs/AddTraceDialog.h"
#include "dialogs/AnalysisDialog.h"
#include "dialogs/ComponentBrowser.h"
#include "dialogs/MiscDialogs.h"
#include "dialogs/NetLabelDialog.h"
#include "dialogs/PassiveDialog.h"
#include "dialogs/SourceDialog.h"
#include "dialogs/AttributeDialog.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/TextDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("LinSpice"));
    setWindowIcon(icons::appIcon());
    setAcceptDrops(true);
    m_mdi = new QMdiArea;
    m_mdi->setViewMode(AppSettings::instance().raw().value(QStringLiteral("ui/tabs"), true).toBool() ? QMdiArea::TabbedView
                                                                                                      : QMdiArea::SubWindowView);
    m_mdi->setTabsClosable(true);
    m_mdi->setTabsMovable(true);
    m_mdi->setDocumentMode(true);
    setCentralWidget(m_mdi);
    connect(m_mdi, &QMdiArea::subWindowActivated, this, [this]() {
        if (m_mdi)
            updateActions();
    });

    m_statusLabel = new QLabel;
    m_simLabel = new QLabel;
    m_progress = new QProgressBar;
    m_progress->setMaximumWidth(180);
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_simLabel);
    statusBar()->addPermanentWidget(m_progress);

    createActions();
    createMenus();
    createToolBars();
    updateActions();

    const QByteArray geo = AppSettings::instance().raw().value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
    else
        resize(1280, 860);
    restoreState(AppSettings::instance().raw().value(QStringLiteral("ui/state")).toByteArray());
}

MainWindow::~MainWindow()
{
    // Tear down documents while members used by their signal handlers still exist.
    for (QMdiSubWindow *w : m_mdi->subWindowList())
        w->removeEventFilter(this);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
        if (it->log && !it->log->parent())
            delete it->log.data();
    delete m_mdi;
    m_mdi = nullptr;
}

// ---------------------------------------------------------------- actions

QAction *MainWindow::act(const QString &id, const QString &text, const QKeySequence &key, const std::function<void()> &fn,
                         const QString &icon)
{
    auto *a = new QAction(text, this);
    if (!key.isEmpty())
        a->setShortcut(key);
    if (!icon.isEmpty())
        a->setIcon(icons::get(icon));
    connect(a, &QAction::triggered, this, [fn]() { fn(); });
    addAction(a);
    m_actions.insert(id, a);
    return a;
}

void MainWindow::createActions()
{
    // File
    act(QStringLiteral("new"), tr("&New Schematic"), QKeySequence(Qt::CTRL | Qt::Key_N), [this]() { newSchematic(); }, QStringLiteral("new"));
    act(QStringLiteral("newSymbol"), tr("New S&ymbol"), {}, [this]() { newSymbol(); });
    act(QStringLiteral("open"), tr("&Open..."), QKeySequence(Qt::CTRL | Qt::Key_O), [this]() { openDialog(); }, QStringLiteral("open"));
    act(QStringLiteral("examples"), tr("Open &Examples..."), {}, [this]() { openExamples(); });
    act(QStringLiteral("save"), tr("&Save"), QKeySequence(Qt::CTRL | Qt::Key_S), [this]() { saveDoc(activeDoc(), false); }, QStringLiteral("save"));
    act(QStringLiteral("saveAs"), tr("Save &As..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this]() { saveDoc(activeDoc(), true); });
    act(QStringLiteral("close"), tr("&Close"), QKeySequence(Qt::CTRL | Qt::Key_W), [this]() { closeActive(); }, QStringLiteral("close"));
    act(QStringLiteral("print"), tr("&Print..."), QKeySequence(Qt::CTRL | Qt::Key_P), [this]() { printDoc(false); }, QStringLiteral("print"));
    act(QStringLiteral("printPreview"), tr("Print Pre&view..."), {}, [this]() { printDoc(true); });
    QAction *mono = act(QStringLiteral("printMono"), tr("Print &Monochrome"), {}, []() {});
    mono->setCheckable(true);
    mono->setChecked(AppSettings::instance().raw().value(QStringLiteral("print/monochrome"), true).toBool());
    connect(mono, &QAction::toggled, this, [](bool on) { AppSettings::instance().raw().setValue(QStringLiteral("print/monochrome"), on); });
    act(QStringLiteral("exportImage"), tr("Export &Image (PNG/SVG)..."), {}, [this]() { exportImage(); });
    act(QStringLiteral("execMeas"), tr("Execute .MEAS Script..."), {}, [this]() {
        WaveformView *w = activeWaveform();
        if (!w)
            return;
        const QString f = QFileDialog::getOpenFileName(this, tr("Execute .MEAS script"), QFileInfo(w->sourceSchematic()).absolutePath(),
                                                       tr("Netlists and scripts (*.net *.cir *.sp *.txt *.meas);;All files (*)"));
        if (f.isEmpty())
            return;
        QString log;
        w->executeMeasScript(f, &log);
        showLog();
        if (m_lastLog)
            m_lastLog->appendLog(QStringLiteral("\n") + log + QStringLiteral("\n"));
    });
    act(QStringLiteral("exit"), tr("E&xit"), QKeySequence(Qt::CTRL | Qt::Key_Q), [this]() { close(); });

    // Shared edit/view
    act(QStringLiteral("undo"), tr("&Undo"), QKeySequence(Qt::CTRL | Qt::Key_Z), [this]() { if (auto *d = activeDoc()) d->undo(); }, QStringLiteral("undo"));
    act(QStringLiteral("redo"), tr("&Redo"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this]() { if (auto *d = activeDoc()) d->redo(); }, QStringLiteral("redo"));
    act(QStringLiteral("zoomIn"), tr("Zoom &In"), {}, [this]() { if (auto *d = activeDoc()) d->zoomIn(); }, QStringLiteral("zoomin"));
    act(QStringLiteral("zoomOut"), tr("Zoom &Back"), QKeySequence(Qt::SHIFT | Qt::Key_Z), [this]() { if (auto *d = activeDoc()) d->zoomBack(); }, QStringLiteral("zoomout"));
    act(QStringLiteral("zoomFit"), tr("Zoom to &Fit"), QKeySequence(Qt::Key_Space), [this]() { if (auto *d = activeDoc()) d->zoomFit(); }, QStringLiteral("zoomfit"));
    act(QStringLiteral("zoomArea"), tr("&Zoom Area"), QKeySequence(Qt::Key_Z), [this]() {
        if (auto *s = activeSchematic())
            s->setMode(SchematicView::Mode::ZoomArea);
    });
    QAction *grid = act(QStringLiteral("grid"), tr("Show &Grid"), QKeySequence(Qt::CTRL | Qt::Key_G), [this]() {
        if (auto *s = activeSchematic())
            s->setGridVisible(!s->gridVisible());
        else if (auto *w = activeWaveform()) {
            w->gridOn = !w->gridOn;
            for (auto *p : w->panes())
                p->update();
        }
    });
    Q_UNUSED(grid);
    QAction *unconn = act(QStringLiteral("unconn"), tr("Mark &Unconnected Pins"), QKeySequence(Qt::CTRL | Qt::Key_U), [this]() {
        if (auto *s = activeSchematic()) {
            const bool on = !AppSettings::instance().markUnconnectedPins();
            AppSettings::instance().setMarkUnconnectedPins(on);
            s->setMarkUnconnected(on);
        }
    });
    Q_UNUSED(unconn);
    QAction *anchors = act(QStringLiteral("anchors"), tr("Mark Text &Anchors"), QKeySequence(Qt::CTRL | Qt::Key_A), [this]() {
        static bool on = false;
        on = !on;
        if (auto *s = activeSchematic())
            s->setShowAnchors(on);
    });
    Q_UNUSED(anchors);
    act(QStringLiteral("copyImage"), tr("Copy Bitmap to Clipboard"), {}, [this]() { if (auto *d = activeDoc()) d->copyImageToClipboard(); });

    // Schematic editing
    act(QStringLiteral("wire"), tr("&Wire"), QKeySequence(Qt::Key_W), [this]() { if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::Wire); }, QStringLiteral("wire"));
    m_actions[QStringLiteral("wire")]->setShortcuts({QKeySequence(Qt::Key_W), QKeySequence(Qt::Key_F3)});
    act(QStringLiteral("ground"), tr("Place &GND"), QKeySequence(Qt::Key_G), [this]() { placeLabel(true); }, QStringLiteral("ground"));
    act(QStringLiteral("com"), tr("Place COM"), QKeySequence(Qt::ALT | Qt::Key_G), [this]() { placeLabel(true, true); });
    act(QStringLiteral("label"), tr("Label &Net..."), QKeySequence(Qt::Key_N), [this]() { placeLabel(false); }, QStringLiteral("label"));
    m_actions[QStringLiteral("label")]->setShortcuts({QKeySequence(Qt::Key_N), QKeySequence(Qt::Key_F4)});
    act(QStringLiteral("resistor"), tr("&Resistor"), QKeySequence(Qt::Key_R), [this]() { placeQuick(QStringLiteral("res")); }, QStringLiteral("resistor"));
    act(QStringLiteral("capacitor"), tr("&Capacitor"), QKeySequence(Qt::Key_C), [this]() { placeQuick(QStringLiteral("cap")); }, QStringLiteral("capacitor"));
    act(QStringLiteral("inductor"), tr("&Inductor"), QKeySequence(Qt::Key_L), [this]() { placeQuick(QStringLiteral("ind")); }, QStringLiteral("inductor"));
    act(QStringLiteral("diode"), tr("&Diode"), QKeySequence(Qt::Key_D), [this]() { placeQuick(QStringLiteral("diode")); }, QStringLiteral("diode"));
    act(QStringLiteral("vsource"), tr("&Voltage Source"), QKeySequence(Qt::Key_V), [this]() { placeQuick(QStringLiteral("voltage")); });
    act(QStringLiteral("isource"), tr("Current &Source"), QKeySequence(Qt::Key_I), [this]() { placeQuick(QStringLiteral("current")); });
    act(QStringLiteral("component"), tr("Com&ponent..."), QKeySequence(Qt::Key_P), [this]() { componentDialog(); }, QStringLiteral("component"));
    m_actions[QStringLiteral("component")]->setShortcuts({QKeySequence(Qt::Key_P), QKeySequence(Qt::Key_F2)});
    act(QStringLiteral("textItem"), tr("&Text..."), QKeySequence(Qt::Key_T), [this]() { placeText(false); }, QStringLiteral("text"));
    act(QStringLiteral("directive"), tr("SPICE &Directive..."), QKeySequence(Qt::Key_Period), [this]() { placeText(true); }, QStringLiteral("directive"));
    act(QStringLiteral("analysis"), tr("Configure &Analysis..."), QKeySequence(Qt::Key_A), [this]() { editAnalysis(-1); }, QStringLiteral("settings"));
    act(QStringLiteral("bustap"), tr("Place &Bus Tap"), QKeySequence(Qt::Key_B), [this]() {
        if (auto *s = activeSchematic()) {
            s->schematicRef().busTaps << BusTap{QPoint(0, 0), QPoint(16, 16)};
            s->commitExternalChange();
        }
    });
    act(QStringLiteral("dataflag"), tr("Place Data Flag (op point readout)"), {}, [this]() {
        if (auto *s = activeSchematic())
            s->setMode(SchematicView::Mode::PlaceDataFlag);
    });
    act(QStringLiteral("rotate"), tr("R&otate"), QKeySequence(Qt::CTRL | Qt::Key_R), [this]() {
        if (auto *s = activeSchematic()) s->rotateSelection();
        else if (auto *y = activeSymbol()) y->rotateSelection();
    }, QStringLiteral("rotate"));
    act(QStringLiteral("mirror"), tr("&Mirror"), QKeySequence(Qt::CTRL | Qt::Key_E), [this]() {
        if (auto *s = activeSchematic()) s->mirrorSelection();
        else if (auto *y = activeSymbol()) y->mirrorSelection();
    }, QStringLiteral("mirror"));
    act(QStringLiteral("delete"), tr("De&lete"), QKeySequence(Qt::Key_Delete), [this]() {
        if (auto *s = activeSchematic()) s->deleteSelection();
        else if (auto *y = activeSymbol()) y->deleteSelection();
    }, QStringLiteral("delete"));
    m_actions[QStringLiteral("delete")]->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace), QKeySequence(Qt::Key_F5)});
    act(QStringLiteral("copy"), tr("&Copy / Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_C), [this]() { if (auto *s = activeSchematic()) s->copySelection(); }, QStringLiteral("copy"));
    m_actions[QStringLiteral("copy")]->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_C), QKeySequence(Qt::Key_F6)});
    act(QStringLiteral("cut"), tr("Cu&t"), QKeySequence(Qt::CTRL | Qt::Key_X), [this]() { if (auto *s = activeSchematic()) s->cutSelection(); });
    act(QStringLiteral("paste"), tr("&Paste"), QKeySequence(Qt::CTRL | Qt::Key_V), [this]() { if (auto *s = activeSchematic()) s->paste(); }, QStringLiteral("paste"));
    act(QStringLiteral("move"), tr("&Move"), QKeySequence(Qt::Key_M), [this]() { if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::Move); }, QStringLiteral("move"));
    m_actions[QStringLiteral("move")]->setShortcuts({QKeySequence(Qt::Key_M), QKeySequence(Qt::Key_F7)});
    act(QStringLiteral("drag"), tr("&Drag (Stretch)"), QKeySequence(Qt::Key_S), [this]() { if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::Drag); }, QStringLiteral("drag"));
    m_actions[QStringLiteral("drag")]->setShortcuts({QKeySequence(Qt::Key_S), QKeySequence(Qt::Key_F8)});
    act(QStringLiteral("selectAll"), tr("Select A&ll"), {}, [this]() { if (auto *s = activeSchematic()) s->selectAll(); });
    act(QStringLiteral("drawLine"), tr("&Line"), {}, [this]() {
        if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::DrawLine);
        else if (auto *y = activeSymbol()) y->setMode(SymbolEditor::Mode::DrawLine);
    });
    act(QStringLiteral("drawRect"), tr("&Rectangle"), {}, [this]() {
        if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::DrawRect);
        else if (auto *y = activeSymbol()) y->setMode(SymbolEditor::Mode::DrawRect);
    });
    act(QStringLiteral("drawCircle"), tr("&Circle"), {}, [this]() {
        if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::DrawCircle);
        else if (auto *y = activeSymbol()) y->setMode(SymbolEditor::Mode::DrawCircle);
    });
    act(QStringLiteral("drawArc"), tr("&Arc"), {}, [this]() {
        if (auto *s = activeSchematic()) s->setMode(SchematicView::Mode::DrawArc);
        else if (auto *y = activeSymbol()) y->setMode(SymbolEditor::Mode::DrawArc);
    });
    act(QStringLiteral("find"), tr("&Find..."), QKeySequence(Qt::CTRL | Qt::Key_F), [this]() { findInSchematic(); });
    act(QStringLiteral("viewNetlist"), tr("Update and View SPICE &Netlist"), {}, [this]() { viewNetlist(); }, QStringLiteral("netlist"));
    act(QStringLiteral("bom"), tr("&Bill of Materials..."), {}, [this]() { if (auto *s = activeSchematic()) dialogs::showBillOfMaterials(this, s->schematic()); });
    QAction *opLabels = act(QStringLiteral("opLabels"), tr("Show &Operating Point on Schematic"), {}, []() {});
    opLabels->setCheckable(true);
    connect(opLabels, &QAction::toggled, this, [this](bool on) {
        if (auto *s = activeSchematic())
            s->showOperatingPointLabels(on);
    });

    // Hierarchy
    act(QStringLiteral("openSymbol"), tr("Open This Sheet's &Symbol"), {}, [this]() { openSymbolForSheet(); });
    act(QStringLiteral("genSubckt"), tr("Generate Subckt Netlist"), {}, [this]() {
        SchematicView *s = activeSchematic();
        if (!s)
            return;
        // A .subckt whose ports are the sheet's I/O pins.
        const NetlistResult r = s->netlist();
        QStringList ports;
        for (const NetFlag &f : s->schematic().flags)
            if (!f.ioDirection.isEmpty() && !ports.contains(f.name))
                ports << f.name;
        QStringList out;
        const QString name = QFileInfo(s->filePath()).completeBaseName();
        out << QStringLiteral(".subckt %1 %2").arg(name.isEmpty() ? QStringLiteral("sub") : name, ports.join(' '));
        for (const QString &l : r.text.split('\n')) {
            const QString t = l.trimmed().toLower();
            if (l.startsWith('*') || t == QLatin1String(".end") || t == QLatin1String(".backanno") || t.startsWith(QLatin1String(".tran"))
                || t.startsWith(QLatin1String(".ac")) || t.startsWith(QLatin1String(".dc")) || t.startsWith(QLatin1String(".op")) || l.isEmpty())
                continue;
            out << l;
        }
        out << QStringLiteral(".ends %1").arg(name);
        auto *ed = new NetlistEditor;
        ed->setText(out.join('\n') + '\n', name + QStringLiteral(".sub"));
        addDocument(ed);
    });

    // Simulation
    act(QStringLiteral("run"), tr("&Run / Pause"), QKeySequence(Qt::ALT | Qt::Key_R), [this]() { runOrPause(); }, QStringLiteral("run"));
    act(QStringLiteral("stop"), tr("&Halt"), QKeySequence(Qt::ALT | Qt::Key_S), [this]() { stopSimulation(); }, QStringLiteral("stop"));
    act(QStringLiteral("log"), tr("SPICE Output &Log"), QKeySequence(Qt::CTRL | Qt::Key_L), [this]() { showLog(); });
    act(QStringLiteral("opPoint"), tr("Show Operating &Point"), {}, [this]() {
        DocumentWidget *root = simulationRoot(activeDoc());
        if (root && m_sessions.contains(root) && m_sessions[root].hasResult)
            dialogs::showOperatingPoint(this, m_sessions[root].last.operatingPoint, tr("Operating Point — %1").arg(root->displayName()));
    });

    // Tools
    act(QStringLiteral("settings"), tr("&Settings..."), {}, [this]() {
        SettingsDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            for (QMdiSubWindow *w : m_mdi->subWindowList())
                if (auto *s = qobject_cast<SchematicView *>(w->widget()))
                    s->reloadStyle();
    }, QStringLiteral("settings"));
    act(QStringLiteral("colors"), tr("&Color Preferences..."), {}, [this]() {
        SettingsDialog dlg(this, 4);
        if (dlg.exec() == QDialog::Accepted)
            for (QMdiSubWindow *w : m_mdi->subWindowList())
                if (auto *s = qobject_cast<SchematicView *>(w->widget()))
                    s->reloadStyle();
    });
    act(QStringLiteral("importLib"), tr("&Import LTspice Library..."), {}, [this]() {
        LibraryImportDialog dlg(this);
        dlg.exec();
    });
    act(QStringLiteral("exportNetlist"), tr("&Export Netlist (PCB)..."), {}, [this]() {
        if (auto *s = activeSchematic())
            dialogs::exportPcbNetlist(this, s->schematic());
    });
    act(QStringLiteral("rescan"), tr("Rescan Symbol and Model &Libraries"), {}, [this]() {
        SymbolLibrary::instance().rescan();
        ModelLibrary::instance().clearCache();
        statusBar()->showMessage(tr("%1 symbols available").arg(SymbolLibrary::instance().entries().size()), 4000);
    });

    // Window
    act(QStringLiteral("tileH"), tr("Tile &Horizontally"), {}, [this]() {
        m_mdi->setViewMode(QMdiArea::SubWindowView);
        const auto wins = m_mdi->subWindowList();
        if (wins.isEmpty())
            return;
        const int h = m_mdi->viewport()->height() / int(wins.size());
        for (int i = 0; i < wins.size(); ++i) {
            wins[i]->showNormal();
            wins[i]->setGeometry(0, i * h, m_mdi->viewport()->width(), h);
        }
    }, QStringLiteral("tile"));
    act(QStringLiteral("tileV"), tr("Tile &Vertically"), {}, [this]() {
        m_mdi->setViewMode(QMdiArea::SubWindowView);
        const auto wins = m_mdi->subWindowList();
        if (wins.isEmpty())
            return;
        const int w = m_mdi->viewport()->width() / int(wins.size());
        for (int i = 0; i < wins.size(); ++i) {
            wins[i]->showNormal();
            wins[i]->setGeometry(i * w, 0, w, m_mdi->viewport()->height());
        }
    });
    act(QStringLiteral("cascade"), tr("&Cascade"), {}, [this]() {
        m_mdi->setViewMode(QMdiArea::SubWindowView);
        m_mdi->cascadeSubWindows();
    }, QStringLiteral("cascade"));
    act(QStringLiteral("closeAll"), tr("Close &Everything"), {}, [this]() {
        for (QMdiSubWindow *w : m_mdi->subWindowList())
            w->close();
    });
    QAction *tabs = act(QStringLiteral("tabs"), tr("Window &Tabs"), {}, []() {});
    tabs->setCheckable(true);
    tabs->setChecked(m_mdi->viewMode() == QMdiArea::TabbedView);
    connect(tabs, &QAction::toggled, this, [this](bool on) {
        m_mdi->setViewMode(on ? QMdiArea::TabbedView : QMdiArea::SubWindowView);
        AppSettings::instance().raw().setValue(QStringLiteral("ui/tabs"), on);
    });

    // Help
    act(QStringLiteral("shortcuts"), tr("&Keyboard Shortcut Cheat Sheet"), QKeySequence(Qt::Key_F1), [this]() { dialogs::showShortcuts(this); });
    act(QStringLiteral("about"), tr("&About LinSpice"), {}, [this]() { dialogs::showAbout(this); });

    // Waveform (Plot Settings)
    act(QStringLiteral("addTrace"), tr("&Add Traces..."), QKeySequence(Qt::Key_A), [this]() { addTraces(); }, QStringLiteral("waveform"));
    act(QStringLiteral("delTraces"), tr("&Delete Traces"), {}, [this]() {
        if (auto *w = activeWaveform())
            if (w->activePane() && !w->activePane()->traces.isEmpty())
                w->removeTrace(w->activePane(), int(w->activePane()->traces.size()) - 1);
    });
    act(QStringLiteral("selectSteps"), tr("Select &Steps..."), QKeySequence(Qt::SHIFT | Qt::Key_S), [this]() { if (auto *w = activeWaveform()) w->showStepSelector(); });
    act(QStringLiteral("cursor"), tr("Place &Cursor on Active Trace"), QKeySequence(Qt::Key_C), [this]() {
        if (auto *w = activeWaveform())
            if (w->activePane() && !w->activePane()->traces.isEmpty())
                w->attachCursor(w->cursors[0].on ? 1 : 0, w->activePane(), 0);
    }, QStringLiteral("cursor"));
    act(QStringLiteral("clearCursors"), tr("C&lear All Cursors"), QKeySequence(Qt::SHIFT | Qt::Key_C), [this]() { if (auto *w = activeWaveform()) w->clearCursors(); });
    act(QStringLiteral("paneAbove"), tr("Add Plot Pane A&bove"), QKeySequence(Qt::Key_P), [this]() { if (auto *w = activeWaveform()) w->addPane(true); });
    act(QStringLiteral("paneBelow"), tr("Add Plot Pane &Below"), QKeySequence(Qt::Key_B), [this]() { if (auto *w = activeWaveform()) w->addPane(false); });
    act(QStringLiteral("paneUp"), tr("Move Active Pane &Up"), QKeySequence(Qt::Key_U), [this]() { if (auto *w = activeWaveform()) w->movePane(w->activePane(), -1); });
    act(QStringLiteral("paneDown"), tr("Move Active Pane &Down"), QKeySequence(Qt::Key_D), [this]() { if (auto *w = activeWaveform()) w->movePane(w->activePane(), 1); });
    act(QStringLiteral("paneDelete"), tr("Delete Active Pane"), {}, [this]() { if (auto *w = activeWaveform()) w->deletePane(w->activePane()); });
    QAction *sync = act(QStringLiteral("syncAxes"), tr("Sync. Horiz. Axes"), {}, []() {});
    sync->setCheckable(true);
    sync->setChecked(true);
    connect(sync, &QAction::toggled, this, [this](bool on) { if (auto *w = activeWaveform()) w->syncHorizontal = on; });
    act(QStringLiteral("autorangeY"), tr("Autorange &Y-axis"), QKeySequence(Qt::CTRL | Qt::Key_Y), [this]() {
        if (auto *w = activeWaveform())
            for (auto *p : w->panes()) {
                p->yLeft.autoRange = p->yRight.autoRange = true;
                p->autorangeY();
            }
    });
    QAction *marks = act(QStringLiteral("markPoints"), tr("&Mark Data Points"), {}, []() {});
    marks->setCheckable(true);
    connect(marks, &QAction::toggled, this, [this](bool on) {
        if (auto *w = activeWaveform()) {
            w->markPoints = on;
            for (auto *p : w->panes())
                p->update();
        }
    });
    act(QStringLiteral("fft"), tr("&FFT..."), {}, [this]() { if (auto *w = activeWaveform()) w->openFftDialog(); });
    act(QStringLiteral("exportData"), tr("&Export Data as Text..."), {}, [this]() {
        WaveformView *w = activeWaveform();
        if (!w)
            return;
        QStringList exprs = w->allTraceExpressions();
        if (exprs.isEmpty())
            exprs = w->availableVectors().mid(1);
        const QString f = QFileDialog::getSaveFileName(this, tr("Export data"), QFileInfo(w->filePath()).completeBaseName() + QStringLiteral(".txt"),
                                                       tr("Text (*.txt *.csv)"));
        if (!f.isEmpty() && !w->exportData(f, exprs))
            QMessageBox::warning(this, tr("Export"), tr("Could not write %1").arg(f));
    });
    act(QStringLiteral("savePlt"), tr("Save Plot Settings &As..."), {}, [this]() {
        WaveformView *w = activeWaveform();
        if (!w)
            return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Save plot settings"), w->defaultPlotSettingsPath(), tr("Plot settings (*.plt)"));
        if (!f.isEmpty())
            w->savePlotSettings(f);
    });
    act(QStringLiteral("openPlt"), tr("&Open Plot Settings File..."), QKeySequence(Qt::Key_O), [this]() {
        WaveformView *w = activeWaveform();
        if (!w)
            return;
        const QString f = QFileDialog::getOpenFileName(this, tr("Open plot settings"), QFileInfo(w->defaultPlotSettingsPath()).absolutePath(),
                                                       tr("Plot settings (*.plt)"));
        if (!f.isEmpty() && !w->loadPlotSettings(f))
            QMessageBox::information(this, tr("Plot settings"), tr("No settings for this analysis type in %1").arg(f));
    });
    act(QStringLiteral("reloadPlt"), tr("&Reload Plot Settings"), QKeySequence(Qt::CTRL | Qt::Key_Space), [this]() {
        if (auto *w = activeWaveform())
            w->loadPlotSettings(w->defaultPlotSettingsPath());
    });
    act(QStringLiteral("editPlotDefs"), tr("Edit Plot &Defs File..."), {}, [this]() {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/plot.defs");
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!QFileInfo::exists(path)) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly))
                f.write("* User-defined functions for the waveform viewer, e.g.\n* .func dBV(x) {20*log10(abs(x))}\n");
        }
        openFile(path);
    });
    act(QStringLiteral("resetColors"), tr("Reset &Colors"), {}, [this]() {
        if (auto *w = activeWaveform())
            for (auto *p : w->panes()) {
                for (auto &t : p->traces)
                    t.autoColor = true;
                p->recompute();
            }
    });

    // Symbol editor
    act(QStringLiteral("symAttrs"), tr("Edit &Attributes..."), {}, [this]() { if (auto *y = activeSymbol()) y->editAttributes(); });
    act(QStringLiteral("symWindow"), tr("Attribute &Window..."), {}, [this]() { if (auto *y = activeSymbol()) y->startPlaceWindow(); });
    act(QStringLiteral("symPin"), tr("Add &Pin/Port"), {}, [this]() { if (auto *y = activeSymbol()) y->setMode(SymbolEditor::Mode::AddPin); });
    act(QStringLiteral("symText"), tr("&Text"), {}, [this]() { if (auto *y = activeSymbol()) y->setMode(SymbolEditor::Mode::PlaceText); });
}

void MainWindow::createMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    for (const char *id : {"new", "newSymbol", "open", "examples"})
        file->addAction(action(QLatin1String(id)));
    file->addSeparator();
    for (const char *id : {"save", "saveAs", "close"})
        file->addAction(action(QLatin1String(id)));
    file->addSeparator();
    file->addAction(action(QStringLiteral("execMeas")));
    file->addAction(action(QStringLiteral("exportImage")));
    file->addSeparator();
    for (const char *id : {"print", "printPreview", "printMono"})
        file->addAction(action(QLatin1String(id)));
    file->addSeparator();
    m_recentMenu = file->addMenu(tr("Recent &Files"));
    updateRecentMenu();
    file->addSeparator();
    file->addAction(action(QStringLiteral("exit")));

    m_editMenu = menuBar()->addMenu(tr("&Edit"));
    for (const char *id : {"undo", "redo"})
        m_editMenu->addAction(action(QLatin1String(id)));
    m_editMenu->addSeparator();
    for (const char *id : {"textItem", "directive", "analysis"})
        m_editMenu->addAction(action(QLatin1String(id)));
    m_editMenu->addSeparator();
    for (const char *id : {"resistor", "capacitor", "inductor", "diode", "vsource", "isource", "component"})
        m_editMenu->addAction(action(QLatin1String(id)));
    m_editMenu->addSeparator();
    for (const char *id : {"rotate", "mirror", "wire", "label", "ground", "com", "bustap", "dataflag"})
        m_editMenu->addAction(action(QLatin1String(id)));
    m_editMenu->addSeparator();
    for (const char *id : {"delete", "copy", "cut", "paste", "move", "drag", "selectAll", "find"})
        m_editMenu->addAction(action(QLatin1String(id)));
    QMenu *draw = m_editMenu->addMenu(tr("D&raft"));
    for (const char *id : {"drawLine", "drawRect", "drawCircle", "drawArc"})
        draw->addAction(action(QLatin1String(id)));

    m_symMenu = menuBar()->addMenu(tr("S&ymbol"));
    for (const char *id : {"undo", "redo", "symAttrs", "symWindow", "symPin", "symText", "rotate", "mirror", "delete"})
        m_symMenu->addAction(action(QLatin1String(id)));
    QMenu *sdraw = m_symMenu->addMenu(tr("D&raw"));
    for (const char *id : {"drawLine", "drawRect", "drawCircle", "drawArc"})
        sdraw->addAction(action(QLatin1String(id)));

    m_hierMenu = menuBar()->addMenu(tr("H&ierarchy"));
    m_hierMenu->addAction(action(QStringLiteral("openSymbol")));
    m_hierMenu->addAction(action(QStringLiteral("genSubckt")));

    m_viewMenu = menuBar()->addMenu(tr("&View"));
    for (const char *id : {"zoomArea", "zoomOut", "zoomFit", "zoomIn"})
        m_viewMenu->addAction(action(QLatin1String(id)));
    m_viewMenu->addSeparator();
    for (const char *id : {"grid", "unconn", "anchors", "opLabels"})
        m_viewMenu->addAction(action(QLatin1String(id)));
    m_viewMenu->addSeparator();
    for (const char *id : {"bom", "viewNetlist", "log", "opPoint"})
        m_viewMenu->addAction(action(QLatin1String(id)));

    m_plotMenu = menuBar()->addMenu(tr("&Plot Settings"));
    for (const char *id : {"addTrace", "delTraces", "selectSteps", "cursor", "clearCursors"})
        m_plotMenu->addAction(action(QLatin1String(id)));
    m_plotMenu->addSeparator();
    for (const char *id : {"paneAbove", "paneBelow", "paneUp", "paneDown", "paneDelete", "syncAxes"})
        m_plotMenu->addAction(action(QLatin1String(id)));
    m_plotMenu->addSeparator();
    for (const char *id : {"grid", "resetColors", "markPoints", "autorangeY", "fft", "exportData"})
        m_plotMenu->addAction(action(QLatin1String(id)));
    m_plotMenu->addSeparator();
    for (const char *id : {"savePlt", "openPlt", "reloadPlt", "editPlotDefs"})
        m_plotMenu->addAction(action(QLatin1String(id)));

    m_simMenu = menuBar()->addMenu(tr("Si&mulate"));
    for (const char *id : {"run", "stop", "analysis", "log", "opPoint"})
        m_simMenu->addAction(action(QLatin1String(id)));

    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    for (const char *id : {"copyImage", "exportImage", "settings", "colors", "exportNetlist", "importLib", "rescan"})
        tools->addAction(action(QLatin1String(id)));

    m_windowMenu = menuBar()->addMenu(tr("&Window"));
    connect(m_windowMenu, &QMenu::aboutToShow, this, [this]() {
        m_windowMenu->clear();
        for (const char *id : {"tileH", "tileV", "cascade", "closeAll", "tabs"})
            m_windowMenu->addAction(action(QLatin1String(id)));
        m_windowMenu->addSeparator();
        int i = 1;
        for (QMdiSubWindow *w : m_mdi->subWindowList()) {
            auto *doc = qobject_cast<DocumentWidget *>(w->widget());
            QAction *a = m_windowMenu->addAction(QStringLiteral("&%1 %2").arg(i++).arg(doc ? doc->displayName() : w->windowTitle()));
            a->setCheckable(true);
            a->setChecked(w == m_mdi->activeSubWindow());
            connect(a, &QAction::triggered, this, [this, w]() { m_mdi->setActiveSubWindow(w); });
        }
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(action(QStringLiteral("shortcuts")));
    help->addAction(action(QStringLiteral("examples")));
    help->addSeparator();
    help->addAction(action(QStringLiteral("about")));
}

void MainWindow::createToolBars()
{
    m_mainBar = addToolBar(tr("Main"));
    m_mainBar->setObjectName(QStringLiteral("mainToolbar"));
    m_mainBar->setIconSize(QSize(22, 22));
    for (const char *id : std::initializer_list<const char *>{"new", "open", "save", nullptr, "settings", "run", "stop", nullptr, "zoomIn", "zoomOut", "zoomFit", nullptr,
                           "tileH", "cascade", "close", nullptr, "print"}) {
        if (!id)
            m_mainBar->addSeparator();
        else
            m_mainBar->addAction(action(QLatin1String(id)));
    }
    m_editBar = addToolBar(tr("Edit"));
    m_editBar->setObjectName(QStringLiteral("editToolbar"));
    m_editBar->setIconSize(QSize(22, 22));
    for (const char *id : std::initializer_list<const char *>{"wire", "ground", "label", "resistor", "capacitor", "inductor", "diode", "component", nullptr, "rotate",
                           "mirror", nullptr, "undo", "redo", nullptr, "textItem", "directive", nullptr, "move", "drag", "delete", "copy",
                           "paste", nullptr, "waveform"}) {
        if (!id)
            m_editBar->addSeparator();
        else if (QString::fromLatin1(id) == QLatin1String("waveform"))
            m_editBar->addAction(action(QStringLiteral("addTrace")));
        else
            m_editBar->addAction(action(QLatin1String(id)));
    }
}

void MainWindow::updateActions()
{
    DocumentWidget *doc = activeDoc();
    const bool sch = qobject_cast<SchematicView *>(doc) != nullptr;
    const bool wave = qobject_cast<WaveformView *>(doc) != nullptr;
    const bool sym = qobject_cast<SymbolEditor *>(doc) != nullptr;
    const bool net = qobject_cast<NetlistEditor *>(doc) != nullptr;
    static const QStringList schematicOnly = {
        QStringLiteral("wire"),     QStringLiteral("ground"),   QStringLiteral("com"),      QStringLiteral("label"),
        QStringLiteral("resistor"), QStringLiteral("capacitor"), QStringLiteral("inductor"), QStringLiteral("diode"),
        QStringLiteral("vsource"),  QStringLiteral("isource"),  QStringLiteral("component"), QStringLiteral("textItem"),
        QStringLiteral("directive"), QStringLiteral("analysis"), QStringLiteral("bustap"),   QStringLiteral("copy"),
        QStringLiteral("cut"),      QStringLiteral("paste"),    QStringLiteral("move"),     QStringLiteral("drag"),
        QStringLiteral("selectAll"), QStringLiteral("find"),    QStringLiteral("viewNetlist"), QStringLiteral("bom"),
        QStringLiteral("unconn"),   QStringLiteral("anchors"),  QStringLiteral("zoomArea"), QStringLiteral("exportNetlist"),
        QStringLiteral("openSymbol"), QStringLiteral("genSubckt"), QStringLiteral("opLabels"), QStringLiteral("dataflag")};
    static const QStringList waveOnly = {
        QStringLiteral("addTrace"),  QStringLiteral("delTraces"), QStringLiteral("selectSteps"), QStringLiteral("cursor"),
        QStringLiteral("clearCursors"), QStringLiteral("paneAbove"), QStringLiteral("paneBelow"), QStringLiteral("paneUp"),
        QStringLiteral("paneDown"),  QStringLiteral("paneDelete"), QStringLiteral("syncAxes"), QStringLiteral("autorangeY"),
        QStringLiteral("markPoints"), QStringLiteral("fft"),      QStringLiteral("exportData"), QStringLiteral("savePlt"),
        QStringLiteral("openPlt"),   QStringLiteral("reloadPlt"), QStringLiteral("resetColors"), QStringLiteral("execMeas")};
    static const QStringList symOnly = {QStringLiteral("symAttrs"), QStringLiteral("symWindow"), QStringLiteral("symPin"), QStringLiteral("symText")};
    for (const QString &id : schematicOnly)
        if (auto *a = action(id))
            a->setEnabled(sch);
    for (const QString &id : waveOnly)
        if (auto *a = action(id))
            a->setEnabled(wave);
    for (const QString &id : symOnly)
        if (auto *a = action(id))
            a->setEnabled(sym);
    for (const char *id : {"rotate", "mirror", "delete", "drawLine", "drawRect", "drawCircle", "drawArc"})
        action(QLatin1String(id))->setEnabled(sch || sym);
    action(QStringLiteral("grid"))->setEnabled(sch || wave);
    action(QStringLiteral("zoomFit"))->setEnabled(sch || wave || sym);
    action(QStringLiteral("zoomOut"))->setEnabled(sch || wave || sym);
    action(QStringLiteral("zoomIn"))->setEnabled(doc != nullptr);
    action(QStringLiteral("save"))->setEnabled(doc && doc->kind() != DocumentWidget::Kind::Waveform && doc->kind() != DocumentWidget::Kind::Log);
    action(QStringLiteral("saveAs"))->setEnabled(action(QStringLiteral("save"))->isEnabled());
    action(QStringLiteral("close"))->setEnabled(doc != nullptr);
    action(QStringLiteral("undo"))->setEnabled(sch || sym || net || wave);
    action(QStringLiteral("redo"))->setEnabled(sch || sym || net);
    DocumentWidget *root = simulationRoot(doc);
    const bool canRun = root != nullptr;
    action(QStringLiteral("run"))->setEnabled(canRun);
    const bool running = root && m_sessions.contains(root) && m_sessions[root].controller && m_sessions[root].controller->isRunning();
    action(QStringLiteral("stop"))->setEnabled(running);
    action(QStringLiteral("opPoint"))->setEnabled(root && m_sessions.contains(root) && m_sessions[root].hasResult);
    m_editMenu->menuAction()->setVisible(!wave && !sym);
    m_symMenu->menuAction()->setVisible(sym);
    m_plotMenu->menuAction()->setVisible(wave);
    m_hierMenu->menuAction()->setVisible(sch);
    m_editBar->setEnabled(sch || wave || sym);
}

// ---------------------------------------------------------------- documents

QMdiSubWindow *MainWindow::addDocument(DocumentWidget *doc)
{
    QMdiSubWindow *w = m_mdi->addSubWindow(doc);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setWindowIcon(icons::get(doc->kind() == DocumentWidget::Kind::Waveform ? QStringLiteral("waveform")
                                : doc->kind() == DocumentWidget::Kind::Netlist ? QStringLiteral("netlist")
                                : doc->kind() == DocumentWidget::Kind::Log ? QStringLiteral("text")
                                : doc->kind() == DocumentWidget::Kind::Symbol ? QStringLiteral("component")
                                                                               : QStringLiteral("resistor")));
    updateWindowTitle(doc);
    connect(doc, &DocumentWidget::titleChanged, this, [this, doc]() { updateWindowTitle(doc); });
    connect(doc, &DocumentWidget::modifiedChanged, this, [this, doc]() { updateWindowTitle(doc); });
    connect(doc, &DocumentWidget::statusMessage, this, [this](const QString &t, int ms) {
        if (ms > 0)
            statusBar()->showMessage(t, ms);
        else
            m_statusLabel->setText(t);
    });
    // Ask before closing modified documents.
    w->installEventFilter(this);
    connect(w, &QObject::destroyed, this, [this, doc]() {
        if (!m_mdi)
            return;
        if (m_sessions.contains(doc)) {
            SimSession s = m_sessions.take(doc);
            if (s.controller)
                s.controller->deleteLater();
        }
        updateActions();
    });
    if (auto *sch = qobject_cast<SchematicView *>(doc)) {
        connect(sch, &SchematicView::probe, this, [this, sch](const QString &e, bool append) { handleProbe(sch, e, append); });
        connect(sch, &SchematicView::probeHover, this, [this](const QString &t) { m_statusLabel->setText(t); });
        connect(sch, &SchematicView::requestComponentDialog, this, [this]() { componentDialog(); });
        connect(sch, &SchematicView::requestAnalysisDialog, this, [this](int idx) { editAnalysis(idx); });
        connect(sch, &SchematicView::requestRun, this, [this]() { runOrPause(); });
        connect(sch, &SchematicView::openSubSheet, this, [this, sch](const QString &asc, const QString &inst) { openSubSheet(sch, asc, inst); });
    }
    if (auto *ne = qobject_cast<NetlistEditor *>(doc))
        connect(ne, &NetlistEditor::createSymbolRequested, this, &MainWindow::createSymbolFromSubckt);
    if (auto *wv = qobject_cast<WaveformView *>(doc)) {
        connect(wv, &WaveformView::addTracesRequested, this, [this]() { addTraces(); });
        connect(wv, &WaveformView::requestNewWaveform, this, [this](DatasetPtr ds, const QStringList &traces, const QString &title) {
            auto *v = new WaveformView;
            v->setDataset(ds, QString(), title);
            v->addTraces(traces);
            addDocument(v)->showMaximized();
        });
        connect(wv, &WaveformView::traceSelected, this, [this, wv](const QString &expr) {
            // Cross-probe: highlight nets of V(...) expressions in the source schematic.
            for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
                if (it->wave != wv)
                    continue;
                if (auto *s = qobject_cast<SchematicView *>(it->source.data())) {
                    QSet<QString> nets;
                    static const QRegularExpression vre(QStringLiteral("V\\(([^,)]+)(?:,([^)]+))?\\)"), QRegularExpression::CaseInsensitiveOption);
                    auto m = vre.globalMatch(expr);
                    while (m.hasNext()) {
                        const auto mm = m.next();
                        nets.insert(mm.captured(1));
                        if (!mm.captured(2).isEmpty())
                            nets.insert(mm.captured(2));
                    }
                    s->setHighlightNets(nets);
                }
            }
        });
    }
    doc->show();
    return w;
}

DocumentWidget *MainWindow::activeDoc() const
{
    QMdiSubWindow *w = m_mdi->activeSubWindow();
    return w ? qobject_cast<DocumentWidget *>(w->widget()) : nullptr;
}

SchematicView *MainWindow::activeSchematic() const { return qobject_cast<SchematicView *>(activeDoc()); }
WaveformView *MainWindow::activeWaveform() const { return qobject_cast<WaveformView *>(activeDoc()); }
SymbolEditor *MainWindow::activeSymbol() const { return qobject_cast<SymbolEditor *>(activeDoc()); }
NetlistEditor *MainWindow::activeNetlist() const { return qobject_cast<NetlistEditor *>(activeDoc()); }

QMdiSubWindow *MainWindow::findWindowFor(const QString &path) const
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (QMdiSubWindow *w : m_mdi->subWindowList()) {
        auto *doc = qobject_cast<DocumentWidget *>(w->widget());
        if (doc && doc->kind() != DocumentWidget::Kind::Waveform && doc->filePath() == abs)
            return w;
    }
    return nullptr;
}

void MainWindow::updateWindowTitle(DocumentWidget *doc)
{
    for (QMdiSubWindow *w : m_mdi->subWindowList()) {
        if (w->widget() == doc) {
            QString t = doc->displayName();
            if (doc->isModified())
                t += QStringLiteral(" *");
            w->setWindowTitle(t);
        }
    }
    if (doc == activeDoc())
        setWindowTitle(QStringLiteral("LinSpice - [%1]").arg(doc->displayName()));
}

bool MainWindow::maybeSave(DocumentWidget *doc)
{
    if (!doc || !doc->isModified())
        return true;
    const auto r = QMessageBox::question(this, tr("Save changes?"), tr("Save changes to %1?").arg(doc->displayName()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel)
        return false;
    if (r == QMessageBox::Save)
        return saveDoc(doc, false);
    return true;
}

void MainWindow::newSchematic()
{
    auto *v = new SchematicView;
    v->schematicRef().filePath.clear();
    ++m_untitled;
    QMdiSubWindow *w = addDocument(v);
    v->setProperty("untitledName", QStringLiteral("Draft%1.asc").arg(m_untitled));
    w->setWindowTitle(QStringLiteral("Draft%1.asc").arg(m_untitled));
    w->showMaximized();
    v->setFocus();
}

void MainWindow::newSymbol()
{
    auto *e = new SymbolEditor;
    addDocument(e)->showMaximized();
}

bool MainWindow::openFile(const QString &pathIn)
{
    const QString path = QFileInfo(pathIn).absoluteFilePath();
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("Open"), tr("File not found: %1").arg(path));
        return false;
    }
    if (QMdiSubWindow *existing = findWindowFor(path)) {
        m_mdi->setActiveSubWindow(existing);
        return true;
    }
    const QString ext = QFileInfo(path).suffix().toLower();
    QString err;
    if (ext == QLatin1String("asc")) {
        auto *v = new SchematicView;
        if (!v->load(path, &err)) {
            delete v;
            QMessageBox::warning(this, tr("Open"), err);
            return false;
        }
        addDocument(v)->showMaximized();
        const QStringList missing = v->schematic().unresolvedSymbols();
        if (!missing.isEmpty())
            statusBar()->showMessage(tr("Missing symbols: %1 — import your LTspice library (Tools > Import LTspice Library)").arg(missing.join(QStringLiteral(", "))),
                                     10000);
    } else if (ext == QLatin1String("asy")) {
        auto *e = new SymbolEditor;
        if (!e->load(path, &err)) {
            delete e;
            QMessageBox::warning(this, tr("Open"), err);
            return false;
        }
        addDocument(e)->showMaximized();
    } else if (ext == QLatin1String("raw") || ext == QLatin1String("fra") || ext == QLatin1String("qraw")) {
        openWaveformFile(path);
    } else if (ext == QLatin1String("plt")) {
        if (auto *w = activeWaveform())
            w->loadPlotSettings(path);
        else
            QMessageBox::information(this, tr("Plot settings"), tr("Open a waveform window first."));
        return true;
    } else {
        auto *e = new NetlistEditor;
        if (!e->load(path, &err)) {
            delete e;
            QMessageBox::warning(this, tr("Open"), err);
            return false;
        }
        addDocument(e)->showMaximized();
    }
    AppSettings::instance().addRecentFile(path);
    updateRecentMenu();
    return true;
}

void MainWindow::openWaveformFile(const QString &path)
{
    QString err;
    const QList<DatasetPtr> plots = rawfile::read(path, &err);
    if (plots.isEmpty()) {
        QMessageBox::warning(this, tr("Open waveform"), err);
        return;
    }
    for (const DatasetPtr &ds : plots) {
        auto *w = new WaveformView;
        w->setDataset(ds, path, QFileInfo(path).fileName() + (plots.size() > 1 ? QStringLiteral(" — ") + ds->plotName : QString()));
        // LTspice loads <name>.plt automatically.
        if (!w->loadPlotSettings(w->defaultPlotSettingsPath()) && ds->vars.size() > 1 && ds->vars.size() <= 4)
            w->addTrace(ds->vars[1].name);
        addDocument(w)->showMaximized();
    }
}

void MainWindow::openDialog()
{
    const QString dir = AppSettings::instance().raw().value(QStringLiteral("ui/lastDir"), QDir::homePath()).toString();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open"), dir,
        tr("All supported (*.asc *.asy *.raw *.net *.cir *.sp *.lib *.sub *.txt *.log *.plt);;Schematics (*.asc);;Symbols (*.asy);;"
           "Waveforms (*.raw);;Netlists (*.net *.cir *.sp);;Libraries (*.lib *.sub *.inc *.mod);;All files (*)"));
    for (const QString &f : files) {
        AppSettings::instance().raw().setValue(QStringLiteral("ui/lastDir"), QFileInfo(f).absolutePath());
        openFile(f);
    }
}

void MainWindow::openExamples()
{
    QString dir = AppSettings::instance().examplesRoot();
    if (!QFileInfo(dir).isDir()) {
        if (QMessageBox::question(this, tr("Examples"), tr("No example circuits found. Import them from your LTspice installation now?")) == QMessageBox::Yes) {
            LibraryImportDialog dlg(this);
            dlg.exec();
            dir = AppSettings::instance().examplesRoot();
        }
        if (!QFileInfo(dir).isDir())
            return;
    }
    const QString f = QFileDialog::getOpenFileName(this, tr("Open Example"), dir, tr("Schematics (*.asc);;All files (*)"));
    if (!f.isEmpty())
        openFile(f);
}

bool MainWindow::saveDoc(DocumentWidget *doc, bool forceDialog)
{
    if (!doc)
        return false;
    if (doc->kind() == DocumentWidget::Kind::Waveform || doc->kind() == DocumentWidget::Kind::Log)
        return true;
    QString path = doc->filePath();
    if (forceDialog || path.isEmpty()) {
        QString suggestion = path;
        if (suggestion.isEmpty()) {
            suggestion = doc->property("untitledName").toString();
            if (suggestion.isEmpty())
                suggestion = doc->kind() == DocumentWidget::Kind::Symbol ? QStringLiteral("symbol.asy") : QStringLiteral("netlist.net");
            suggestion = AppSettings::instance().raw().value(QStringLiteral("ui/lastDir"), QDir::homePath()).toString() + '/' + suggestion;
        }
        path = QFileDialog::getSaveFileName(this, tr("Save As"), suggestion, doc->fileFilter());
        if (path.isEmpty())
            return false;
        const QString ext = doc->kind() == DocumentWidget::Kind::Schematic ? QStringLiteral(".asc")
                          : doc->kind() == DocumentWidget::Kind::Symbol ? QStringLiteral(".asy") : QString();
        if (!ext.isEmpty() && !path.endsWith(ext, Qt::CaseInsensitive))
            path += ext;
        AppSettings::instance().raw().setValue(QStringLiteral("ui/lastDir"), QFileInfo(path).absolutePath());
    }
    const bool ok = doc->saveAs(path);
    if (ok) {
        AppSettings::instance().addRecentFile(QFileInfo(path).absoluteFilePath());
        updateRecentMenu();
        statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
        // A saved symbol may be used by open schematics.
        if (doc->kind() == DocumentWidget::Kind::Symbol)
            SymbolLibrary::instance().rescan();
    }
    return ok;
}

void MainWindow::closeActive()
{
    if (QMdiSubWindow *w = m_mdi->activeSubWindow())
        w->close();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    for (QMdiSubWindow *w : m_mdi->subWindowList()) {
        if (!maybeSave(qobject_cast<DocumentWidget *>(w->widget()))) {
            e->ignore();
            return;
        }
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
        if (it->controller)
            it->controller->stop();
    AppSettings::instance().raw().setValue(QStringLiteral("ui/geometry"), saveGeometry());
    AppSettings::instance().raw().setValue(QStringLiteral("ui/state"), saveState());
    // Modified flags were handled above; prevent sub-window prompts.
    for (QMdiSubWindow *w : m_mdi->subWindowList())
        w->removeEventFilter(this);
    e->accept();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (e->type() == QEvent::Close) {
        if (auto *w = qobject_cast<QMdiSubWindow *>(obj)) {
            if (!maybeSave(qobject_cast<DocumentWidget *>(w->widget()))) {
                e->ignore();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile())
            openFile(u.toLocalFile());
}

void MainWindow::updateRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    for (const QString &f : AppSettings::instance().recentFiles()) {
        QAction *a = m_recentMenu->addAction(QFileInfo(f).fileName());
        a->setStatusTip(f);
        a->setToolTip(f);
        connect(a, &QAction::triggered, this, [this, f]() { openFile(f); });
        connect(a, &QAction::hovered, this, [this, f]() { statusBar()->showMessage(f, 3000); });
    }
    m_recentMenu->setEnabled(!m_recentMenu->isEmpty());
}

void MainWindow::printDoc(bool preview)
{
    DocumentWidget *doc = activeDoc();
    if (!doc)
        return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageOrientation(QPageLayout::Landscape);
    if (preview) {
        QPrintPreviewDialog dlg(&printer, this);
        connect(&dlg, &QPrintPreviewDialog::paintRequested, doc, [doc](QPrinter *p) { doc->print(p); });
        dlg.exec();
        return;
    }
    QPrintDialog dlg(&printer, this);
    if (dlg.exec() == QDialog::Accepted)
        doc->print(&printer);
}

void MainWindow::exportImage()
{
    DocumentWidget *doc = activeDoc();
    if (!doc)
        return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export image"), QFileInfo(doc->displayName()).completeBaseName() + QStringLiteral(".png"),
                                                   tr("PNG image (*.png);;SVG vector image (*.svg)"));
    if (f.isEmpty())
        return;
    if (!doc->exportImage(f))
        QMessageBox::warning(this, tr("Export"), tr("Could not write %1").arg(f));
}

// ---------------------------------------------------------------- schematic helpers

void MainWindow::placeQuick(const QString &symbol)
{
    if (auto *s = activeSchematic())
        s->startPlaceSymbol(symbol);
}

void MainWindow::placeLabel(bool ground, bool com)
{
    SchematicView *s = activeSchematic();
    if (!s)
        return;
    if (ground) {
        s->startPlaceFlag(com ? QStringLiteral("COM") : QStringLiteral("0"));
        return;
    }
    QString name, io;
    if (!NetLabelDialog::edit(this, name, io))
        return;
    s->startPlaceFlag(name, io);
}

void MainWindow::placeText(bool directive)
{
    SchematicView *s = activeSchematic();
    if (!s)
        return;
    SchText t;
    t.directive = directive;
    t.just = Justify::fromString(QStringLiteral("Left"));
    if (!TextDialog::edit(this, t, true))
        return;
    s->startPlaceText(t.text, t.directive, t.fontSize, t.just);
}

void MainWindow::editAnalysis(int textIndex)
{
    SchematicView *s = activeSchematic();
    if (!s)
        return;
    Schematic &sch = s->schematicRef();
    static const QRegularExpression an(QStringLiteral("^\\.(tran|ac|dc|op|noise|tf)\\b"), QRegularExpression::CaseInsensitiveOption);
    if (textIndex < 0) {
        for (int i = 0; i < sch.texts.size(); ++i)
            if (sch.texts[i].directive && an.match(sch.texts[i].text.trimmed()).hasMatch() && !sch.texts[i].text.contains('\n')) {
                textIndex = i;
                break;
            }
    }
    AnalysisDialog dlg(textIndex >= 0 ? sch.texts[textIndex].text : QString(), this);
    if (dlg.exec() != QDialog::Accepted || dlg.command().isEmpty())
        return;
    if (textIndex >= 0) {
        // Replace the existing analysis directive in place (keep other lines).
        sch.texts[textIndex].text = dlg.command();
        sch.texts[textIndex].directive = true;
        s->commitExternalChange();
    } else {
        s->startPlaceText(dlg.command(), true, 2, Justify::fromString(QStringLiteral("Left")));
    }
}

void MainWindow::componentDialog()
{
    SchematicView *s = activeSchematic();
    if (!s)
        return;
    if (SymbolLibrary::instance().entries().size() < 60 && !AppSettings::instance().raw().value(QStringLiteral("ui/libPromptShown")).toBool()) {
        AppSettings::instance().raw().setValue(QStringLiteral("ui/libPromptShown"), true);
        if (QMessageBox::question(this, tr("Component library"),
                                  tr("Only the built-in basic symbols are available. Import the full component library from your LTspice installation?"))
            == QMessageBox::Yes) {
            LibraryImportDialog dlg(this);
            dlg.exec();
        }
    }
    ComponentBrowser dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (!dlg.exampleToOpen().isEmpty()) {
        openFile(dlg.exampleToOpen());
        return;
    }
    if (!dlg.selectedSymbol().isEmpty())
        s->startPlaceSymbol(dlg.selectedSymbol());
}

void MainWindow::viewNetlist()
{
    SchematicView *s = activeSchematic();
    if (!s)
        return;
    const NetlistResult r = s->netlist();
    auto *ed = new NetlistEditor;
    QString text = r.text;
    if (!r.errors.isEmpty() || !r.warnings.isEmpty()) {
        QStringList notes;
        for (const QString &e : r.errors)
            notes << QStringLiteral("* ERROR: ") + e;
        for (const QString &w : r.warnings)
            notes << QStringLiteral("* WARNING: ") + w;
        text = notes.join('\n') + '\n' + text;
    }
    const QString name = (s->filePath().isEmpty() ? s->property("untitledName").toString() : QFileInfo(s->filePath()).fileName());
    ed->setText(text, QFileInfo(name).completeBaseName() + QStringLiteral(".net"));
    ed->setReadOnly(true);
    addDocument(ed)->show();
}

void MainWindow::findInSchematic()
{
    SchematicView *s = activeSchematic();
    if (!s)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Find"), tr("Component reference or net name:"), QLineEdit::Normal, QString(), &ok);
    if (ok && !name.isEmpty() && !s->findAndSelect(name.trimmed()))
        statusBar()->showMessage(tr("\"%1\" not found").arg(name), 3000);
}

void MainWindow::openSubSheet(SchematicView *from, const QString &ascPath, const QString &instancePath)
{
    auto *v = new SchematicView;
    QString err;
    if (!v->load(ascPath, &err)) {
        delete v;
        QMessageBox::warning(this, tr("Open"), err);
        return;
    }
    v->setInstancePath(instancePath);
    SchematicView *root = m_subSheetRoot.value(from, from);
    if (!root)
        root = from;
    m_subSheetRoot.insert(v, root);
    if (m_sessions.contains(root) && m_sessions[root].hasResult) {
        v->setSimulationAvailable(true, m_sessions[root].last.analysis == QLatin1String("tran"));
        v->setOperatingPoint(m_sessions[root].last.operatingPoint);
    }
    QMdiSubWindow *w = addDocument(v);
    w->setWindowTitle(QStringLiteral("%1 [%2]").arg(QFileInfo(ascPath).fileName(), instancePath));
    w->showMaximized();
}

void MainWindow::openSymbolForSheet()
{
    SchematicView *s = activeSchematic();
    if (!s || s->filePath().isEmpty())
        return;
    const QFileInfo fi(s->filePath());
    const QString asy = fi.absolutePath() + '/' + fi.completeBaseName() + QStringLiteral(".asy");
    if (QFileInfo::exists(asy)) {
        openFile(asy);
        return;
    }
    if (QMessageBox::question(this, tr("Create symbol"), tr("%1 does not exist. Generate a symbol from this sheet's I/O pins?").arg(QFileInfo(asy).fileName()))
        != QMessageBox::Yes)
        return;
    QStringList in, out, bi;
    for (const NetFlag &f : s->schematic().flags) {
        const QString d = f.ioDirection.toLower();
        if (d == QLatin1String("in")) in << f.name;
        else if (d == QLatin1String("out")) out << f.name;
        else if (d == QLatin1String("bidir")) bi << f.name;
    }
    SymbolDef def = SymbolEditor::generateBlock(in, out, bi);
    def.save(asy);
    SymbolLibrary::instance().rescan();
    openFile(asy);
}

void MainWindow::createSymbolFromSubckt(const QString &name, const QStringList &ports, const QString &file)
{
    auto *e = new SymbolEditor;
    e->setSymbol(SymbolEditor::generateFromSubckt(name, ports, file));
    addDocument(e)->showMaximized();
    statusBar()->showMessage(tr("Symbol generated for %1 — save it as %1.asy next to your schematic or in a symbol search path").arg(name), 8000);
}

// ---------------------------------------------------------------- simulation

DocumentWidget *MainWindow::simulationRoot(DocumentWidget *doc) const
{
    if (!doc)
        return nullptr;
    if (auto *s = qobject_cast<SchematicView *>(doc)) {
        SchematicView *root = m_subSheetRoot.value(s);
        return root ? root : s;
    }
    if (qobject_cast<NetlistEditor *>(doc))
        return doc;
    if (auto *w = qobject_cast<WaveformView *>(doc)) {
        for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it)
            if (it->wave == w)
                return it.key();
    }
    return nullptr;
}

MainWindow::SimSession &MainWindow::sessionFor(DocumentWidget *doc)
{
    SimSession &s = m_sessions[doc];
    if (!s.controller) {
        s.source = doc;
        s.controller = new SimController(this);
        connect(s.controller, &SimController::progress, this, [this](double pct, const QString &status) {
            m_progress->setVisible(true);
            m_progress->setValue(int(pct));
            m_simLabel->setText(status.isEmpty() ? tr("Simulating...") : tr("Simulating %1").arg(status));
        });
        connect(s.controller, &SimController::started, this, [this](int runs) {
            m_progress->setVisible(true);
            m_progress->setValue(0);
            m_simLabel->setText(runs > 1 ? tr("Running %1 steps...").arg(runs) : tr("Simulating..."));
            updateActions();
        });
        connect(s.controller, &SimController::logText, this, [this, doc](const QString &t) {
            SimSession &ss = m_sessions[doc];
            if (ss.log)
                ss.log->appendLog(t);
        });
        connect(s.controller, &SimController::finished, this, [this, doc](const SimResult &r) { simulationFinished(doc, r); });
    }
    return s;
}

void MainWindow::runOrPause()
{
    DocumentWidget *root = simulationRoot(activeDoc());
    if (!root) {
        statusBar()->showMessage(tr("Open a schematic or netlist to simulate"), 3000);
        return;
    }
    SimSession &s = sessionFor(root);
    if (s.controller->isRunning()) {
        if (s.controller->isPaused()) {
            s.controller->resume();
            m_simLabel->setText(tr("Simulating..."));
        } else {
            s.controller->pause();
            m_simLabel->setText(tr("Paused (Alt+R to resume)"));
        }
        return;
    }
    startSimulation(root);
}

void MainWindow::stopSimulation()
{
    DocumentWidget *root = simulationRoot(activeDoc());
    if (root && m_sessions.contains(root) && m_sessions[root].controller)
        m_sessions[root].controller->stop();
}

void MainWindow::startSimulation(DocumentWidget *doc)
{
    QStringList netlist;
    QString baseDir, outputBase;
    if (auto *s = qobject_cast<SchematicView *>(doc)) {
        const NetlistResult r = s->netlist();
        if (!r.errors.isEmpty()) {
            QMessageBox::warning(this, tr("Netlist errors"), r.errors.join('\n'));
            return;
        }
        netlist = r.text.split('\n');
        baseDir = s->schematic().directory();
        if (!s->filePath().isEmpty() && AppSettings::instance().keepRawFiles()) {
            const QFileInfo fi(s->filePath());
            outputBase = fi.absolutePath() + '/' + fi.completeBaseName();
        }
        if (baseDir.isEmpty())
            baseDir = QDir::homePath();
    } else if (auto *n = qobject_cast<NetlistEditor *>(doc)) {
        netlist = n->lines();
        if (!n->filePath().isEmpty()) {
            const QFileInfo fi(n->filePath());
            baseDir = fi.absolutePath();
            if (AppSettings::instance().keepRawFiles())
                outputBase = fi.absolutePath() + '/' + fi.completeBaseName();
        }
    } else {
        return;
    }
    SimSession &s = sessionFor(doc);
    if (!s.log) {
        s.log = new LogWindow;
        s.log->setTitle(tr("SPICE Output Log — %1").arg(doc->displayName()));
    }
    s.log->setLog(QString());
    m_lastLog = s.log;
    s.controller->start(netlist, baseDir, outputBase);
    updateActions();
}

WaveformView *MainWindow::ensureWaveform(SimSession &s, DocumentWidget *source)
{
    if (s.wave)
        return s.wave;
    auto *w = new WaveformView;
    s.wave = w;
    QMdiSubWindow *ww = addDocument(w);
    // Tile like LTspice: waveform above the schematic.
    QMdiSubWindow *sw = nullptr;
    for (QMdiSubWindow *x : m_mdi->subWindowList())
        if (x->widget() == source)
            sw = x;
    if (m_mdi->viewMode() == QMdiArea::SubWindowView && sw)
        tileForSimulation(ww, sw);
    else
        ww->showMaximized();
    return w;
}

void MainWindow::tileForSimulation(QMdiSubWindow *top, QMdiSubWindow *bottom)
{
    const QSize vs = m_mdi->viewport()->size();
    top->showNormal();
    bottom->showNormal();
    top->setGeometry(0, 0, vs.width(), vs.height() / 2);
    bottom->setGeometry(0, vs.height() / 2, vs.width(), vs.height() - vs.height() / 2);
}

void MainWindow::simulationFinished(DocumentWidget *source, const SimResult &r)
{
    m_progress->setVisible(false);
    m_simLabel->setText(r.ok ? tr("Completed in %1 s").arg(r.elapsed, 0, 'f', 2) : (r.aborted ? tr("Stopped") : tr("Failed")));
    SimSession &s = m_sessions[source];
    if (s.log)
        s.log->setLog(r.log);
    updateActions();
    if (!r.ok) {
        QString msg = r.errors.join('\n');
        if (msg.isEmpty())
            msg = tr("The simulation failed. See the SPICE Output Log for details.");
        // Show the relevant tail of the log in the message.
        QStringList errLines;
        for (const QString &l : r.log.split('\n'))
            if (l.contains(QLatin1String("rror")) || l.contains(QLatin1String("fail")) || l.contains(QLatin1String("Fatal")))
                errLines << l;
        if (!errLines.isEmpty())
            msg += QStringLiteral("\n\n") + errLines.mid(0, 12).join('\n');
        QMessageBox box(QMessageBox::Warning, tr("Simulation error"), msg, QMessageBox::Ok, this);
        QPushButton *logBtn = box.addButton(tr("Show Log"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == reinterpret_cast<QAbstractButton *>(logBtn))
            showLog();
        return;
    }
    s.last = r;
    s.hasResult = true;
    // Update schematic (and open sub-sheets) for probing.
    auto applyTo = [&](SchematicView *v) {
        v->setSimulationAvailable(r.data && r.data->kind != Dataset::Kind::OperatingPoint, r.analysis == QLatin1String("tran"));
        v->setOperatingPoint(r.operatingPoint);
    };
    if (auto *sch = qobject_cast<SchematicView *>(source)) {
        applyTo(sch);
        for (auto it = m_subSheetRoot.constBegin(); it != m_subSheetRoot.constEnd(); ++it)
            if (it.value() == sch && it.key())
                applyTo(it.key());
    }
    if (!r.data) {
        updateActions();
        return;
    }
    if (r.data->kind == Dataset::Kind::OperatingPoint || r.data->kind == Dataset::Kind::TransferFunction) {
        dialogs::showOperatingPoint(this, r.operatingPoint, tr("%1 — %2").arg(r.data->kind == Dataset::Kind::TransferFunction ? tr("Transfer Function") : tr("Operating Point"),
                                                                             source->displayName()));
        updateActions();
        return;
    }
    const bool fresh = !s.wave;
    WaveformView *w = ensureWaveform(s, source);
    if (auto *sch = qobject_cast<SchematicView *>(source))
        w->setSourceSchematic(sch->filePath());
    if (fresh) {
        w->setDataset(r.data, r.rawPath, QFileInfo(r.rawPath.isEmpty() ? source->displayName() : r.rawPath).completeBaseName() + QStringLiteral(".raw"));
        if (!w->loadPlotSettings(w->defaultPlotSettingsPath())) {
            // Netlists: plot the first node like LTspice's default when no .plt exists.
            if (qobject_cast<NetlistEditor *>(source) && r.data->vars.size() > 1)
                w->addTrace(r.data->vars[1].name);
        }
    } else {
        w->updateDataset(r.data);
    }
    // .fra: a Bode plot per analyzer, with LTspice's automatic trace when there is one.
    for (int i = 0; i < r.fraData.size(); ++i) {
        const DatasetPtr &ds = r.fraData[i];
        WaveformView *fv = s.fraViews.value(i);
        const QString title = QFileInfo(r.rawPath.isEmpty() ? source->displayName() : r.rawPath).completeBaseName()
            + QStringLiteral(".fra_%1.raw").arg(ds->vars.value(1).name.section('_', 1));
        if (!fv) {
            fv = new WaveformView;
            fv->setDataset(ds, QString(), title);
            const QString trace = r.fraAutoPlot.value(i);
            if (!trace.isEmpty())
                fv->addTrace(trace);
            addDocument(fv)->showMaximized();
            while (s.fraViews.size() <= i)
                s.fraViews << QPointer<WaveformView>();
            s.fraViews[i] = fv;
        } else {
            fv->updateDataset(ds);
        }
    }
    if (!r.measLines.isEmpty())
        statusBar()->showMessage(tr(".meas results are in the SPICE Output Log (Ctrl+L)"), 5000);
    updateActions();
}

void MainWindow::showLog()
{
    DocumentWidget *root = simulationRoot(activeDoc());
    QPointer<LogWindow> log = root && m_sessions.contains(root) ? m_sessions[root].log : m_lastLog;
    if (!log) {
        statusBar()->showMessage(tr("No simulation log yet"), 3000);
        return;
    }
    for (QMdiSubWindow *w : m_mdi->subWindowList()) {
        if (w->widget() == log) {
            m_mdi->setActiveSubWindow(w);
            return;
        }
    }
    QMdiSubWindow *w = addDocument(log);
    w->resize(700, 500);
    w->show();
    // The log window is owned by the session; do not delete it when closed.
    w->setAttribute(Qt::WA_DeleteOnClose, false);
    connect(w, &QMdiSubWindow::windowStateChanged, this, []() {});
}

void MainWindow::handleProbe(SchematicView *view, const QString &expr, bool append)
{
    Q_UNUSED(append);
    DocumentWidget *root = simulationRoot(view);
    if (!root || !m_sessions.contains(root) || !m_sessions[root].hasResult)
        return;
    SimSession &s = m_sessions[root];
    WaveformView *w = ensureWaveform(s, root);
    if (!w->dataset())
        w->setDataset(s.last.data, s.last.rawPath);
    w->addTrace(expr);
    statusBar()->showMessage(tr("Plotted %1").arg(expr), 2000);
}

void MainWindow::addTraces()
{
    WaveformView *w = activeWaveform();
    if (!w || !w->dataset())
        return;
    AddTraceDialog dlg(w->availableVectors(), this);
    if (dlg.exec() == QDialog::Accepted)
        w->addTraces(dlg.expressions());
}

bool MainWindow::isSimulating() const
{
    for (const SimSession &s : m_sessions)
        if (s.controller && s.controller->isRunning())
            return true;
    return false;
}

void MainWindow::plotOnActiveWaveform(const QString &expr)
{
    for (QMdiSubWindow *w : m_mdi->subWindowList()) {
        if (auto *wv = qobject_cast<WaveformView *>(w->widget())) {
            if (expr.startsWith(QLatin1String("pane:"))) {
                wv->addPane(false);
                wv->addTrace(expr.mid(5));
            } else {
                wv->addTrace(expr);
            }
            return;
        }
    }
}

void MainWindow::activateDocument(int index)
{
    const auto list = m_mdi->subWindowList();
    if (index >= 0 && index < list.size())
        m_mdi->setActiveSubWindow(list[index]);
}

QWidget *MainWindow::createDialogForTest(const QString &name)
{
    SchematicView *s = activeSchematic();
    auto firstWithPrefix = [s](QChar p) -> int {
        if (!s)
            return -1;
        for (int i = 0; i < s->schematic().symbols.size(); ++i)
            if (s->schematic().symbols[i].attr(QStringLiteral("Prefix")).startsWith(p, Qt::CaseInsensitive))
                return i;
        return -1;
    };
    QDialog *d = nullptr;
    if (name == QLatin1String("component"))
        d = new ComponentBrowser(this);
    else if (name == QLatin1String("analysis"))
        d = new AnalysisDialog(QStringLiteral(".tran 0 10m 0 1u startup"), this);
    else if (name == QLatin1String("settings"))
        d = new SettingsDialog(this);
    else if (name == QLatin1String("addtrace") && activeWaveform())
        d = new AddTraceDialog(activeWaveform()->availableVectors(), this);
    else if (name == QLatin1String("source") && firstWithPrefix('V') >= 0)
        d = new SourceDialog(s->schematic().symbols[firstWithPrefix('V')], this);
    else if (name == QLatin1String("passive") && firstWithPrefix('C') >= 0)
        d = new PassiveDialog(s->schematic().symbols[firstWithPrefix('C')], this);
    else if (name == QLatin1String("model") && firstWithPrefix('Q') >= 0)
        d = new ModelSelectDialog(s->schematic().symbols[firstWithPrefix('Q')], this);
    else if (name == QLatin1String("attributes") && s && !s->schematic().symbols.isEmpty())
        d = new AttributeDialog(s->schematic().symbols.first(), this);
    if (d)
        d->show();
    return d;
}

void MainWindow::firstRunCheck()
{
    // Load user-defined waveform functions.
    const QString defs = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/plot.defs");
    for (const QString &l : textio::splitLines(textio::readText(defs)))
        Expression::defineFromDirective(l);
    if (QFileInfo(AppSettings::instance().libraryRoot() + QStringLiteral("/sym")).isDir())
        return;
    if (AppSettings::instance().raw().value(QStringLiteral("ui/firstRunDone")).toBool())
        return;
    AppSettings::instance().raw().setValue(QStringLiteral("ui/firstRunDone"), true);
    const QStringList cands = LibraryImportDialog::detectCandidates();
    const QString msg = cands.isEmpty()
        ? tr("Welcome to LinSpice!\n\nOnly basic built-in symbols are available. To use the full component library, import it from your "
             "LTspice installation (lib.zip / examples.zip). Do this now?")
        : tr("Welcome to LinSpice!\n\nAn LTspice library was found at:\n%1\n\nImport it now?").arg(cands.first());
    if (QMessageBox::question(this, tr("Component library"), msg) == QMessageBox::Yes) {
        LibraryImportDialog dlg(this);
        dlg.exec();
    }
}
