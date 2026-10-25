#include "MiscDialogs.h"

#include "core/AppSettings.h"
#include "core/DirCache.h"
#include "core/ModelLibrary.h"
#include "core/Netlister.h"
#include "core/SymbolLibrary.h"
#include "core/Units.h"
#include "core/ZipReader.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

// ---------------------------------------------------------------- LibraryImportDialog

QStringList LibraryImportDialog::detectCandidates()
{
    QStringList out;
    const QString home = QDir::homePath();
    QStringList prefixes{home + QStringLiteral("/.wine")};
    const QString winePrefix = qEnvironmentVariable("WINEPREFIX");
    if (!winePrefix.isEmpty())
        prefixes.prepend(winePrefix);
    for (const QString &prefix : prefixes) {
        const QString users = prefix + QStringLiteral("/drive_c/users");
        for (const QString &u : QDir(users).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const char *rel : {"/AppData/Local/LTspice", "/Local Settings/Application Data/LTspice", "/Documents/LTspiceXVII",
                                    "/My Documents/LTspiceXVII"}) {
                const QString d = users + '/' + u + QLatin1String(rel);
                if (QFileInfo(d + QStringLiteral("/lib/sym")).isDir())
                    out << d;
            }
        }
        for (const char *rel : {"/drive_c/Program Files/ADI/LTspice", "/drive_c/Program Files/LTC/LTspiceXVII"}) {
            const QString d = prefix + QLatin1String(rel);
            if (QFileInfo::exists(d + QStringLiteral("/lib.zip")))
                out << d;
        }
    }
    // Folders next to the application or in the user's documents that hold lib.zip.
    for (const QString &d : {QDir::currentPath(), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
                             QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)}) {
        QDirIterator it(d, {QStringLiteral("lib.zip")}, QDir::Files, QDirIterator::Subdirectories);
        int guard = 0;
        while (it.hasNext() && guard++ < 2000) {
            const QString p = it.next();
            if (QFileInfo::exists(QFileInfo(p).absolutePath() + QStringLiteral("/examples.zip")) || p.contains(QLatin1String("LTspice"), Qt::CaseInsensitive))
                out << QFileInfo(p).absolutePath();
            if (out.size() > 10)
                break;
        }
    }
    out.removeDuplicates();
    return out;
}

LibraryImportDialog::LibraryImportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Import Component Library"));
    resize(640, 280);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("LinSpice uses LTspice-compatible symbol and model libraries. Point it at your LTspice installation "
                                "folder (containing lib.zip and examples.zip), an extracted lib directory (with sym/ sub/ cmp/), or a "
                                "Wine LTspice data folder. Files are copied to %1.")
                                 .arg(QFileInfo(AppSettings::instance().defaultLibraryRoot()).absolutePath()));
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *row = new QHBoxLayout;
    m_source = new QLineEdit;
    const QStringList cands = detectCandidates();
    if (!cands.isEmpty())
        m_source->setText(cands.first());
    auto *browse = new QPushButton(tr("Browse..."));
    row->addWidget(new QLabel(tr("Source:")));
    row->addWidget(m_source, 1);
    row->addWidget(browse);
    lay->addLayout(row);
    if (cands.size() > 1) {
        auto *combo = new QComboBox;
        combo->addItems(cands);
        lay->addWidget(combo);
        connect(combo, &QComboBox::currentTextChanged, m_source, &QLineEdit::setText);
    }
    m_examples = new QCheckBox(tr("Also import example circuits"));
    m_examples->setChecked(true);
    lay->addWidget(m_examples);
    m_progress = new QProgressBar;
    m_progress->setVisible(false);
    lay->addWidget(m_progress);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    lay->addStretch();
    auto *bb = new QDialogButtonBox;
    auto *go = bb->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Close);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(this, tr("LTspice folder"), m_source->text());
        if (!d.isEmpty())
            m_source->setText(d);
    });
    connect(go, &QPushButton::clicked, this, &LibraryImportDialog::runImport);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

