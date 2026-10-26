#pragma once

#include "Schematic.h"

#include <QHash>
#include <QSet>
#include <QStringList>

// Connectivity of one schematic sheet.
struct Connectivity {
    // Net name for each wire index, each flag index, and each pin of each symbol.
    QList<QString> wireNet;
    QList<QString> flagNet;
    QList<QStringList> symbolPinNets; // indexed by symbol, pins in file order
    QHash<qint64, QString> pointNet;  // any connection point -> net
    QStringList netNames;             // all nets in creation order

    static qint64 key(QPoint p) { return (qint64(p.x()) << 32) ^ quint32(p.y()); }
    QString netAt(QPoint p) const { return pointNet.value(key(p)); }
};

struct NetlistResult {
    QString text;                 // LTspice-style netlist
    QStringList warnings;
    QStringList errors;
    Connectivity conn;            // connectivity of the top sheet
    QList<QString> symbolSpiceName; // per top-level symbol: netlist element name ("" if not netlisted)
    QStringList libFiles;         // .lib files requested by symbols (unresolved names)
};

class Netlister
{
public:
    explicit Netlister(const Schematic &sch);

    NetlistResult build();
    static Connectivity connectivity(const Schematic &sch, QStringList *warnings = nullptr);

    // Name an element would get in the netlist (prefix handling, '§' separator).
    static QString spiceNameFor(const SymbolInstance &s);
    // True if a symbol stands for a hierarchical sub-schematic; fills path.
    // `selfPath` is the schematic containing the symbol: a schematic is never a block of itself
    // (e.g. examples named after the library symbol they use, such as opamp.asc).
    static bool isHierarchicalBlock(const SymbolInstance &s, const QString &schDir, QString *ascPath,
                                    const QString &selfPath = QString());

private:
    struct SubcktJob {
        QString name;
        QString ascPath;
        QStringList ports;
    };
    QStringList sheetBody(const Schematic &sch, const Connectivity &conn, bool topLevel,
                          QList<QString> *names);
    void emitSubckts();

    const Schematic &m_sch;
    NetlistResult m_result;
    QList<SubcktJob> m_pendingSubckts;
    QSet<QString> m_doneSubckts;
    QStringList m_subcktText;
    QStringList m_libLines;
    QStringList m_topDirectives;
    QSet<QString> m_globals;
};

// True if an attribute value names a model file rather than a model.
bool looksLikeModelFile(const QString &s);
// The Misc\\jumper symbol: joins two nets, not a netlist element.
bool isJumper(const SymbolInstance &s);
