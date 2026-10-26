#include "RawFile.h"

#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>

#include <cmath>
#include <cstring>

int Dataset::varIndex(const QString &name) const
{
    return index.value(name.toLower(), -1);
}

int Dataset::pointCount(int run) const
{
    if (run < 0 || run >= runs.size() || runs[run].re.isEmpty())
        return 0;
    return runs[run].re[0].size();
}

QString Dataset::unitForType(const QString &type)
{
    const QString t = type.toLower();
    if (t == QLatin1String("time"))
        return QStringLiteral("s");
    if (t == QLatin1String("frequency"))
        return QStringLiteral("Hz");
    if (t.contains(QLatin1String("noise")) && t.contains(QLatin1String("voltage")))
        return QStringLiteral("V/√Hz");
    if (t.contains(QLatin1String("noise")) && t.contains(QLatin1String("current")))
        return QStringLiteral("A/√Hz");
    if (t == QLatin1String("voltage"))
        return QStringLiteral("V");
    if (t == QLatin1String("current") || t == QLatin1String("device_current") || t == QLatin1String("subckt_current"))
        return QStringLiteral("A");
    if (t == QLatin1String("power"))
        return QStringLiteral("W");
    if (t == QLatin1String("impedance"))
        return QStringLiteral("Ω");
    if (t == QLatin1String("conductance") || t == QLatin1String("admittance"))
        return QStringLiteral("S");
    if (t == QLatin1String("temp-sweep") || t == QLatin1String("temperature"))
        return QStringLiteral("°C");
    return QString();
}

QString Dataset::xUnit() const
{
    return vars.isEmpty() ? QString() : unitFor(0);
}

void Dataset::rebuildIndex()
{
    index.clear();
    for (int i = 0; i < vars.size(); ++i)
        index.insert(vars[i].name.toLower(), i);
}

void Dataset::splitSteps()
{
    if (runs.size() != 1 || vars.isEmpty())
        return;
    const QVector<double> &xs = runs[0].re[0];
    if (xs.size() < 3)
        return;
    // A new step begins where the sweep variable returns to its first value
    // (transient: time back to start; AC: frequency back to start).
    const double first = xs[0];
    QList<int> starts{0};
    for (int i = 1; i < xs.size(); ++i) {
        if (xs[i] == first && xs[i - 1] != first)
            starts << i;
        else if (kind == Kind::Transient && xs[i] < xs[i - 1])
            starts << i;
    }
    if (starts.size() < 2)
        return;
    QList<Run> out;
    for (int s = 0; s < starts.size(); ++s) {
        const int a = starts[s];
        const int b = s + 1 < starts.size() ? starts[s + 1] : xs.size();
        Run r;
        r.re.resize(vars.size());
        if (complex)
            r.im.resize(vars.size());
        for (int v = 0; v < vars.size(); ++v) {
            r.re[v] = runs[0].re[v].mid(a, b - a);
            if (complex)
                r.im[v] = runs[0].im[v].mid(a, b - a);
        }
        r.label = QStringLiteral("step %1").arg(s + 1);
        out << r;
    }
    runs = out;
}

namespace rawfile {

Dataset::Kind kindFromPlotName(const QString &plotName)
{
    const QString p = plotName.toLower();
    if (p.contains(QLatin1String("transient")))
        return Dataset::Kind::Transient;
    if (p.contains(QLatin1String("ac analysis")) || p.startsWith(QLatin1String("ac ")))
        return Dataset::Kind::AC;
    if (p.contains(QLatin1String("dc transfer")) || p.contains(QLatin1String("dc sweep")))
        return Dataset::Kind::DCSweep;
    if (p.contains(QLatin1String("operating point")))
        return Dataset::Kind::OperatingPoint;
    if (p.contains(QLatin1String("noise")))
        return Dataset::Kind::Noise;
    if (p.contains(QLatin1String("transfer function")))
        return Dataset::Kind::TransferFunction;
    if (p.contains(QLatin1String("fft")))
        return Dataset::Kind::FFT;
    return Dataset::Kind::Unknown;
}

namespace {

// Reads header lines until "Binary:" or "Values:", handling both 8-bit and
// UTF-16LE encodings. Returns false at end of file.
struct HeaderReader {
    const QByteArray &data;
    qsizetype pos = 0;
    bool utf16 = false;

    bool readLine(QString &line)
    {
        if (pos >= data.size())
            return false;
        if (utf16) {
            QString s;
            while (pos + 1 < data.size()) {
                const char16_t c = char16_t(uchar(data[pos])) | (char16_t(uchar(data[pos + 1])) << 8);
                pos += 2;
                if (c == u'\n')
                    break;
                if (c != u'\r')
                    s.append(QChar(c));
            }
            line = s;
        } else {
            const qsizetype nl = data.indexOf('\n', pos);
            const qsizetype end = nl < 0 ? data.size() : nl;
            line = QString::fromLatin1(data.constData() + pos, end - pos);
            if (line.endsWith('\r'))
                line.chop(1);
            pos = end + 1;
        }
        return true;
    }
};

double readF64(const char *p) { return qFromLittleEndian<double>(p); }
double readF32(const char *p) { return double(qFromLittleEndian<float>(p)); }

} // namespace

QList<DatasetPtr> read(const QString &path, QString *error)
{
    QList<DatasetPtr> result;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return result;
    }
    const QByteArray data = f.readAll();
    HeaderReader hr{data};
    // LTspice writes UTF-16LE headers.
    if (data.size() > 4 && data[1] == 0 && data[3] == 0)
        hr.utf16 = true;

