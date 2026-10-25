#include "AppSettings.h"

#include <QDir>
#include <QStandardPaths>
#include <QThread>

AppSettings &AppSettings::instance()
{
    static AppSettings s;
    return s;
}

AppSettings::AppSettings()
    : m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("LinSpice"), QStringLiteral("LinSpice"))
{
}

QString AppSettings::defaultLibraryRoot() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/lib");
}

QString AppSettings::libraryRoot() const
{
    if (qEnvironmentVariableIsSet("LINSPICE_LIBRARY_ROOT"))
        return qEnvironmentVariable("LINSPICE_LIBRARY_ROOT");
    return m_settings.value(QStringLiteral("paths/libraryRoot"), defaultLibraryRoot()).toString();
}

void AppSettings::setLibraryRoot(const QString &dir) { m_settings.setValue(QStringLiteral("paths/libraryRoot"), dir); }

QString AppSettings::examplesRoot() const
{
    if (qEnvironmentVariableIsSet("LINSPICE_EXAMPLES_ROOT"))
        return qEnvironmentVariable("LINSPICE_EXAMPLES_ROOT");
    return m_settings
        .value(QStringLiteral("paths/examplesRoot"),
               QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/examples"))
        .toString();
}

void AppSettings::setExamplesRoot(const QString &dir) { m_settings.setValue(QStringLiteral("paths/examplesRoot"), dir); }

QString AppSettings::userFilesDir() const
{
    return m_settings
        .value(QStringLiteral("paths/userFiles"),
               QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/LinSpice"))
        .toString();
}

void AppSettings::setUserFilesDir(const QString &dir) { m_settings.setValue(QStringLiteral("paths/userFiles"), dir); }

QStringList AppSettings::symbolSearchPaths() const
{
    return m_settings.value(QStringLiteral("paths/symbolSearch")).toStringList();
}

void AppSettings::setSymbolSearchPaths(const QStringList &paths)
{
    m_settings.setValue(QStringLiteral("paths/symbolSearch"), paths);
}

QStringList AppSettings::librarySearchPaths() const
{
    return m_settings.value(QStringLiteral("paths/librarySearch")).toStringList();
}

void AppSettings::setLibrarySearchPaths(const QStringList &paths)
{
    m_settings.setValue(QStringLiteral("paths/librarySearch"), paths);
}

QString AppSettings::ngspiceLibrary() const { return m_settings.value(QStringLiteral("sim/ngspiceLib")).toString(); }
void AppSettings::setNgspiceLibrary(const QString &path) { m_settings.setValue(QStringLiteral("sim/ngspiceLib"), path); }

int AppSettings::maxThreads() const
{
    return m_settings.value(QStringLiteral("sim/threads"), std::max(1, QThread::idealThreadCount())).toInt();
}

void AppSettings::setMaxThreads(int n) { m_settings.setValue(QStringLiteral("sim/threads"), n); }
QString AppSettings::defaultSolverOptions() const { return m_settings.value(QStringLiteral("sim/options")).toString(); }
void AppSettings::setDefaultSolverOptions(const QString &opts) { m_settings.setValue(QStringLiteral("sim/options"), opts); }
bool AppSettings::saveDeviceCurrents() const { return m_settings.value(QStringLiteral("sim/saveCurrents"), true).toBool(); }
void AppSettings::setSaveDeviceCurrents(bool on) { m_settings.setValue(QStringLiteral("sim/saveCurrents"), on); }
bool AppSettings::keepRawFiles() const { return m_settings.value(QStringLiteral("sim/keepRaw"), true).toBool(); }
void AppSettings::setKeepRawFiles(bool on) { m_settings.setValue(QStringLiteral("sim/keepRaw"), on); }
bool AppSettings::defaultInductorRser() const { return m_settings.value(QStringLiteral("sim/inductorRser"), true).toBool(); }
void AppSettings::setDefaultInductorRser(bool on) { m_settings.setValue(QStringLiteral("sim/inductorRser"), on); }

bool AppSettings::showGrid() const { return m_settings.value(QStringLiteral("schematic/grid"), true).toBool(); }
void AppSettings::setShowGrid(bool on) { m_settings.setValue(QStringLiteral("schematic/grid"), on); }
bool AppSettings::orthoWires() const { return m_settings.value(QStringLiteral("schematic/ortho"), true).toBool(); }
void AppSettings::setOrthoWires(bool on) { m_settings.setValue(QStringLiteral("schematic/ortho"), on); }
bool AppSettings::markUnconnectedPins() const { return m_settings.value(QStringLiteral("schematic/markUnconn"), true).toBool(); }
void AppSettings::setMarkUnconnectedPins(bool on) { m_settings.setValue(QStringLiteral("schematic/markUnconn"), on); }
bool AppSettings::allowPinShorts() const { return m_settings.value(QStringLiteral("schematic/allowShorts"), false).toBool(); }
void AppSettings::setAllowPinShorts(bool on) { m_settings.setValue(QStringLiteral("schematic/allowShorts"), on); }
bool AppSettings::thickLines() const { return m_settings.value(QStringLiteral("schematic/thick"), false).toBool(); }
void AppSettings::setThickLines(bool on) { m_settings.setValue(QStringLiteral("schematic/thick"), on); }
int AppSettings::undoDepth() const { return m_settings.value(QStringLiteral("schematic/undoDepth"), 200).toInt(); }
QString AppSettings::fontFamily() const { return m_settings.value(QStringLiteral("schematic/font"), QStringLiteral("Sans Serif")).toString(); }
void AppSettings::setFontFamily(const QString &f) { m_settings.setValue(QStringLiteral("schematic/font"), f); }

bool AppSettings::waveformThickLines() const { return m_settings.value(QStringLiteral("waveform/thick"), false).toBool(); }
void AppSettings::setWaveformThickLines(bool on) { m_settings.setValue(QStringLiteral("waveform/thick"), on); }
bool AppSettings::waveformGrid() const { return m_settings.value(QStringLiteral("waveform/grid"), true).toBool(); }
void AppSettings::setWaveformGrid(bool on) { m_settings.setValue(QStringLiteral("waveform/grid"), on); }
bool AppSettings::markDataPoints() const { return m_settings.value(QStringLiteral("waveform/markPoints"), false).toBool(); }
void AppSettings::setMarkDataPoints(bool on) { m_settings.setValue(QStringLiteral("waveform/markPoints"), on); }

QStringList AppSettings::colorKeys()
{
    return {QStringLiteral("sch.background"), QStringLiteral("sch.wire"), QStringLiteral("sch.junction"),
            QStringLiteral("sch.component"), QStringLiteral("sch.componentFill"), QStringLiteral("sch.pin"),
            QStringLiteral("sch.text"), QStringLiteral("sch.directive"), QStringLiteral("sch.comment"),
            QStringLiteral("sch.flag"), QStringLiteral("sch.grid"), QStringLiteral("sch.selection"),
            QStringLiteral("sch.unconnected"), QStringLiteral("sch.drawing"),
            QStringLiteral("wave.background"), QStringLiteral("wave.axis"), QStringLiteral("wave.grid"),
            QStringLiteral("wave.cursor"), QStringLiteral("wave.text"),
            QStringLiteral("trace.0"), QStringLiteral("trace.1"), QStringLiteral("trace.2"), QStringLiteral("trace.3"),
            QStringLiteral("trace.4"), QStringLiteral("trace.5"), QStringLiteral("trace.6"), QStringLiteral("trace.7"),
            QStringLiteral("trace.8"), QStringLiteral("trace.9"), QStringLiteral("trace.10"), QStringLiteral("trace.11"),
            QStringLiteral("net.background"), QStringLiteral("net.text"), QStringLiteral("net.comment"),
            QStringLiteral("net.directive")};
}

QString AppSettings::colorLabel(const QString &key)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("sch.background"), QStringLiteral("Schematic background")},
        {QStringLiteral("sch.wire"), QStringLiteral("Wires")},
        {QStringLiteral("sch.junction"), QStringLiteral("Junction dots")},
        {QStringLiteral("sch.component"), QStringLiteral("Component outlines")},
        {QStringLiteral("sch.componentFill"), QStringLiteral("Component fill")},
        {QStringLiteral("sch.pin"), QStringLiteral("Pins")},
        {QStringLiteral("sch.text"), QStringLiteral("Attribute text")},
        {QStringLiteral("sch.directive"), QStringLiteral("SPICE directives")},
        {QStringLiteral("sch.comment"), QStringLiteral("Comments")},
        {QStringLiteral("sch.flag"), QStringLiteral("Net labels / ground")},
        {QStringLiteral("sch.grid"), QStringLiteral("Grid points")},
        {QStringLiteral("sch.selection"), QStringLiteral("Selection highlight")},
        {QStringLiteral("sch.unconnected"), QStringLiteral("Unconnected pin marks")},
        {QStringLiteral("sch.drawing"), QStringLiteral("Graphic drawings")},
        {QStringLiteral("wave.background"), QStringLiteral("Waveform background")},
        {QStringLiteral("wave.axis"), QStringLiteral("Axes")},
        {QStringLiteral("wave.grid"), QStringLiteral("Waveform grid")},
        {QStringLiteral("wave.cursor"), QStringLiteral("Cursors")},
        {QStringLiteral("wave.text"), QStringLiteral("Waveform text")},
        {QStringLiteral("net.background"), QStringLiteral("Netlist background")},
        {QStringLiteral("net.text"), QStringLiteral("Netlist text")},
        {QStringLiteral("net.comment"), QStringLiteral("Netlist comments")},
        {QStringLiteral("net.directive"), QStringLiteral("Netlist directives")},
    };
    if (key.startsWith(QLatin1String("trace.")))
        return QStringLiteral("Trace color %1").arg(key.mid(6).toInt() + 1);
    return labels.value(key, key);
}

