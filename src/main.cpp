#include "core/AppSettings.h"
#include "core/Expression.h"
#include "core/Measure.h"
#include "core/Netlister.h"
#include "core/Schematic.h"
#include "core/TextIO.h"
#include "gui/MainWindow.h"
#include "gui/SchematicView.h"
#include "sim/SimController.h"
#include "sim/SimWorker.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>
#include <cstring>

// Headless batch simulation, like "LTspice -b": writes <name>.raw/.log/.net
// next to the input file and exits with 0 on success.
static int runBatch(const QString &path)
{
    QStringList netlist;
    QString baseDir = QFileInfo(path).absolutePath();
    if (path.endsWith(QLatin1String(".asc"), Qt::CaseInsensitive)) {
        Schematic sch;
        QString err;
        if (!sch.load(path, &err)) {
            std::fprintf(stderr, "Cannot load %s: %s\n", qPrintable(path), qPrintable(err));
            return 2;
        }
        const NetlistResult r = Netlister(sch).build();
        for (const QString &e : r.errors)
            std::fprintf(stderr, "Netlist error: %s\n", qPrintable(e));
        if (!r.errors.isEmpty())
            return 2;
        netlist = r.text.split('\n');
    } else {
        bool ok = false;
        netlist = textio::splitLines(textio::readText(path, &ok));
        if (!ok) {
            std::fprintf(stderr, "Cannot read %s\n", qPrintable(path));
            return 2;
        }
    }
    const QFileInfo fi(path);
    SimController ctl;
    int rc = 1;
    QObject::connect(&ctl, &SimController::finished, [&](const SimResult &r) {
        std::fputs(qPrintable(r.log), stdout);
        for (const QString &e : r.errors)
            std::fprintf(stderr, "%s\n", qPrintable(e));
        rc = r.ok ? 0 : 1;
        QCoreApplication::quit();
    });
    QTimer::singleShot(0, [&]() { ctl.start(netlist, baseDir, fi.absolutePath() + '/' + fi.completeBaseName()); });
    QCoreApplication::exec();
    return rc;
}

// Testing aid: simulates a schematic, then checks that every probe a click in the
// schematic would produce (pin currents, net voltages) names a signal in the results.
static int runProbeAudit(const QString &path)
{
    Schematic sch;
    QString err;
    if (!sch.load(path, &err)) {
        std::fprintf(stderr, "Cannot load %s: %s\n", qPrintable(path), qPrintable(err));
        return 2;
    }
    const NetlistResult nr = Netlister(sch).build();
    if (!nr.errors.isEmpty())
        return 2;
    const QFileInfo fi(path);
    SimController ctl;
    SimResult result;
    QObject::connect(&ctl, &SimController::finished, [&](const SimResult &r) {
        result = r;
        QCoreApplication::quit();
    });
    QTimer::singleShot(0, [&]() { ctl.start(nr.text.split('\n'), fi.absolutePath(), fi.absolutePath() + '/' + fi.completeBaseName()); });
    QCoreApplication::exec();
    if (!result.data) {
        std::fprintf(stderr, "%s: simulation failed\n", qPrintable(path));
        return 1;
    }
    SchematicView view;
    if (!view.load(path, &err))
        return 2;
    view.setPinProbes(result.pinProbes);
    QSet<QString> names;
    for (const auto &v : result.data->vars)
        names.insert(v.name.toLower());
    view.setProbeContext(result.analysis, names);
    int bad = 0, total = 0;
    for (const auto &[what, expr] : view.allProbeExpressions()) {
        ++total;
        DatasetContext ctx(*result.data, 0);
        Expression e(expr);
        if (!e.evaluate(ctx)) {
            ++bad;
            std::printf("UNRESOLVED %s: %s  (%s)\n", qPrintable(fi.fileName()), qPrintable(expr), qPrintable(what));
        }
    }
    std::printf("%s: %d probes, %d unresolved\n", qPrintable(fi.fileName()), total, bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    // Simulation worker processes re-enter here.
    if (argc >= 2 && std::strcmp(argv[1], "--sim-worker") == 0)
        return runSimWorker(argc, argv);

    Q_INIT_RESOURCE(resources);

    // Batch mode does not need a GUI.
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-b") == 0 || std::strcmp(argv[i], "--batch") == 0) && i + 1 < argc) {
            QCoreApplication app(argc, argv);
            app.setApplicationName(QStringLiteral("LinSpice"));
            app.setOrganizationName(QStringLiteral("LinSpice"));
            return runBatch(QString::fromLocal8Bit(argv[i + 1]));
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LinSpice"));
    app.setOrganizationName(QStringLiteral("LinSpice"));
    if (argc >= 3 && std::strcmp(argv[1], "--probe-audit") == 0)
        return runProbeAudit(QString::fromLocal8Bit(argv[2]));
    app.setApplicationVersion(QStringLiteral(LINSPICE_VERSION));
    app.setDesktopFileName(QStringLiteral("linspice"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("LinSpice circuit simulator (LTspice-compatible)"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({{QStringLiteral("b"), QStringLiteral("batch")}, QStringLiteral("Run a schematic or netlist in batch mode."), QStringLiteral("file")});
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Schematics, symbols, netlists or waveform files to open."));
    // Hidden automation options for visual testing.
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot and exit."), QStringLiteral("png"));
    QCommandLineOption runOpt(QStringLiteral("run"), QStringLiteral("Run the active schematic before the screenshot."));
    QCommandLineOption probeOpt(QStringLiteral("probe"), QStringLiteral("Trace to plot (repeatable)."), QStringLiteral("expr"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size WxH."), QStringLiteral("size"));
    QCommandLineOption dialogOpt(QStringLiteral("dialog"), QStringLiteral("Open a dialog for the screenshot."), QStringLiteral("name"));
    dialogOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    shotOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    runOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    probeOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    sizeOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({shotOpt, runOpt, probeOpt, sizeOpt, dialogOpt});
    parser.process(app);

    MainWindow w;
    if (parser.isSet(sizeOpt)) {
        const QStringList wh = parser.value(sizeOpt).split('x');
        w.resize(wh.value(0).toInt(), wh.value(1).toInt());
    }
    w.show();
    const QStringList files = parser.positionalArguments();
    for (const QString &f : files)
        w.openFile(f);
    if (parser.isSet(shotOpt)) {
        const QString out = parser.value(shotOpt);
        const bool run = parser.isSet(runOpt);
        const QStringList probes = parser.values(probeOpt);
        const QString dialogName = parser.value(dialogOpt);
        QWidget *dialog = nullptr;
        auto *timer = new QTimer(&w);
        int stage = 0;
        int waited = 0;
        QObject::connect(timer, &QTimer::timeout, &w, [&, timer]() mutable {
            waited += 100;
            if (stage == 0) {
                if (run)
                    w.runActiveSimulation();
                stage = 1;
                return;
            }
            if (stage == 1) {
                if (run && w.isSimulating() && waited < 60000)
                    return;
                for (const QString &p : probes)
                    w.plotOnActiveWaveform(p);
                stage = 2;
                return;
            }
            if (stage == 2) {
                stage = 3;
                if (!dialogName.isEmpty())
                    dialog = w.createDialogForTest(dialogName);
                return; // let layouts settle
            }
            if (dialog)
                dialog->grab().save(out);
            else
                w.grab().save(out);
            timer->stop();
            QCoreApplication::quit();
        });
        timer->start(100);
    } else {
        QTimer::singleShot(200, &w, [&w]() { w.firstRunCheck(); });
    }
    return app.exec();
}
