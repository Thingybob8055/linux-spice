#pragma once

#include "core/Schematic.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QRectF>
#include <QSet>

struct SchematicStyle {
    QColor background, wire, junction, component, componentFill, pin, text, directive, comment, flag, grid, selection,
        unconnected, drawing, highlight;
    bool showGrid = true;
    bool markUnconnected = true;
    bool showAnchors = false;
    bool thick = false;
    QString fontFamily;

    static SchematicStyle fromSettings();
    static SchematicStyle monochrome();
};

// Identifies one drawable/selectable object of a schematic.
struct SchItem {
    enum Type : quint8 { None, Wire, Symbol, Flag, Text, Shape, DataFlag, BusTap, SymbolAttr };
    Type type = None;
    int index = -1;
    int sub = -1; // attribute window id for SymbolAttr
    bool operator==(const SchItem &o) const { return type == o.type && index == o.index && sub == o.sub; }
    bool isValid() const { return type != None && index >= 0; }
};
inline size_t qHash(const SchItem &i, size_t seed = 0) { return ::qHash((int(i.type) << 24) ^ (i.index << 8) ^ (i.sub & 0xff), seed); }

class SchematicPainter
{
public:
    // Base height of text with font size index 2, in schematic units.
    static constexpr double kTextHeight = 14.0;

    static QFont font(int fontSize, const QString &family = QString());
    // Bounding rect (world coordinates) of justified text; handles multi-line and vertical text.
    static QRectF textRect(QPointF anchor, const Justify &just, int fontSize, const QString &text, const QString &family = QString());
    static void drawText(QPainter &p, QPointF anchor, const Justify &just, int fontSize, const QString &text, const QColor &color,
                         const QString &family = QString());

    static void drawShape(QPainter &p, const Shape &s);
    // Draws a symbol definition placed at `pos` with orientation `o` (no attribute text).
    static void drawSymbolBody(QPainter &p, const SymbolDef &def, QPoint pos, Orient o, const QColor &color, const QColor &pinColor,
                               bool drawPinLabels, const QString &family = QString());
    // Text shown for an attribute window of a placed symbol (empty if none).
    static QString windowText(const SymbolInstance &s, int windowId);
    // World rect and justification of an attribute window of a placed symbol.
    static bool windowGeometry(const SymbolInstance &s, int windowId, QPointF *anchor, Justify *just, int *fontSize);
    static QRectF windowRect(const SymbolInstance &s, int windowId, const QString &family = QString());

    // Direction (unit vector) in which a flag's label should be drawn, based on its connections.
    static QPoint flagDirection(const Schematic &sch, const NetFlag &f);
    static void drawFlag(QPainter &p, const NetFlag &f, QPoint dir, const SchematicStyle &st, bool selected);
    static QRectF flagRect(const NetFlag &f, QPoint dir, const QString &family = QString());

    struct Options {
        QSet<SchItem> selected;
        QSet<QString> highlightNets; // lower-case net names
        const QHash<QString, QString> *netOfWire = nullptr; // unused placeholder
        QList<QString> wireNets;      // per-wire net (for highlighting), optional
        QHash<qint64, QString> opLabels; // point -> text (operating point annotations)
        bool drawSheetBorder = false;
    };

    // Paints the full schematic; `visible` limits work to the exposed world area.
    static void paint(QPainter &p, const Schematic &sch, const SchematicStyle &st, const QRectF &visible, const Options &opt);
    static void drawGrid(QPainter &p, const QRectF &visible, double scale, const QColor &color);
};