    while (hr.pos < data.size()) {
        auto ds = DatasetPtr::create();
        ds->sourceFile = path;
        int nVars = 0, nPoints = 0;
        QString flags;
        QStringList stepLabels;
        bool gotData = false, binary = false;
        QString line;
        bool inVars = false;
        while (hr.readLine(line)) {
            if (line.trimmed().isEmpty() && !inVars)
                continue;
            if (inVars) {
                const QStringList t = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (t.size() >= 3 && t[0].at(0).isDigit()) {
                    ds->vars.append({t[1], t[2]});
                    continue;
                }
                inVars = false;
            }
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QString key = line.left(colon).trimmed().toLower();
            const QString val = line.mid(colon + 1).trimmed();
            if (key == QLatin1String("title")) ds->title = val;
            else if (key == QLatin1String("date")) ds->date = val;
            else if (key == QLatin1String("plotname")) ds->plotName = val;
            else if (key == QLatin1String("flags")) flags = val.toLower();
            else if (key == QLatin1String("no. variables")) nVars = val.toInt();
            else if (key == QLatin1String("no. points")) nPoints = val.toInt();
            else if (key == QLatin1String("command")) ds->command = val;
            else if (key == QLatin1String("steps")) stepLabels = val.split(QStringLiteral("; "));
            else if (key == QLatin1String("variables")) inVars = true;
            else if (key == QLatin1String("binary")) { binary = true; gotData = true; break; }
            else if (key == QLatin1String("values")) { binary = false; gotData = true; break; }
        }
        if (!gotData || nVars <= 0)
            break;
        // Some writers put the first variable on the "Variables:" line itself.
        ds->complex = flags.contains(QLatin1String("complex"));
        ds->logScaleX = flags.contains(QLatin1String("log"));
        ds->linearX = flags.contains(QLatin1String("linear"));
        const bool allDouble = flags.contains(QLatin1String("double")) || !hr.utf16; // ngspice always writes doubles
        const bool fastAccess = flags.contains(QLatin1String("fastaccess"));
        ds->kind = kindFromPlotName(ds->plotName);
        while (ds->vars.size() < nVars)
            ds->vars.append({QStringLiteral("var%1").arg(ds->vars.size()), QString()});

        Dataset::Run run;
        run.re.resize(nVars);
        if (ds->complex)
            run.im.resize(nVars);
        for (int v = 0; v < nVars; ++v) {
            run.re[v].resize(nPoints);
            if (ds->complex)
                run.im[v].resize(nPoints);
        }

        if (binary) {
            // Value sizes.
            const int scaleSize = ds->complex ? 16 : 8;
            const int otherSize = ds->complex ? 16 : (allDouble ? 8 : 4);
            const qsizetype rowSize = scaleSize + qsizetype(nVars - 1) * otherSize;
            qsizetype available = (data.size() - hr.pos) / rowSize;
            if (available < nPoints) {
                nPoints = int(available); // truncated (simulation interrupted)
                for (int v = 0; v < nVars; ++v) {
                    run.re[v].resize(nPoints);
                    if (ds->complex)
                        run.im[v].resize(nPoints);
                }
            }
            const char *base = data.constData() + hr.pos;
            if (!fastAccess) {
                for (int p = 0; p < nPoints; ++p) {
                    const char *row = base + p * rowSize;
                    for (int v = 0; v < nVars; ++v) {
                        const char *cell = v == 0 ? row : row + scaleSize + qsizetype(v - 1) * otherSize;
                        if (ds->complex) {
                            run.re[v][p] = readF64(cell);
                            run.im[v][p] = readF64(cell + 8);
                        } else if (v == 0 || otherSize == 8) {
                            run.re[v][p] = readF64(cell);
                        } else {
                            run.re[v][p] = readF32(cell);
                        }
                    }
                }
            } else {
                // Column-major: all points of variable 0, then variable 1, ...
                qsizetype off = 0;
                for (int v = 0; v < nVars; ++v) {
                    const int sz = v == 0 ? scaleSize : otherSize;
                    for (int p = 0; p < nPoints; ++p) {
                        const char *cell = base + off;
                        if (ds->complex) {
                            run.re[v][p] = readF64(cell);
                            run.im[v][p] = readF64(cell + 8);
                        } else {
                            run.re[v][p] = sz == 8 ? readF64(cell) : readF32(cell);
                        }
                        off += sz;
                    }
                }
            }
            hr.pos += rowSize * nPoints;
            // LTspice marks compressed time points with a negative sign.
            if (ds->kind == Dataset::Kind::Transient && !ds->complex)
                for (double &t : run.re[0])
                    t = std::fabs(t);
        } else {
            // ASCII values: "index<tab>value" then subsequent values on their own lines;
            // complex values are "re,im".
            int p = 0, v = 0;
            QString l;
            while (p < nPoints && hr.readLine(l)) {
                QString s = l.trimmed();
                if (s.isEmpty())
                    continue;
                if (v == 0) {
                    const QStringList t = s.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                    if (t.size() < 2)
                        continue;
                    s = t[1];
                }
                if (ds->complex) {
                    const QStringList c = s.split(',');
                    run.re[v][p] = c.value(0).toDouble();
                    run.im[v][p] = c.value(1).toDouble();
                } else {
                    run.re[v][p] = s.toDouble();
                }
                if (++v == nVars) {
                    v = 0;
                    ++p;
                }
            }
            if (p < nPoints) {
                for (int k = 0; k < nVars; ++k) {
                    run.re[k].resize(p);
                    if (ds->complex)
                        run.im[k].resize(p);
                }
            }
        }
        // For AC data the frequency is stored as complex with zero imaginary part.
        ds->runs.append(run);
        ds->rebuildIndex();
        if (flags.contains(QLatin1String("stepped"))) {
            ds->splitSteps();
            for (int r = 0; r < ds->runs.size() && r < stepLabels.size(); ++r)
                ds->runs[r].label = stepLabels[r];
        }
        result.append(ds);
    }
    if (result.isEmpty() && error)
        *error = QStringLiteral("No data found in %1").arg(path);
    return result;
}