QColor AppSettings::defaultColor(const QString &key)
{
    static const QHash<QString, QColor> defaults = {
        {QStringLiteral("sch.background"), QColor(255, 255, 255)},
        {QStringLiteral("sch.wire"), QColor(0, 0, 255)},
        {QStringLiteral("sch.junction"), QColor(0, 0, 255)},
        {QStringLiteral("sch.component"), QColor(0, 0, 0)},
        {QStringLiteral("sch.componentFill"), QColor(255, 255, 220)},
        {QStringLiteral("sch.pin"), QColor(128, 0, 0)},
        {QStringLiteral("sch.text"), QColor(0, 0, 0)},
        {QStringLiteral("sch.directive"), QColor(0, 0, 0)},
        {QStringLiteral("sch.comment"), QColor(0, 0, 255)},
        {QStringLiteral("sch.flag"), QColor(0, 0, 0)},
        {QStringLiteral("sch.grid"), QColor(128, 128, 128)},
        {QStringLiteral("sch.selection"), QColor(255, 0, 0)},
        {QStringLiteral("sch.unconnected"), QColor(255, 0, 0)},
        {QStringLiteral("sch.drawing"), QColor(0, 0, 128)},
        {QStringLiteral("wave.background"), QColor(0, 0, 0)},
        {QStringLiteral("wave.axis"), QColor(160, 160, 160)},
        {QStringLiteral("wave.grid"), QColor(70, 70, 70)},
        {QStringLiteral("wave.cursor"), QColor(220, 220, 220)},
        {QStringLiteral("wave.text"), QColor(220, 220, 220)},
        {QStringLiteral("net.background"), QColor(255, 255, 255)},
        {QStringLiteral("net.text"), QColor(0, 0, 0)},
        {QStringLiteral("net.comment"), QColor(0, 128, 0)},
        {QStringLiteral("net.directive"), QColor(0, 0, 200)},
    };
    if (key.startsWith(QLatin1String("trace."))) {
        static const QColor traces[] = {QColor(0, 200, 0),   QColor(0, 120, 255),  QColor(255, 60, 60),
                                        QColor(0, 200, 200), QColor(220, 0, 220),  QColor(200, 200, 0),
                                        QColor(160, 160, 160), QColor(255, 140, 0), QColor(120, 120, 255),
                                        QColor(100, 255, 100), QColor(255, 120, 180), QColor(0, 160, 120)};
        return traces[key.mid(6).toInt() % 12];
    }
    return defaults.value(key, Qt::black);
}

QColor AppSettings::color(const QString &key) const
{
    const QVariant v = m_settings.value(QStringLiteral("colors/") + key);
    if (v.isValid()) {
        QColor c(v.toString());
        if (c.isValid())
            return c;
    }
    return defaultColor(key);
}

void AppSettings::setColor(const QString &key, const QColor &c)
{
    m_settings.setValue(QStringLiteral("colors/") + key, c.name());
}

void AppSettings::resetColors()
{
    m_settings.remove(QStringLiteral("colors"));
}

QList<QColor> AppSettings::traceColors() const
{
    QList<QColor> out;
    for (int i = 0; i < 12; ++i)
        out << color(QStringLiteral("trace.%1").arg(i));
    return out;
}

QStringList AppSettings::recentFiles() const
{
    return m_settings.value(QStringLiteral("recent/files")).toStringList();
}

void AppSettings::addRecentFile(const QString &path)
{
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 12)
        files.removeLast();
    m_settings.setValue(QStringLiteral("recent/files"), files);
}
