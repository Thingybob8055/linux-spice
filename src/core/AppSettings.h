#pragma once

#include <QColor>
#include <QHash>
#include <QSettings>
#include <QString>
#include <QStringList>

// Central access to persistent application settings.
class AppSettings
{
public:
    static AppSettings &instance();

    // Directory holding an LTspice-compatible component library (sym/, sub/, cmp/).
    // Defaults to ~/.local/share/LinSpice/lib (populated by "Import LTspice Library").
    QString libraryRoot() const;
    void setLibraryRoot(const QString &dir);
    QString defaultLibraryRoot() const;
    QString examplesRoot() const;
    void setExamplesRoot(const QString &dir);
    QString userFilesDir() const; // ~/Documents/LinSpice by default
    void setUserFilesDir(const QString &dir);

    QStringList symbolSearchPaths() const;
    void setSymbolSearchPaths(const QStringList &paths);
    QStringList librarySearchPaths() const;
    void setLibrarySearchPaths(const QStringList &paths);

    // Simulator
    QString ngspiceLibrary() const; // path to libngspice.so (empty = auto)
    void setNgspiceLibrary(const QString &path);
    int maxThreads() const;
    void setMaxThreads(int n);
    QString defaultSolverOptions() const; // extra ".options" appended to every run
    void setDefaultSolverOptions(const QString &opts);
    bool saveDeviceCurrents() const;
    void setSaveDeviceCurrents(bool on);
    bool keepRawFiles() const;
    void setKeepRawFiles(bool on);
    bool defaultInductorRser() const; // emulate the 1mOhm default series resistance
    void setDefaultInductorRser(bool on);

    // Schematic drafting
    bool showGrid() const;
    void setShowGrid(bool on);
    bool orthoWires() const;
    void setOrthoWires(bool on);
    bool markUnconnectedPins() const;
    void setMarkUnconnectedPins(bool on);
    bool allowPinShorts() const;
    void setAllowPinShorts(bool on);
    bool thickLines() const;
    void setThickLines(bool on);
    int undoDepth() const;
    QString fontFamily() const;
    void setFontFamily(const QString &f);

    // Waveforms
    bool waveformThickLines() const;
    void setWaveformThickLines(bool on);
    bool waveformGrid() const;
    void setWaveformGrid(bool on);
    bool markDataPoints() const;
    void setMarkDataPoints(bool on);

    // Colors (key -> color); keys in colorKeys().
    QColor color(const QString &key) const;
    void setColor(const QString &key, const QColor &c);
    void resetColors();
    static QStringList colorKeys();
    static QString colorLabel(const QString &key);
    static QColor defaultColor(const QString &key);
    QList<QColor> traceColors() const;

    QStringList recentFiles() const;
    void addRecentFile(const QString &path);

    QSettings &raw() { return m_settings; }

private:
    AppSettings();
    mutable QSettings m_settings;
};
