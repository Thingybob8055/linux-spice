#pragma once

#include "Geometry.h"

#include <QList>
#include <QPair>
#include <QPoint>
#include <QSharedPointer>
#include <QString>
#include <QStringList>

// Line styles used by LINE/RECTANGLE/CIRCLE/ARC (trailing optional integer).
enum class LineStyle : quint8 { Solid = 0, Dash = 1, Dot = 2, DashDot = 3, DashDotDot = 4 };

struct Shape {
    enum Kind : quint8 { Line, Rect, Circle, Arc };
    Kind kind = Line;
    // Line: p[0]-p[1]. Rect/Circle: bounding box p[0],p[1].
    // Arc: bounding box p[0],p[1], start point p[2], end point p[3] (counter-clockwise).
    QPoint p[4];
    LineStyle style = LineStyle::Solid;
    bool hasStyle = false; // whether the style number was present in the file
    QString width = QStringLiteral("Normal");

    QString keyword() const;
    static bool parse(const QStringList &tok, Shape &out);
    QString serialize() const;
    Shape transformed(Orient o, QPoint offset) const;
    QRect bounds() const;
};

// Well known attribute window ids.
namespace AttrId {
constexpr int InstName = 0;
constexpr int Type = 1;
constexpr int Prefix = 2;
constexpr int Value = 3;
constexpr int SpiceModel = 38;
constexpr int SpiceLine = 39;
constexpr int SpiceLine2 = 40;
constexpr int Value2 = 123;
constexpr int Description = 37; // not displayable by convention
constexpr int ModelFile = 41;
} // namespace AttrId

int attrIdForName(const QString &name);
QString attrNameForId(int id);

struct AttrWindow {
    int id = 0;
    QPoint pos;
    Justify just;
    int fontSize = 2; // 0 => hidden
};

struct SymText {
    QPoint pos;
    Justify just;
    int fontSize = 2;
    QString text;
};

struct SymPin {
    QPoint pos;
    Justify labelJust;
    bool labelNone = true;
    int labelOffset = 8;
    QString name;
    int spiceOrder = 0;
};

// Ordered key/value list (attribute order matters for round-tripping).
class AttrList
{
public:
    QString value(const QString &key, const QString &def = QString()) const;
    bool contains(const QString &key) const;
    void set(const QString &key, const QString &value);
    void remove(const QString &key);
    const QList<QPair<QString, QString>> &items() const { return m_items; }
    QList<QPair<QString, QString>> &items() { return m_items; }
    bool isEmpty() const { return m_items.isEmpty(); }

private:
    QList<QPair<QString, QString>> m_items;
};

class SymbolDef
{
public:
    enum class Type { Cell, Block };

    QString name;     // name as referenced from schematics (relative to sym dir, '\\' separated)
    QString filePath; // absolute path, or ":/symbols/..." for built-ins
    Type type = Type::Cell;
    QString typeText = QStringLiteral("CELL");
    QString versionLine = QStringLiteral("Version 4");
    QList<Shape> shapes;
    QList<SymText> texts;
    QList<AttrWindow> windows;
    AttrList attrs;
    QList<SymPin> pins;
    bool builtin = false;

    static QSharedPointer<SymbolDef> parse(const QString &text, QString *error = nullptr);
    static QSharedPointer<SymbolDef> load(const QString &path, QString *error = nullptr);
    QString serialize() const;
    bool save(const QString &path, QString *error = nullptr) const;

    const AttrWindow *window(int id) const;
    QRect bounds() const; // local coordinates, includes pins
    QList<SymPin> pinsBySpiceOrder() const;
    QString prefix() const { return attrs.value(QStringLiteral("Prefix")); }
    QString description() const { return attrs.value(QStringLiteral("Description")); }
};

using SymbolPtr = QSharedPointer<SymbolDef>;

// Font size index (0..7) to multiplier relative to the default size.
double fontScale(int fontSize);
