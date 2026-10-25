#pragma once

#include "Symbol.h"
#include "TextIO.h"

#include <QHash>
#include <QList>
#include <QPoint>
#include <QString>

struct Wire {
    QPoint a, b;
};

struct NetFlag {
    QPoint pos;
    QString name;          // "0" = ground; "COM" = common
    QString ioDirection;   // non-empty if an IOPIN accompanies the flag: In, Out, BiDir
    bool isGround() const { return name == QLatin1String("0"); }
};

struct DataFlag {
    QPoint pos;
    QString expr; // without quotes
};

struct BusTap {
    QPoint a, b;
};

struct SymbolInstance {
    QString symbolName; // as written in the file (may contain "\\\\")
    QPoint pos;
    Orient orient = Orient::R0;
    AttrList attrs;               // instance SYMATTRs
    QList<AttrWindow> windows;    // instance WINDOW overrides (local coordinates)
    SymbolPtr def;                // resolved definition (may be null)

    QString instName() const { return attrs.value(QStringLiteral("InstName")); }
    // Attribute lookup with fallback to the symbol definition.
    QString attr(const QString &key) const;
    const AttrWindow *window(int id) const; // instance override, else symbol default
    QList<QPoint> pinPositions() const;     // world coordinates in file pin order
    QPoint pinWorld(const SymPin &pin) const { return pos + orientApply(orient, pin.pos); }
    QRect worldBounds() const;
};

struct SchText {
    QPoint pos;
    Justify just;
    int fontSize = 2;
    QString text;     // with literal newlines (converted from "\n")
    bool directive = false; // '!' prefix vs ';' comment
};

class Schematic
{
public:
    QString filePath;
    QString versionLine = QStringLiteral("Version 4.1");
    int sheetNumber = 1;
    QSize sheetSize = QSize(880, 680);
    textio::Encoding encoding = textio::Encoding::Latin1;

    QList<Wire> wires;
    QList<NetFlag> flags;
    QList<SymbolInstance> symbols;
    QList<SchText> texts;
    QList<Shape> shapes;
    QList<DataFlag> dataFlags;
    QList<BusTap> busTaps;

    bool parse(const QString &text, QString *error = nullptr);
    QString serialize() const;
    bool load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr);

    // Resolve all symbol definitions via the symbol library (uses the file dir).
    void resolveSymbols();
    QString directory() const;
    QStringList unresolvedSymbols() const;

    QRect bounds() const;
    QStringList directives() const; // all active SPICE directive lines, split on newlines

    // Next free instance name for a prefix ("R" -> "R3").
    QString nextInstName(const QString &prefix) const;

    // Split wires at T-junctions and merge collinear overlaps; removes zero-length wires.
    void normalizeWires();
    // Points where three or more connection ends meet (draw junction dots).
    QList<QPoint> junctions() const;
    // Pin positions not touching any wire/pin/flag.
    QList<QPoint> unconnectedPins() const;
};

// Escape/unescape the literal "\n" used for multi-line TEXT content.
QString unescapeText(const QString &s);
QString escapeText(const QString &s);