void LibraryImportDialog::runImport()
{
    const QString src = m_source->text().trimmed();
    if (src.isEmpty() || !QFileInfo::exists(src)) {
        m_status->setText(tr("Source folder does not exist."));
        return;
    }
    AppSettings &s = AppSettings::instance();
    const QString dataDir = QFileInfo(s.defaultLibraryRoot()).absolutePath();
    m_progress->setVisible(true);
    auto progress = [this](int done, int total) {
        m_progress->setMaximum(total);
        m_progress->setValue(done);
        QApplication::processEvents();
        return true;
    };
    bool imported = false;
    // Case 1: an already extracted library directory.
    QString libDir;
    if (QFileInfo(src + QStringLiteral("/sym")).isDir())
        libDir = src;
    else if (QFileInfo(src + QStringLiteral("/lib/sym")).isDir())
        libDir = src + QStringLiteral("/lib");
    if (!libDir.isEmpty()) {
        s.setLibraryRoot(libDir);
        m_status->setText(tr("Using library at %1").arg(libDir));
        imported = true;
        const QString ex = QFileInfo(libDir).absolutePath() + QStringLiteral("/examples");
        if (QFileInfo(ex).isDir())
            s.setExamplesRoot(ex);
    }
    // Case 2: zip archives.
    const QString libZip = src + QStringLiteral("/lib.zip");
    if (!imported && QFileInfo::exists(libZip)) {
        m_status->setText(tr("Extracting component library..."));
        QApplication::processEvents();
        ZipReader zip(libZip);
        if (!zip.isValid()) {
            m_status->setText(tr("Cannot read %1: %2").arg(libZip, zip.error()));
            return;
        }
        // Archives contain a top-level "lib/" folder.
        if (!zip.extractAll(dataDir, QString(), progress)) {
            m_status->setText(tr("Extraction failed: %1").arg(zip.error()));
            return;
        }
        s.setLibraryRoot(dataDir + QStringLiteral("/lib"));
        imported = true;
    }
    const QString exZip = src + QStringLiteral("/examples.zip");
    if (m_examples->isChecked() && QFileInfo::exists(exZip)) {
        m_status->setText(tr("Extracting examples..."));
        QApplication::processEvents();
        ZipReader zip(exZip);
        if (zip.isValid() && zip.extractAll(dataDir, QString(), progress)) {
            s.setExamplesRoot(dataDir + QStringLiteral("/examples"));
            imported = true;
        }
    }
    m_progress->setVisible(false);
    if (!imported) {
        m_status->setText(tr("No LTspice library found in %1 (expected lib.zip or a sym/ directory).").arg(src));
        return;
    }
    DirCache::clear();
    SymbolLibrary::instance().rescan();
    ModelLibrary::instance().clearCache();
    const int count = SymbolLibrary::instance().entries().size();
    m_status->setText(tr("Done. %1 symbols available.").arg(count));
    QMessageBox::information(this, tr("Import complete"), tr("The component library was imported (%1 symbols).").arg(count));
    accept();
}

// ---------------------------------------------------------------- misc