bool write(const QString &path, const Dataset &ds, bool binary, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    // All runs go into one plot; several runs are flagged "stepped" (as LTspice does).
    int total = 0;
    for (int r = 0; r < ds.runs.size(); ++r)
        total += ds.pointCount(r);
    QByteArray hdr;
    hdr += "Title: " + ds.title.toUtf8() + "\n";
    hdr += "Date: " + ds.date.toUtf8() + "\n";
    hdr += "Plotname: " + ds.plotName.toUtf8() + "\n";
    QByteArray flags = ds.complex ? "complex" : "real";
    if (ds.linearX)
        flags += " forward linear";
    else if (ds.logScaleX || ds.kind == Dataset::Kind::AC)
        flags += " forward log";
    if (ds.runs.size() > 1)
        flags += " stepped";
    hdr += "Flags: " + flags + "\n";
    hdr += "No. Variables: " + QByteArray::number(ds.vars.size()) + "\n";
    hdr += "No. Points: " + QByteArray::number(total) + "\n";
    if (ds.runs.size() > 1) {
        QStringList labels;
        for (const auto &run : ds.runs)
            labels << run.label;
        hdr += "Steps: " + labels.join(QStringLiteral("; ")).toUtf8() + "\n";
    }
    hdr += "Variables:\n";
    for (int v = 0; v < ds.vars.size(); ++v)
        hdr += "\t" + QByteArray::number(v) + "\t" + ds.vars[v].name.toUtf8() + "\t" + ds.vars[v].type.toUtf8() + "\n";
    hdr += binary ? "Binary:\n" : "Values:\n";
    if (binary) {
        // LTspice's binary layout: UTF-16LE header, scale as double, other real vectors as float32.
        const QString h = QString::fromUtf8(hdr);
        f.write(reinterpret_cast<const char *>(h.utf16()), h.size() * 2);
    } else {
        f.write(hdr);
    }
    int index = 0;
    for (int r = 0; r < ds.runs.size(); ++r) {
        const Dataset::Run &run = ds.runs[r];
        const int n = ds.pointCount(r);
        QByteArray buf;
        if (binary) {
            buf.reserve(qsizetype(n) * ds.vars.size() * (ds.complex ? 16 : 4) + 4);
            for (int p = 0; p < n; ++p) {
                for (int v = 0; v < ds.vars.size(); ++v) {
                    double re = run.re[v][p];
                    if (ds.complex) {
                        double im = run.im.isEmpty() ? 0.0 : run.im[v][p];
                        buf.append(reinterpret_cast<const char *>(&re), 8);
                        buf.append(reinterpret_cast<const char *>(&im), 8);
                    } else if (v == 0) {
                        buf.append(reinterpret_cast<const char *>(&re), 8);
                    } else {
                        const float f32 = float(re);
                        buf.append(reinterpret_cast<const char *>(&f32), 4);
                    }
                }
            }
        } else {
            for (int p = 0; p < n; ++p, ++index) {
                for (int v = 0; v < ds.vars.size(); ++v) {
                    buf += v == 0 ? QByteArray::number(index) + "\t" : QByteArray("\t");
                    if (ds.complex)
                        buf += QByteArray::number(run.re[v][p], 'g', 17) + "," + QByteArray::number(run.im.isEmpty() ? 0.0 : run.im[v][p], 'g', 17);
                    else
                        buf += QByteArray::number(run.re[v][p], 'g', 17);
                    buf += "\n";
                }
            }
        }
        f.write(buf);
    }
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace rawfile
