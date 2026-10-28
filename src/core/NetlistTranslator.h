#pragma once

#include "Fra.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

// Translates an LTspice-dialect netlist into a netlist ngspice can simulate
// (with ngspice's LTspice compatibility mode enabled). Library references are
// resolved and only the definitions that are actually used are pulled in.
struct TranslationResult {
    QString netlist;
    QStringList errors;
    QStringList warnings;
    QString analysis;       // "tran", "ac", "dc", "op", "noise", "tf" or ""
    QString analysisLine;   // the original LTspice analysis directive
    QString dcSweepSource;  // name of the first swept source/parameter in .dc
    QHash<QString, QString> elementNames; // lower-case ngspice name -> LTspice name
    QHash<QString, QString> nodeNames;    // lower-case node -> original spelling
    QStringList measStatements;
    QStringList waveStatements;
    QStringList fourStatements;
    QStringList funcLines;  // .func definitions (also usable in the waveform viewer)
    QHash<QString, QString> params; // .param name -> expression text
    QString measComplexFormat;      // from .options meascplxfmt
    bool hasAnalysis = false;
    // BJTs whose model has no substrate junction: LTspice reports their Is as zero,
    // where ngspice's substrate current is gmin leakage.
    QSet<QString> noSubstrateBjts; // lower-case LTspice names
    // Pin current sense sources of subcircuit instances: lower-case source name -> "Ix(u1:pin)".
    QHash<QString, QString> pinCurrents;
    // Click-probe lookup: "<instance>:<netlist pin number>" (lower case, as LTspice shows the
    // instance: "u1:3", "a1:7", "z1:1") -> the current's vector name.
    QHash<QString, QString> pinProbes;
    // Pin currents computed after the run: name -> pin currents whose negated sum it is
    // (empty: identically zero), e.g. "I8(A1)" -> {"I6(A1)", "I7(A1)"}.
    QList<QPair<QString, QStringList>> derivedCurrents;
    // Rpar/Cpar are separate elements here but part of the device in LTspice, whose current
    // includes them: lower-case element -> its parallel helper elements.
    QHash<QString, QStringList> parallelParts;
    QSet<QString> negatedDevices; // lower-case ngspice names whose terminal currents flip sign
    // .fra: the run is a transient with these analyzers' stimuli; results are extracted afterwards.
    bool fra = false;
    QList<fra::Analyzer> fraAnalyzers;
    QList<fra::Probe> fraProbes;
};

class NetlistTranslator
{
public:
    struct Options {
        QString baseDir;          // directory of the schematic/netlist (for relative paths)
        bool inductorRser = true; // LTspice's default 1mΩ series resistance on inductors
        bool saveCurrents = true;
        QString extraOptions;     // appended as a .options line
    };

    TranslationResult translate(const QStringList &ltLines, const Options &opts);

    // Splits an LTspice element/directive line into tokens, keeping
    // parenthesised/braced groups together ("PULSE(0 1 0)" is one token).
    static QStringList tokenize(const QString &line);
    // Rewrites LTspice expression functions that ngspice lacks (if, limit, table, buf, inv).
    static QString rewriteExpression(const QString &expr);

private:
    struct Subckt {
        QString name;
        QString header;
        QStringList body;
    };

    void processLines(const QStringList &lines, int depth);
    QString akoType(const QRegularExpressionMatch &m) const;
    QStringList subcktPorts(const QString &name) const;
    void addLibrary(const QString &file, const QString &section);
    QStringList translateElement(const QString &line, bool topLevel);
    QStringList translateElementTokens(QStringList t, const QString &line, QChar k);
    QStringList translatePassive(const QStringList &t);
    QStringList translateSource(const QStringList &t);
    QStringList translateBehavioral(const QString &line);
    QStringList translateControlled(const QStringList &t, const QString &line);
    QStringList translateSemiconductor(const QStringList &t);
    QStringList translateDigital(const QStringList &t);
    QStringList translateMutual(const QStringList &t);
    // 1T resistors to ground for top-level nodes with no DC path to ground (e.g. joined only by capacitors),
    // which LTspice solves but which make the ngspice operating-point matrix singular.
    static QStringList floatingNodeShunts(const QStringList &elements, const QStringList &tiedNodes = {});
    QStringList translateSwitch(const QStringList &t);
    QStringList translateIdealDiode(const QStringList &t, const QString &modelLine);
    QStringList translateIgbt(const QStringList &t, const QString &modelLine, bool pChannel);
    QStringList translateFraAnalyzer(const QStringList &t);
    QStringList laplaceSource(const QString &name, QChar kind, const QString &n1, const QString &n2, const QString &input, const QString &line);
    QString translateModel(const QString &line);
    QString translateAnalysis(const QString &line);
    void handleOptions(const QString &line);
    void collectNeeds(const QStringList &elementLines, QSet<QString> &models, QSet<QString> &subckts) const;
    void resolveDefinitions();
    QString pwlFromFile(const QString &file, double timeScale, double valueScale);
    QString pwlFromWav(const QString &file, int channel);
    bool readWav(const QString &file, int channel, QVector<double> *samples, double *rate);
    QString wavStreamFile(const QString &file, int channel, double *rate);
    QString nodeToken(const QString &n);
    QString elementName(const QString &n);
    bool isKnownModel(const QString &name) const;
    QString internalNode(const QString &elem, int k);
    void collectCurrentSenses(const QStringList &lines);
    void disambiguateModels(QStringList &out);
    QString rewriteCurrents(const QString &expr) const;

    Options m_opts;
    TranslationResult m_res;
    QStringList m_topElements;
    QList<Subckt> m_subckts;
    QStringList m_models;      // .model lines from the netlist
    QStringList m_params;      // .param / .func lines
    QStringList m_directives;  // other directives passed through (.ic, .nodeset, .temp, .global, .four)
    QStringList m_analysis;    // translated analysis lines
    QStringList m_optionLines;
    QStringList m_libFiles;    // resolved library paths in order
    QHash<QString, QString> m_libSections; // path -> section (for HSPICE style .lib file section)
    QSet<QString> m_definedModels;  // lower-case names defined in the netlist
    QSet<QString> m_definedSubckts;
    QHash<QString, QString> m_modelTypes; // lower-case model -> type (all known models)
    QStringList m_pulledDefs;  // definitions pulled from libraries
    QHash<QString, QString> m_inductorIc; // from .ic I(L1)=x
    QHash<QString, QStringList> m_stepModelOverrides; // lower model -> "param=value"
    QHash<QString, QString> m_modelLines; // lower model name -> .model line (for parameter lookups)
    QHash<QString, QSet<int>> m_senses; // lower device name -> 1-based terminals needing a 0V sense source
    double m_tranMaxStep = 0;
    double m_deviceMaxStep = 0; // timestep limit required by devices without integrated state (e.g. VCO outputs)
    bool m_thevInduc = false;
    int m_includeDepth = 0;
    bool m_inMachine = false;
    bool m_inSubckt = false; // translating a subcircuit body (models there are macromodel internals)
    bool m_startup = false;
};
