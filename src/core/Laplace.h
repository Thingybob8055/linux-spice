#pragma once

#include <QString>
#include <QVector>

#include <functional>
#include <optional>

// Parses a Laplace-domain transfer function written as an expression in `s`
// (e.g. "1/(1+s/(2*pi*1k))", "(s+w0)**2/(s**2+s*w0/Q+w0**2)") into a ratio
// of polynomials. Identifiers other than s are resolved through `lookup`.
namespace laplace {

struct Rational {
    QVector<double> num; // ascending powers of s
    QVector<double> den;
    double delay = 0; // seconds, from exp(-s*T) factors
    // Cascade of first-order sections (1+s/z)/(1+s/p), as {z, p} in rad/s,
    // approximating fractional powers such as sqrt(1+s*tau).
    QVector<QPair<double, double>> sections;
};

std::optional<Rational> parse(const QString &expr, const std::function<std::optional<double>(const QString &)> &lookup,
                              QString *error = nullptr);

// Coefficient list in descending powers, formatted for an XSPICE s_xfer model.
QString coeffList(const QVector<double> &ascending);

} // namespace laplace
