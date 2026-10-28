#pragma once

#include "RawFile.h"

#include <QHash>
#include <QString>
#include <QStringList>

// LTspice's .four: harmonic analysis of transient waveforms over the last N periods,
// done in post-processing on the simulated data.
namespace fourier {

// .four <freq> [Nharmonics] [Nperiods] <trace> [<trace> ...]
// Nharmonics defaults to 9, Nperiods to 1 (-1: the whole run). Returns the log
// text LTspice prints for one run.
QString evaluate(const Dataset &ds, int run, const QStringList &statements, const QHash<QString, double> &params);

} // namespace fourier