namespace dialogs {

void showOperatingPoint(QWidget *parent, const QHash<QString, double> &op, const QString &title)
{
    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(title);
    dlg->resize(480, 560);
    auto *lay = new QVBoxLayout(dlg);
    lay->addWidget(new QLabel(QObject::tr("--- Operating Point ---")));
    auto *table = new QTableWidget(0, 3);
    table->setHorizontalHeaderLabels({QObject::tr("Quantity"), QObject::tr("Value"), QObject::tr("Type")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    QStringList keys = op.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
        const bool av = a.startsWith(QLatin1String("V(")), bv = b.startsWith(QLatin1String("V("));
        if (av != bv)
            return av;
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    for (const QString &k : keys) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(k));
        table->setItem(r, 1, new QTableWidgetItem(QString::number(op.value(k), 'g', 9)));
        const bool v = k.startsWith(QLatin1String("V("));
        table->setItem(r, 2, new QTableWidgetItem(v ? QObject::tr("voltage") : (k.startsWith('I') ? QObject::tr("device_current") : QString())));
    }
    table->resizeColumnsToContents();
    lay->addWidget(table);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    lay->addWidget(bb);
    dlg->show();
}

void showBillOfMaterials(QWidget *parent, const Schematic &sch)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Bill of Materials"));
    dlg.resize(720, 480);
    auto *lay = new QVBoxLayout(&dlg);
    auto *table = new QTableWidget(0, 5);
    table->setHorizontalHeaderLabels({QObject::tr("Qty"), QObject::tr("Reference"), QObject::tr("Value"), QObject::tr("Symbol"), QObject::tr("Description")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    struct Row {
        QStringList refs;
        QString value, symbol, desc;
    };
    QList<Row> rows;
    for (const SymbolInstance &s : sch.symbols) {
        if (s.attr(QStringLiteral("Prefix")).isEmpty())
            continue;
        const QString value = (s.attr(QStringLiteral("Value")) + ' ' + s.attr(QStringLiteral("Value2"))).trimmed();
        bool merged = false;
        for (Row &r : rows) {
            if (r.value == value && r.symbol == s.symbolName) {
                r.refs << s.instName();
                merged = true;
                break;
            }
        }
        if (!merged)
            rows << Row{{s.instName()}, value, s.symbolName, s.attr(QStringLiteral("Description"))};
    }
    for (const Row &r : rows) {
        const int i = table->rowCount();
        table->insertRow(i);
        table->setItem(i, 0, new QTableWidgetItem(QString::number(r.refs.size())));
        table->setItem(i, 1, new QTableWidgetItem(r.refs.join(QStringLiteral(", "))));
        table->setItem(i, 2, new QTableWidgetItem(r.value));
        table->setItem(i, 3, new QTableWidgetItem(r.symbol));
        table->setItem(i, 4, new QTableWidgetItem(r.desc));
    }
    table->resizeColumnsToContents();
    lay->addWidget(table);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *save = bb->addButton(QObject::tr("Export CSV..."), QDialogButtonBox::ActionRole);
    QObject::connect(save, &QPushButton::clicked, &dlg, [&]() {
        const QString path = QFileDialog::getSaveFileName(&dlg, QObject::tr("Export BOM"), QFileInfo(sch.filePath).completeBaseName() + QStringLiteral("_bom.csv"),
                                                          QObject::tr("CSV (*.csv)"));
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return;
        QTextStream ts(&f);
        ts << "Qty,Reference,Value,Symbol,Description\n";
        for (const Row &r : rows) {
            auto q = [](QString s) { return '"' + s.replace('"', QStringLiteral("\"\"")) + '"'; };
            ts << r.refs.size() << ',' << q(r.refs.join(' ')) << ',' << q(r.value) << ',' << q(r.symbol) << ',' << q(r.desc) << '\n';
        }
    });
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    dlg.exec();
}

void exportPcbNetlist(QWidget *parent, const Schematic &sch)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Export Netlist for PCB Layout"));
    auto *lay = new QFormLayout(&dlg);
    auto *fmt = new QComboBox;
    fmt->addItems({QStringLiteral("Wire List"), QStringLiteral("PADS"), QStringLiteral("Tango"), QStringLiteral("Allegro (Telesis)"),
                   QStringLiteral("Calay"), QStringLiteral("KiCad (legacy .net)"), QStringLiteral("SPICE")});
    lay->addRow(QObject::tr("Format:"), fmt);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString base = QFileInfo(sch.filePath).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(parent, QObject::tr("Save netlist"), base + QStringLiteral(".net"));
    if (path.isEmpty())
        return;
    const Connectivity c = Netlister::connectivity(sch);
    // net -> list of (ref, pin number)
    QMap<QString, QList<QPair<QString, int>>> nets;
    for (int i = 0; i < sch.symbols.size(); ++i) {
        const SymbolInstance &s = sch.symbols[i];
        if (!s.def || s.attr(QStringLiteral("Prefix")).isEmpty())
            continue;
        const QStringList pinNets = c.symbolPinNets.value(i);
        for (int k = 0; k < s.def->pins.size() && k < pinNets.size(); ++k)
            nets[pinNets[k]] << qMakePair(s.instName(), s.def->pins[k].spiceOrder);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream ts(&f);
    const QString format = fmt->currentText();
    if (format == QLatin1String("SPICE")) {
        ts << Netlister(sch).build().text;
    } else if (format == QLatin1String("PADS")) {
        ts << "*PADS-PCB*\n*PART*\n";
        for (const SymbolInstance &s : sch.symbols)
            if (!s.attr(QStringLiteral("Prefix")).isEmpty())
                ts << s.instName() << ' ' << QString(s.symbolName).replace('\\', '_') << '\n';
        ts << "*NET*\n";
        for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
            ts << "*SIGNAL* " << it.key() << '\n';
            QStringList nodes;
            for (const auto &n : it.value())
                nodes << QStringLiteral("%1.%2").arg(n.first).arg(n.second);
            ts << nodes.join(' ') << '\n';
        }
        ts << "*END*\n";
    } else if (format == QLatin1String("Tango")) {
        for (const SymbolInstance &s : sch.symbols)
            if (!s.attr(QStringLiteral("Prefix")).isEmpty())
                ts << "[\n" << s.instName() << '\n' << QString(s.symbolName).replace('\\', '_') << '\n' << s.attr(QStringLiteral("Value")) << "\n\n\n\n]\n";
        for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
            ts << "(\n" << it.key() << '\n';
            for (const auto &n : it.value())
                ts << n.first << '-' << n.second << '\n';
            ts << ")\n";
        }
    } else if (format.startsWith(QLatin1String("Allegro"))) {
        ts << "$PACKAGES\n";
        for (const SymbolInstance &s : sch.symbols)
            if (!s.attr(QStringLiteral("Prefix")).isEmpty())
                ts << QString(s.symbolName).replace('\\', '_') << "! " << s.attr(QStringLiteral("Value")) << "; " << s.instName() << '\n';
        ts << "$NETS\n";
        for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
            QStringList nodes;
            for (const auto &n : it.value())
                nodes << QStringLiteral("%1.%2").arg(n.first).arg(n.second);
            ts << it.key() << "; " << nodes.join(' ') << '\n';
        }
        ts << "$END\n";
    } else if (format == QLatin1String("Calay")) {
        for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
            ts << '/' << it.key() << ' ';
            QStringList nodes;
            for (const auto &n : it.value())
                nodes << QStringLiteral("%1(%2)").arg(n.first).arg(n.second);
            ts << nodes.join(' ') << ";\n";
        }
    } else if (format.startsWith(QLatin1String("KiCad"))) {
        ts << "(export (version D)\n  (components\n";
        for (const SymbolInstance &s : sch.symbols)
            if (!s.attr(QStringLiteral("Prefix")).isEmpty())
                ts << "    (comp (ref " << s.instName() << ") (value \"" << s.attr(QStringLiteral("Value")) << "\"))\n";
        ts << "  )\n  (nets\n";
        int code = 1;
        for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
            ts << "    (net (code " << code++ << ") (name \"" << it.key() << "\")\n";
            for (const auto &n : it.value())
                ts << "      (node (ref " << n.first << ") (pin " << n.second << "))\n";
            ts << "    )\n";
        }
        ts << "  )\n)\n";
    } else {
        // Wire list
        for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
            ts << it.key() << ":";
            for (const auto &n : it.value())
                ts << ' ' << n.first << '-' << n.second;
            ts << '\n';
        }
    }
}

