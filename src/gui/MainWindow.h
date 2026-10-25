#pragma once

#include "sim/SimController.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class DocumentWidget;
class LogWindow;
class NetlistEditor;
class QLabel;
class QMdiArea;
class QMdiSubWindow;
class QMenu;
class QProgressBar;
class QToolBar;
class SchematicView;
class SymbolEditor;
class WaveformView;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString &path);
    void firstRunCheck();

    // Automation hooks (used by --screenshot for visual testing).
    void runActiveSimulation() { runOrPause(); }
    bool isSimulating() const;
    void plotOnActiveWaveform(const QString &expr);
    void activateDocument(int index);

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    struct SimSession {
        QPointer<DocumentWidget> source; // schematic or netlist
        SimController *controller = nullptr;
        QPointer<WaveformView> wave;
        QPointer<LogWindow> log;
        SimResult last;
        bool hasResult = false;
    };

    // Construction
    void createActions();
    void createMenus();
    void createToolBars();
    QAction *act(const QString &id, const QString &text, const QKeySequence &key, const std::function<void()> &fn,
                 const QString &icon = QString());
    QAction *action(const QString &id) const { return m_actions.value(id); }

    // Documents
    QMdiSubWindow *addDocument(DocumentWidget *doc);
    DocumentWidget *activeDoc() const;
    SchematicView *activeSchematic() const;
    WaveformView *activeWaveform() const;
    SymbolEditor *activeSymbol() const;
    NetlistEditor *activeNetlist() const;
    QMdiSubWindow *findWindowFor(const QString &path) const;
    void updateActions();
    void updateWindowTitle(DocumentWidget *doc);
    bool maybeSave(DocumentWidget *doc);
    void newSchematic();
    void newSymbol();
    void openDialog();
    void openExamples();
    bool saveDoc(DocumentWidget *doc, bool forceDialog);
    void closeActive();
    void updateRecentMenu();
    void printDoc(bool preview);
    void exportImage();

    // Schematic helpers
    void placeQuick(const QString &symbol);
    void placeLabel(bool ground, bool com = false);
    void placeText(bool directive);
    void editAnalysis(int textIndex = -1);
    void componentDialog();
    void viewNetlist();
    void findInSchematic();
    void openSubSheet(SchematicView *from, const QString &ascPath, const QString &instancePath);
    void openSymbolForSheet();
    void createSymbolFromSubckt(const QString &name, const QStringList &ports, const QString &file);

    // Simulation
    SimSession &sessionFor(DocumentWidget *doc);
    DocumentWidget *simulationRoot(DocumentWidget *doc) const;
    void runOrPause();
    void stopSimulation();
    void startSimulation(DocumentWidget *doc);
    void simulationFinished(DocumentWidget *source, const SimResult &r);
    void showLog();
    WaveformView *ensureWaveform(SimSession &s, DocumentWidget *source);
    void handleProbe(SchematicView *view, const QString &expr, bool append);
    void openWaveformFile(const QString &path);
    void tileForSimulation(QMdiSubWindow *top, QMdiSubWindow *bottom);

    // Waveform helpers
    void addTraces();

    QMdiArea *m_mdi = nullptr;
    QHash<QString, QAction *> m_actions;
    QMenu *m_editMenu = nullptr, *m_plotMenu = nullptr, *m_hierMenu = nullptr, *m_simMenu = nullptr, *m_recentMenu = nullptr,
          *m_windowMenu = nullptr, *m_viewMenu = nullptr, *m_symMenu = nullptr;
    QToolBar *m_mainBar = nullptr, *m_editBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_simLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QHash<DocumentWidget *, SimSession> m_sessions;
    QHash<SchematicView *, QPointer<SchematicView>> m_subSheetRoot; // sub-sheet -> top schematic
    QPointer<LogWindow> m_lastLog;
    int m_untitled = 0;
};
