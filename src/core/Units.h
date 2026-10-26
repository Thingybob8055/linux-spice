#pragma once

#include <QString>
#include <optional>

namespace units {

// Parses a SPICE number with an engineering suffix ("1k", "4.7u", "2Meg",
// "10mil", "1e-6", "2.2µ"). Trailing unit letters after the multiplier are
// ignored the same way SPICE does ("10uF" == 10u). Returns nullopt if the text
// does not start with a number.
std::optional<double> parseSpiceNumber(const QString &text);

// Formats a value with an SI prefix: 1500 -> "1.5K", 1e-6 -> "1µ".
// unit is appended after the prefix (e.g. "V", "Hz").
QString formatEng(double value, const QString &unit = QString(), int significant = 4);

// Same as formatEng but uses SPICE-compatible suffixes ("Meg", "u") so the
// result can be written back into a netlist.
QString formatSpice(double value, int significant = 6);

// Formats a duration/frequency pair the way the status bar does for a
// horizontal distance: "dx=1ms (1KHz)".
QString formatDelta(double dx, const QString &unit);

// True when `s` is one {...} group: it starts with '{' and that brace's
// match is the final character ("{a}*{b}" is not braced).
bool isBraced(const QString &s);

} // namespace units
