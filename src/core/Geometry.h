#pragma once

#include <QPoint>
#include <QPointF>

#include <algorithm>
#include <cmath>
#include <QRect>
#include <QString>

// Orientation of a placed symbol. The M forms rotate first, then mirror
// horizontally (negate x), matching the .asc file semantics.
enum class Orient : quint8 { R0, R90, R180, R270, M0, M90, M180, M270 };

inline QString orientToString(Orient o)
{
    static const char *names[] = {"R0", "R90", "R180", "R270", "M0", "M90", "M180", "M270"};
    return QString::fromLatin1(names[int(o)]);
}

inline Orient orientFromString(const QString &s)
{
    const QString u = s.trimmed().toUpper();
    static const char *names[] = {"R0", "R90", "R180", "R270", "M0", "M90", "M180", "M270"};
    for (int i = 0; i < 8; ++i)
        if (u == QLatin1String(names[i]))
            return Orient(i);
    return Orient::R0;
}

inline bool isMirrored(Orient o) { return int(o) >= 4; }
inline int rotationQuarter(Orient o) { return int(o) & 3; }

// Transform a vector/local point by an orientation.
inline QPoint orientApply(Orient o, QPoint p)
{
    int x = p.x(), y = p.y();
    int rx = x, ry = y;
    switch (rotationQuarter(o)) {
    case 0: rx = x; ry = y; break;
    case 1: rx = -y; ry = x; break;
    case 2: rx = -x; ry = -y; break;
    case 3: rx = y; ry = -x; break;
    }
    if (isMirrored(o))
        rx = -rx;
    return {rx, ry};
}

inline QPointF orientApplyF(Orient o, QPointF p)
{
    double x = p.x(), y = p.y();
    double rx = x, ry = y;
    switch (rotationQuarter(o)) {
    case 0: rx = x; ry = y; break;
    case 1: rx = -y; ry = x; break;
    case 2: rx = -x; ry = -y; break;
    case 3: rx = y; ry = -x; break;
    }
    if (isMirrored(o))
        rx = -rx;
    return {rx, ry};
}

// Compose: apply a then b (b ∘ a).
inline Orient orientCompose(Orient a, Orient b)
{
    // Brute force: find the orientation that maps two probe vectors the same way.
    const QPoint p1(1, 0), p2(0, 1);
    const QPoint t1 = orientApply(b, orientApply(a, p1));
    const QPoint t2 = orientApply(b, orientApply(a, p2));
    for (int i = 0; i < 8; ++i)
        if (orientApply(Orient(i), p1) == t1 && orientApply(Orient(i), p2) == t2)
            return Orient(i);
    return Orient::R0;
}

// Rotating a placed item in the editor (Ctrl+R) rotates 90° clockwise in world space.
inline Orient orientRotateCW(Orient o) { return orientCompose(o, Orient::R90); }
// Mirroring in the editor (Ctrl+E) flips horizontally in world space.
inline Orient orientMirror(Orient o) { return orientCompose(o, Orient::M0); }

// Text justification as used by WINDOW, TEXT and PIN lines.
struct Justify {
    enum Align : quint8 { Start, Middle, End };      // along the reading direction
    enum Perp : quint8 { PerpTop, PerpCenter, PerpBottom }; // perpendicular to it
    Align align = Start;
    Perp perp = PerpCenter;
    bool vertical = false;
    bool invisible = false;

    static Justify fromString(const QString &s);
    QString toString() const;
    // Pin label justification uses upper case and NONE for "no label".
    static Justify fromPinString(const QString &s, bool *none);
    QString toPinString() const;
};

// Transform a justification by an orientation, keeping text readable
// (left-to-right or bottom-to-top).
Justify justifyTransform(const Justify &j, Orient o);

inline Justify Justify::fromString(const QString &s)
{
    Justify j;
    QString u = s.trimmed().toLower();
    if (u == QLatin1String("invisible")) {
        j.invisible = true;
        return j;
    }
    if (u.startsWith('v') && u.size() > 1) {
        j.vertical = true;
        u = u.mid(1);
    }
    if (u == QLatin1String("left")) { j.align = Start; j.perp = PerpCenter; }
    else if (u == QLatin1String("right")) { j.align = End; j.perp = PerpCenter; }
    else if (u == QLatin1String("center")) { j.align = Middle; j.perp = PerpCenter; }
    else if (u == QLatin1String("top")) { j.align = Middle; j.perp = PerpTop; }
    else if (u == QLatin1String("bottom")) { j.align = Middle; j.perp = PerpBottom; }
    return j;
}

inline QString Justify::toString() const
{
    if (invisible)
        return QStringLiteral("Invisible");
    QString base;
    if (perp == PerpTop) base = QStringLiteral("Top");
    else if (perp == PerpBottom) base = QStringLiteral("Bottom");
    else if (align == Start) base = QStringLiteral("Left");
    else if (align == End) base = QStringLiteral("Right");
    else base = QStringLiteral("Center");
    return vertical ? QStringLiteral("V") + base : base;
}

inline Justify Justify::fromPinString(const QString &s, bool *none)
{
    const QString u = s.trimmed().toUpper();
    *none = (u == QLatin1String("NONE"));
    if (*none)
        return Justify();
    return fromString(s);
}

inline QString Justify::toPinString() const
{
    return toString().toUpper();
}

inline Justify justifyTransform(const Justify &j, Orient o)
{
    if (o == Orient::R0)
        return j;
    // Reading direction s and "up" direction u in local coordinates (y down).
    QPoint s = j.vertical ? QPoint(0, -1) : QPoint(1, 0);
    QPoint u = j.vertical ? QPoint(-1, 0) : QPoint(0, -1);
    s = orientApply(o, s);
    u = orientApply(o, u);
    Justify r = j;
    r.vertical = (s.x() == 0);
    const QPoint sWant = r.vertical ? QPoint(0, -1) : QPoint(1, 0);
    const QPoint uWant = r.vertical ? QPoint(-1, 0) : QPoint(0, -1);
    if (s != sWant) {
        if (r.align == Justify::Start) r.align = Justify::End;
        else if (r.align == Justify::End) r.align = Justify::Start;
    }
    if (u != uWant) {
        if (r.perp == Justify::PerpTop) r.perp = Justify::PerpBottom;
        else if (r.perp == Justify::PerpBottom) r.perp = Justify::PerpTop;
    }
    return r;
}

// Distance from point p to segment ab.
inline double distToSegment(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = 0.0;
    if (len2 > 0.0)
        t = std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0);
    const QPointF c = a + t * ab;
    const QPointF d = p - c;
    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

// True if p lies on segment ab (integer grid, inclusive of endpoints).
inline bool pointOnSegment(QPoint p, QPoint a, QPoint b)
{
    const qint64 cross = qint64(b.x() - a.x()) * (p.y() - a.y()) - qint64(b.y() - a.y()) * (p.x() - a.x());
    if (cross != 0)
        return false;
    return p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x()) && p.y() >= std::min(a.y(), b.y())
        && p.y() <= std::max(a.y(), b.y());
}

inline QRect rectFromPoints(QPoint a, QPoint b)
{
    return QRect(QPoint(std::min(a.x(), b.x()), std::min(a.y(), b.y())),
                 QPoint(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
}
