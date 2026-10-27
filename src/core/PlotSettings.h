#pragma once

#include <QColor>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Plot configuration as stored in LTspice .plt files. Each analysis type has
// its own section ("[Transient Analysis]", "[AC Analysis]", ...).
struct PlotAxis {
    bool valid = false;
    double min = 0, max = 0, step = 0;
    QString unitChar; // ' ' or a unit character as written in the file
    bool log = false;
};

struct PlotTrace {
    QString expr;
    QColor color; // invalid = automatic
    int axis = 0; // 0 = left, 1 = right (for real data)
};

struct PlotPaneSettings {
    QList<PlotTrace> traces;
    PlotAxis x;
    PlotAxis y[3];
    bool gridOn = true;
    bool plotMag = true, plotPhase = true, plotGroupDelay = false;
    // Left y axis mode from "Log: x y ...": 0 linear, 1 logarithmic, 2 dB. In dB mode LTspice
    // stores the axis limits as linear magnitudes (0.0224..1 means -33..0 dB).
    int yMode = 0;
    QString rawText; // original text of the pane block
};

struct PlotSection {
    QString analysis; // e.g. "Transient Analysis"
    QList<PlotPaneSettings> panes;
};

struct PlotSettingsFile {
    QList<PlotSection> sections;

    bool load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;
    const PlotSection *section(const QString &analysis) const;
    PlotSection &sectionFor(const QString &analysis);
};