void showShortcuts(QWidget *parent)
{
    auto *dlg = new QDialog(parent, Qt::Tool | Qt::WindowStaysOnTopHint);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QObject::tr("Keyboard Shortcut Cheat Sheet"));
    dlg->resize(560, 640);
    auto *lay = new QVBoxLayout(dlg);
    auto *table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({QObject::tr("Key"), QObject::tr("Action")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    const QList<QPair<QString, QString>> rows = {
        {QStringLiteral("— Schematic —"), QString()},
        {QStringLiteral("W / F3"), QObject::tr("Draw wire (right click ends segment, / toggles corner)")},
        {QStringLiteral("G"), QObject::tr("Place ground")},
        {QStringLiteral("Alt+G"), QObject::tr("Place COM ground")},
        {QStringLiteral("N / F4"), QObject::tr("Label net")},
        {QStringLiteral("R C L D V I"), QObject::tr("Place resistor, capacitor, inductor, diode, voltage/current source")},
        {QStringLiteral("P / F2"), QObject::tr("Component browser")},
        {QStringLiteral("T"), QObject::tr("Text comment")},
        {QStringLiteral("."), QObject::tr("SPICE directive")},
        {QStringLiteral("A"), QObject::tr("Configure analysis")},
        {QStringLiteral("B"), QObject::tr("Bus tap")},
        {QStringLiteral("M / F7"), QObject::tr("Move")},
        {QStringLiteral("S / F8"), QObject::tr("Drag (stretch wires)")},
        {QStringLiteral("Del / F5"), QObject::tr("Delete")},
        {QStringLiteral("Ctrl+C / F6"), QObject::tr("Copy / duplicate")},
        {QStringLiteral("Ctrl+V"), QObject::tr("Paste")},
        {QStringLiteral("Ctrl+R / Ctrl+E"), QObject::tr("Rotate / mirror")},
        {QStringLiteral("Ctrl+Z / Ctrl+Shift+Z"), QObject::tr("Undo / redo")},
        {QStringLiteral("Z / Shift+Z / Space"), QObject::tr("Zoom area / zoom out / zoom to fit")},
        {QStringLiteral("Ctrl+G"), QObject::tr("Toggle grid")},
        {QStringLiteral("Ctrl+U"), QObject::tr("Mark unconnected pins")},
        {QStringLiteral("Ctrl+A"), QObject::tr("Show text anchors")},
        {QStringLiteral("Ctrl+F"), QObject::tr("Find component / net")},
        {QStringLiteral("Esc / right click"), QObject::tr("Leave current mode")},
        {QStringLiteral("— Simulation —"), QString()},
        {QStringLiteral("Alt+R"), QObject::tr("Run / pause")},
        {QStringLiteral("Alt+S"), QObject::tr("Stop")},
        {QStringLiteral("Ctrl+L"), QObject::tr("SPICE output log")},
        {QStringLiteral("click wire / part"), QObject::tr("Plot voltage / current")},
        {QStringLiteral("Alt+click part"), QObject::tr("Plot power")},
        {QStringLiteral("drag net to net"), QObject::tr("Plot differential voltage")},
        {QStringLiteral("— Waveforms —"), QString()},
        {QStringLiteral("A"), QObject::tr("Add trace")},
        {QStringLiteral("P / B"), QObject::tr("Add pane above / below")},
        {QStringLiteral("U / D"), QObject::tr("Move pane up / down")},
        {QStringLiteral("Shift+S"), QObject::tr("Select steps")},
        {QStringLiteral("C / Shift+C"), QObject::tr("Cursor on active trace / clear cursors")},
        {QStringLiteral("click label"), QObject::tr("Attach cursor (Shift: 2nd, Alt: both)")},
        {QStringLiteral("Ctrl+click label"), QObject::tr("Average / RMS over the visible range")},
        {QStringLiteral("← → (↑ ↓)"), QObject::tr("Move cursor between points (steps/traces)")},
        {QStringLiteral("wheel / Ctrl+wheel"), QObject::tr("Zoom horizontal / vertical")},
        {QStringLiteral("Ctrl+drag / middle drag"), QObject::tr("Pan")},
        {QStringLiteral("Ctrl+Y"), QObject::tr("Autorange Y")},
    };
    for (const auto &r : rows) {
        const int i = table->rowCount();
        table->insertRow(i);
        auto *k = new QTableWidgetItem(r.first);
        if (r.second.isEmpty()) {
            QFont f = k->font();
            f.setBold(true);
            k->setFont(f);
        }
        table->setItem(i, 0, k);
        table->setItem(i, 1, new QTableWidgetItem(r.second));
    }
    table->resizeColumnsToContents();
    lay->addWidget(table);
    dlg->show();
}

void showAbout(QWidget *parent)
{
    QMessageBox::about(parent, QObject::tr("About LinSpice"),
                       QObject::tr("<h3>LinSpice %1</h3>"
                                   "<p>A native Linux circuit simulator and schematic editor that reads and writes LTspice-format "
                                   "schematics (.asc), symbols (.asy), netlists and waveform files.</p>"
                                   "<p>Simulation engine: ngspice (shared library), run with its LTspice compatibility mode.</p>"
                                   "<p>LinSpice is an independent project and is not affiliated with Analog Devices. LTspice is a "
                                   "trademark of Analog Devices, Inc. Component libraries are not included; import the library from "
                                   "your own LTspice installation. Encrypted vendor models cannot be simulated.</p>"
                                   "<p>User interface: Qt %2.</p>")
                           .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

} // namespace dialogs
