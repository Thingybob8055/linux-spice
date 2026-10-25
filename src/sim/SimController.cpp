#include "SimController.h"

#include "core/AppSettings.h"
#include "core/Measure.h"
#include "core/StepSweep.h"
#include "core/TextIO.h"
#include "core/Units.h"

#include <QRandomGenerator>
#include <QTimer>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>

#include <csignal>
#include <cmath>

SimController::SimController(QObject *parent)
    : QObject(parent)
{
}

SimController::~SimController()
{
    for (Job &j : m_jobs) {
        if (j.proc) {
            j.proc->disconnect(this);
            if (m_paused)
                ::kill(pid_t(j.proc->processId()), SIGCONT);
            j.proc->kill();
            j.proc->waitForFinished(1000);
        }
    }
}

QString SimController::workerProgram() const
{
    return QCoreApplication::applicationFilePath();
}

void SimController::start(const QStringList &ltNetlist, const QString &baseDir, const QString &outputBase)
{
    if (m_running)
        return;
    m_running = true;
    m_paused = false;
    m_aborted = false;
    m_jobs.clear();
    m_translations.clear();
    m_stepLabels.clear();
    m_outputBase = outputBase;
    m_baseDir = baseDir;
    m_ltNetlist = ltNetlist;
    m_preLog.clear();
    m_timer.start();
    m_tmp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/linspice-XXXXXX"));

    auto failNow = [this](const QStringList &errors) {
        SimResult r;
        r.errors = errors;
        r.log = m_preLog + errors.join('\n');
        m_running = false;
        emit logText(r.log);
        emit finished(r);
    };

    // Write the LTspice-style netlist next to the schematic, like LTspice does.
    if (!m_outputBase.isEmpty()) {
        textio::TextFile tf;
        tf.text = ltNetlist.join('\n') + '\n';
        tf.encoding = textio::Encoding::Latin1;
        textio::writeFile(m_outputBase + QStringLiteral(".net"), tf);
    }

    StepPlan plan;
    QString err;
    if (!plan.parse(ltNetlist, &err)) {
        failNow({err});
        return;
    }
    const int runs = plan.runCount();
    if (runs > 5000) {
        failNow({QStringLiteral("Too many stepped runs (%1)").arg(runs)});
        return;
    }

    NetlistTranslator::Options opts;
    opts.baseDir = baseDir;
    opts.inductorRser = AppSettings::instance().defaultInductorRser();
    opts.saveCurrents = AppSettings::instance().saveDeviceCurrents();
    opts.extraOptions = AppSettings::instance().defaultSolverOptions();

    for (int r = 0; r < runs; ++r) {
        const QStringList lines = plan.isEmpty() ? ltNetlist : plan.apply(ltNetlist, r);
        NetlistTranslator tr;
        TranslationResult res = tr.translate(lines, opts);
        if (!res.errors.isEmpty()) {
            if (r == 0) {
                for (const QString &w : res.warnings)
                    m_preLog += QStringLiteral("Warning: %1\n").arg(w);
                failNow(res.errors);
                return;
            }
        }
        if (r == 0)
            for (const QString &w : res.warnings)
                m_preLog += QStringLiteral("Warning: %1\n").arg(w);
        Job job;
        job.index = r;
        job.netlistPath = m_tmp->filePath(QStringLiteral("run%1.cir").arg(r));
        job.rawPath = m_tmp->filePath(QStringLiteral("run%1.raw").arg(r));
        QFile f(job.netlistPath);
        if (!f.open(QIODevice::WriteOnly)) {
            failNow({QStringLiteral("Cannot write temporary netlist %1").arg(job.netlistPath)});
            return;
        }
        f.write(res.netlist.toUtf8());
        f.close();
        m_translations << res;
        m_jobs << job;
        m_stepLabels << (plan.isEmpty() ? QString() : plan.runLabel(r));
    }
    if (!m_preLog.isEmpty())
        emit logText(m_preLog);

    m_maxParallel = std::max(1, AppSettings::instance().maxThreads());
    emit started(runs);
    launchNext();
}

void SimController::launchNext()
{
    int running = 0;
    for (const Job &j : m_jobs)
        if (j.proc && !j.done)
            ++running;
    for (int i = 0; i < m_jobs.size() && running < m_maxParallel; ++i) {
        Job &j = m_jobs[i];
        if (j.proc || j.done)
            continue;
        auto *p = new QProcess(this);
        j.proc = p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString lib = AppSettings::instance().ngspiceLibrary();
        if (!lib.isEmpty())
            env.insert(QStringLiteral("LINSPICE_NGSPICE_LIB"), lib);
        p->setProcessEnvironment(env);
        p->setProcessChannelMode(QProcess::MergedChannels);
        p->setWorkingDirectory(m_baseDir.isEmpty() ? m_tmp->path() : m_baseDir);
        const int idx = i;
        connect(p, &QProcess::readyReadStandardOutput, this, [this, idx]() { onOutput(m_jobs[idx]); });
        connect(p, &QProcess::finished, this, [this, idx](int code, QProcess::ExitStatus st) {
            onFinished(m_jobs[idx], st == QProcess::CrashExit ? 139 : code);
        });
        // Each run gets its own random seed so Monte Carlo steps differ.
        const quint32 seed = QRandomGenerator::global()->bounded(1u, 2000000000u);
        p->start(workerProgram(), {QStringLiteral("--sim-worker"), j.netlistPath, j.rawPath, QStringLiteral("--seed"), QString::number(seed)});
        if (m_paused)
            ::kill(pid_t(p->processId()), SIGSTOP);
        ++running;
    }
}

void SimController::onOutput(Job &job)
{
    if (!job.proc)
        return;
    job.lineBuffer += QString::fromUtf8(job.proc->readAllStandardOutput());
    int nl;
    QString logChunk;
    while ((nl = job.lineBuffer.indexOf('\n')) >= 0) {
        const QString line = job.lineBuffer.left(nl);
        job.lineBuffer.remove(0, nl + 1);
        if (line.startsWith(QLatin1String("LOG "))) {
            const QString text = line.mid(4);
            // Skip ngspice banner noise.
            if (text.startsWith(QLatin1String("**")) || text.trimmed().isEmpty())
                continue;
            job.log += text + '\n';
            if (m_jobs.size() == 1)
                logChunk += text + '\n';
        } else if (line.startsWith(QLatin1String("PROGRESS "))) {
            job.percent = line.mid(9).toDouble();
            double total = 0;
            for (const Job &j : m_jobs)
                total += j.done ? 100.0 : j.percent;
            total /= m_jobs.size();
            QString status = m_jobs.size() > 1 ? QStringLiteral("step %1/%2").arg(job.index + 1).arg(m_jobs.size()) : QString();
            emit progress(total, status);
        } else if (line.startsWith(QLatin1String("STAT "))) {
            // status text (already reflected by PROGRESS)
        } else if (line.startsWith(QLatin1String("DONE "))) {
            job.exitCode = line.mid(5).toInt();
        }
    }
    if (!logChunk.isEmpty())
        emit logText(logChunk);
}

void SimController::onFinished(Job &job, int code)
{
    if (job.proc) {
        onOutput(job);
        job.proc->deleteLater();
    }
    // Convergence trouble: retry once with stronger convergence aids
    // (shunt resistance on every node, more gmin/source steps, more iterations).
    const bool failed = (job.exitCode < 0 ? code : job.exitCode) != 0 || !QFileInfo::exists(job.rawPath);
    const bool convergence = job.log.contains(QLatin1String("gmin stepping failed")) || job.log.contains(QLatin1String("singular matrix"))
        || job.log.contains(QLatin1String("timestep too small")) || job.log.contains(QLatin1String("source stepping failed"));
    if (failed && convergence && !job.retried && !m_aborted) {
        job.retried = true;
        QFile f(job.netlistPath);
        if (f.open(QIODevice::ReadOnly)) {
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            const int endIdx = text.lastIndexOf(QLatin1String("\n.end"));
            const QString extra = QStringLiteral("\n.options rshunt=1e12 gminsteps=200 srcsteps=200 itl1=1000 itl2=500 itl4=100 method=gear");
            if (endIdx >= 0)
                text.insert(endIdx, extra);
            else
                text += extra + QStringLiteral("\n.end\n");
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(text.toUtf8());
                f.close();
                job.log += QStringLiteral("\n*** Convergence failed; retrying with rshunt=1e12 and extended gmin/source stepping ***\n");
                job.proc = nullptr;
                job.exitCode = -1;
                job.percent = 0;
                launchNext();
                return;
            }
        }
    }
    job.done = true;
    if (job.exitCode < 0)
        job.exitCode = code;
    int done = 0;
    for (const Job &j : m_jobs)
        if (j.done)
            ++done;
    emit progress(100.0 * done / m_jobs.size(), m_jobs.size() > 1 ? QStringLiteral("%1/%2 runs").arg(done).arg(m_jobs.size()) : QString());
    if (done == m_jobs.size())
        assemble();
    else if (!m_aborted)
        launchNext();
}

void SimController::pause()
{
    if (!m_running || m_paused)
        return;
    m_paused = true;
    for (Job &j : m_jobs)
        if (j.proc && !j.done)
            ::kill(pid_t(j.proc->processId()), SIGSTOP);
}

void SimController::resume()
{
    if (!m_running || !m_paused)
        return;
    m_paused = false;
    for (Job &j : m_jobs)
        if (j.proc && !j.done)
            ::kill(pid_t(j.proc->processId()), SIGCONT);
}

void SimController::stop()
{
    if (!m_running)
        return;
    m_aborted = true;
    if (m_paused)
        resume();
    // Jobs that never started are dropped.
    for (Job &j : m_jobs) {
        if (!j.proc && !j.done) {
            j.done = true;
            j.exitCode = -2;
        }
    }
    for (Job &j : m_jobs) {
        if (j.proc && !j.done) {
            j.proc->write("halt\n");
            QPointer<QProcess> p = j.proc;
            QTimer::singleShot(3000, this, [p]() {
                if (p && p->state() != QProcess::NotRunning)
                    p->kill();
            });
        }
    }
    bool anyRunning = false;
    for (const Job &j : m_jobs)
        if (!j.done)
            anyRunning = true;
    if (!anyRunning)
        assemble();
}

QString SimController::ltName(const QString &ngNameIn, const TranslationResult &tr)
{
    const QString ng = ngNameIn.trimmed();
    const QString low = ng.toLower();
    if (low.contains(QLatin1String("ls_")))
        return QString();
    auto origNode = [&](const QString &n) -> QString {
        QString s = n;
        // Subcircuit hierarchy: ngspice "xu1.n001" -> "xu1:n001"
        if (s.contains('.') && s.startsWith('x', Qt::CaseInsensitive))
            s.replace('.', ':');
        return tr.nodeNames.value(s.toLower(), s);
    };
    auto origElem = [&](const QString &n) -> QString {
        QString s = n;
        if (s.contains('.')) {
            // ngspice: "r.xu1.r1" (type letter prefix) -> "xu1:r1"
            QStringList parts = s.split('.');
            if (parts.size() >= 3 && parts[0].size() == 1)
                parts.removeFirst();
            const QString leaf = parts.takeLast();
            return parts.join(':') + ':' + tr.elementNames.value(leaf.toLower(), leaf);
        }
        return tr.elementNames.value(s.toLower(), s);
    };
    // Helper elements generated by the translator are hidden.
    auto isHelper = [&](const QString &elem) {
        static const QRegularExpression helper(QStringLiteral("^[a-z](.+)_(rser|lser|cpar|rpar|jumper|load|q|qb|qo|qbo|qt|qbt|ota|rout|cout|var|s|m|sl|r|s\\d+)$"));
        const auto m = helper.match(elem.toLower().section('.', -1));
        return m.hasMatch() && tr.elementNames.contains(m.captured(1));
    };

    if (low == QLatin1String("time") || low == QLatin1String("frequency"))
        return low;
    if (low.endsWith(QLatin1String("-sweep"))) {
        if (low.startsWith(QLatin1String("temp")))
            return QStringLiteral("temp");
        return tr.dcSweepSource.isEmpty() ? ng : tr.dcSweepSource;
    }
    if (low == QLatin1String("onoise_spectrum"))
        return QStringLiteral("V(onoise)");
    if (low == QLatin1String("inoise_spectrum"))
        return QStringLiteral("V(inoise)");
    static const QRegularExpression vRe(QStringLiteral("^v\\((.+)\\)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression iRe(QStringLiteral("^i\\((.+)\\)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression branchRe(QStringLiteral("^(.+)#branch$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression devRe(QStringLiteral("^@(.+)\\[(\\w+)\\]$"), QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m;
    if ((m = vRe.match(ng)).hasMatch())
        return QStringLiteral("V(%1)").arg(origNode(m.captured(1)));
    QString elem;
    QString param;
    if ((m = iRe.match(ng)).hasMatch())
        elem = m.captured(1);
    else if ((m = branchRe.match(ng)).hasMatch())
        elem = m.captured(1);
    else if ((m = devRe.match(ng)).hasMatch()) {
        elem = m.captured(1);
        param = m.captured(2).toLower();
    } else {
        // Plain node names in some plots (op/tf).
        return ng;
    }
    if (isHelper(elem))
        return QString();
    const QString name = origElem(elem);
    const QChar kind = elem.section('.', -1).isEmpty() ? QChar() : elem.section('.', -1)[0].toLower();
    if (param.isEmpty() || param == QLatin1String("i") || param == QLatin1String("c"))
        return QStringLiteral("I(%1)").arg(name);
    if (kind == 'q' && (param == QLatin1String("ic") || param == QLatin1String("ib") || param == QLatin1String("ie") || param == QLatin1String("is")))
        return QStringLiteral("I%1(%2)").arg(param.mid(1), name);
    if ((kind == 'm' || kind == 'j' || kind == 'z') && (param == QLatin1String("id") || param == QLatin1String("ig") || param == QLatin1String("is") || param == QLatin1String("ib")))
        return QStringLiteral("I%1(%2)").arg(param.mid(1), name);
    if (kind == 'd' && param == QLatin1String("id"))
        return QStringLiteral("I(%1)").arg(name);
    if (param == QLatin1String("p"))
        return QString(); // power vectors are computed on demand in the viewer
    return QStringLiteral("%1(%2)").arg(param, name);
}

namespace {

QString typeFor(const QString &ltName, const QString &ngType)
{
    if (ltName.startsWith(QLatin1String("V("), Qt::CaseInsensitive))
        return QStringLiteral("voltage");
    if (ltName.startsWith('I', Qt::CaseInsensitive) && ltName.contains('('))
        return QStringLiteral("current");
    return ngType;
}

DatasetPtr pickPlot(const QList<DatasetPtr> &plots, const QString &analysis)
{
    Dataset::Kind want = Dataset::Kind::Unknown;
    if (analysis == QLatin1String("tran")) want = Dataset::Kind::Transient;
    else if (analysis == QLatin1String("ac")) want = Dataset::Kind::AC;
    else if (analysis == QLatin1String("dc")) want = Dataset::Kind::DCSweep;
    else if (analysis == QLatin1String("op")) want = Dataset::Kind::OperatingPoint;
    else if (analysis == QLatin1String("noise")) want = Dataset::Kind::Noise;
    else if (analysis == QLatin1String("tf")) want = Dataset::Kind::TransferFunction;
    DatasetPtr best;
    for (const DatasetPtr &p : plots) {
        if (p->kind == want) {
            // Noise: the spectrum plot has more than one point.
            if (want == Dataset::Kind::Noise && p->pointCount() <= 1 && best)
                continue;
            if (want == Dataset::Kind::Noise && best && best->pointCount() > 1)
                continue;
            best = p;
        }
    }
    if (!best && !plots.isEmpty())
        best = plots.last();
    return best;
}

} // namespace

void SimController::assemble()
{
    SimResult result;
    result.elapsed = m_timer.elapsed() / 1000.0;
    result.aborted = m_aborted;
    result.analysis = m_translations.isEmpty() ? QString() : m_translations[0].analysis;
    result.stepLabels = m_stepLabels;
    QString log = m_preLog;

    DatasetPtr merged;
    QList<int> varMap; // merged var index -> run var index (per run computed)
    for (int i = 0; i < m_jobs.size(); ++i) {
        Job &job = m_jobs[i];
        if (m_jobs.size() > 1 && !job.log.isEmpty())
            log += QStringLiteral("\n--- step %1: %2 ---\n").arg(i + 1).arg(m_stepLabels.value(i));
        if (m_jobs.size() == 1 || job.exitCode != 0)
            log += job.log;
        if (!QFileInfo::exists(job.rawPath)) {
            if (job.exitCode != -2)
                result.errors << QStringLiteral("Simulation%1 failed (exit code %2)")
                                     .arg(m_jobs.size() > 1 ? QStringLiteral(" of step %1").arg(i + 1) : QString())
                                     .arg(job.exitCode);
            continue;
        }
        QString err;
        const QList<DatasetPtr> plots = rawfile::read(job.rawPath, &err);
        if (plots.isEmpty()) {
            result.errors << err;
            continue;
        }
        const TranslationResult &tr = m_translations[i];
        // Operating point (from the first run).
        if (i == 0) {
            for (const DatasetPtr &p : plots) {
                if (p->kind == Dataset::Kind::OperatingPoint || p->kind == Dataset::Kind::TransferFunction) {
                    for (int v = 0; v < p->vars.size(); ++v) {
                        const QString n = ltName(p->vars[v].name, tr);
                        if (!n.isEmpty() && p->pointCount() > 0)
                            result.operatingPoint.insert(n, p->runs[0].re[v][0]);
                    }
                }
            }
        }
        DatasetPtr main = pickPlot(plots, tr.analysis);
        if (!main)
            continue;
        // Rename and filter vectors.
        Dataset renamed;
        renamed.title = main->title;
        renamed.plotName = main->plotName;
        renamed.date = main->date;
        renamed.kind = main->kind;
        renamed.complex = main->complex;
        renamed.logScaleX = main->logScaleX;
        QList<int> keep;
        for (int v = 0; v < main->vars.size(); ++v) {
            const QString n = v == 0 ? ltName(main->vars[0].name, tr) : ltName(main->vars[v].name, tr);
            if (n.isEmpty() && v != 0)
                continue;
            bool dup = false;
            for (const auto &existing : renamed.vars)
                if (existing.name.compare(n, Qt::CaseInsensitive) == 0)
                    dup = true;
            if (dup)
                continue;
            renamed.vars.append({n.isEmpty() ? main->vars[v].name : n, typeFor(n, main->vars[v].type)});
            keep << v;
        }
        Dataset::Run run;
        for (int v : keep) {
            run.re << main->runs[0].re[v];
            if (main->complex)
                run.im << main->runs[0].im[v];
        }
        // AC frequency is stored complex; keep the real part only.
        run.label = m_stepLabels.value(i);
        if (!merged) {
            merged = DatasetPtr::create(renamed);
            merged->runs.clear();
        }
        // Align variables by name with the merged dataset.
        Dataset::Run aligned;
        aligned.label = run.label;
        for (int mv = 0; mv < merged->vars.size(); ++mv) {
            int src = -1;
            for (int k = 0; k < renamed.vars.size(); ++k)
                if (renamed.vars[k].name.compare(merged->vars[mv].name, Qt::CaseInsensitive) == 0)
                    src = k;
            const int n = run.re.isEmpty() ? 0 : run.re[0].size();
            aligned.re << (src >= 0 ? run.re[src] : QVector<double>(n, qQNaN()));
            if (merged->complex)
                aligned.im << (src >= 0 && !run.im.isEmpty() ? run.im[src] : QVector<double>(n, 0.0));
        }
        merged->runs << aligned;
    }
    Q_UNUSED(varMap);

    if (merged) {
        merged->rebuildIndex();
        merged->title = QFileInfo(m_outputBase).fileName();
        merged->date = QDateTime::currentDateTime().toString();
        result.data = merged;
        // .meas evaluation in post-processing, per step.
        const TranslationResult &tr0 = m_translations[0];
        if (!tr0.measStatements.isEmpty()) {
            MeasureEngine me;
            if (tr0.measComplexFormat == QLatin1String("polar"))
                me.complexFormat = MeasureEngine::ComplexFormat::Polar;
            else if (tr0.measComplexFormat == QLatin1String("cartesian"))
                me.complexFormat = MeasureEngine::ComplexFormat::Cartesian;
            QList<QList<MeasResult>> perRun;
            for (int r = 0; r < merged->runs.size(); ++r) {
                // Numeric parameters for PARAM expressions.
                QHash<QString, double> params;
                const TranslationResult &tr = m_translations.value(r, tr0);
                ParamContext pc;
                for (int pass = 0; pass < 4; ++pass) {
                    for (auto it = tr.params.constBegin(); it != tr.params.constEnd(); ++it) {
                        Expression e(it.value());
                        bool ok = false;
                        const double v = e.evaluateScalar(pc, &ok);
                        if (ok) {
                            params.insert(it.key(), v);
                            pc.vars.insert(it.key(), Value::scalar(v));
                        }
                    }
                }
                perRun << me.evaluate(*merged, r, tr0.measStatements, params);
            }
            const QString measLog = MeasureEngine::formatLog(perRun, m_stepLabels);
            if (!measLog.isEmpty()) {
                log += QStringLiteral("\n") + measLog + QStringLiteral("\n");
                result.measLines = measLog.split('\n');
            }
        }
        // .wave output
        for (const QString &w : tr0.waveStatements) {
            // .wave "file.wav" <bits> <rate> V(a) [V(b) ...]
            static const QRegularExpression waveRe(QStringLiteral("^\\.wave\\s+(\"[^\"]+\"|\\S+)\\s+(\\d+)\\s+(\\S+)\\s+(.+)$"),
                                                   QRegularExpression::CaseInsensitiveOption);
            const auto m = waveRe.match(w.trimmed());
            if (!m.hasMatch() || merged->kind != Dataset::Kind::Transient)
                continue;
            QString file = m.captured(1);
            file.remove('"');
            if (QFileInfo(file).isRelative())
                file = QDir(m_baseDir).filePath(file);
            const int bits = m.captured(2).toInt();
            const int rate = int(units::parseSpiceNumber(m.captured(3)).value_or(44100));
            QList<QVector<double>> chans;
            DatasetContext ctx(*merged, 0);
            for (const QString &sig : m.captured(4).split(QRegularExpression(QStringLiteral("\\s+(?![^(]*\\))")), Qt::SkipEmptyParts)) {
                Expression e(sig);
                auto v = e.evaluate(ctx);
                if (v)
                    chans << v->re;
            }
            QString werr;
            if (!chans.isEmpty() && writeWaveFile(file, merged->runs[0].re[0], chans, rate, bits, &werr))
                log += QStringLiteral("Wrote %1\n").arg(file);
            else if (!werr.isEmpty())
                log += QStringLiteral("Could not write %1: %2\n").arg(file, werr);
        }
        if (!m_outputBase.isEmpty()) {
            result.rawPath = m_outputBase + QStringLiteral(".raw");
            QString werr;
            Dataset copy = *merged;
            rawfile::write(result.rawPath, copy, true, &werr);
        }
    }
    log += QStringLiteral("\nTotal elapsed time: %1 seconds.\n").arg(result.elapsed, 0, 'f', 3);
    if (m_aborted)
        log += QStringLiteral("Simulation stopped by user.\n");
    result.log = log;
    result.ok = merged != nullptr && (result.errors.isEmpty() || m_aborted || merged->runs.size() > 0);
    if (!m_outputBase.isEmpty()) {
        QFile f(m_outputBase + QStringLiteral(".log"));
        if (f.open(QIODevice::WriteOnly))
            f.write(log.toUtf8());
    }
    m_running = false;
    m_paused = false;
    emit finished(result);
}

bool writeWaveFile(const QString &path, const QVector<double> &t, const QList<QVector<double>> &channels, int sampleRate, int bits,
                   QString *error)
{
    if (t.size() < 2 || channels.isEmpty() || sampleRate <= 0) {
        if (error)
            *error = QStringLiteral("nothing to write");
        return false;
    }
    bits = bits <= 8 ? 8 : (bits <= 16 ? 16 : (bits <= 24 ? 24 : 32));
    const double duration = t.last() - t.first();
    const qint64 frames = qint64(duration * sampleRate);
    const int nch = channels.size();
    const int bytesPer = bits / 8;
    QByteArray data;
    data.reserve(frames * nch * bytesPer);
    QVector<int> idx(nch, 0);
    for (qint64 f = 0; f < frames; ++f) {
        const double ti = t.first() + double(f) / sampleRate;
        for (int c = 0; c < nch; ++c) {
            const QVector<double> &y = channels[c];
            int &k = idx[c];
            while (k + 1 < t.size() - 1 && t[k + 1] < ti)
                ++k;
            const double ta = t[k], tb = t[k + 1];
            const double v = tb > ta ? y[k] + (y[k + 1] - y[k]) * (ti - ta) / (tb - ta) : y[k];
            // LTspice maps -1..+1 V to full scale.
            const double clipped = std::clamp(v, -1.0, 1.0);
            if (bits == 8) {
                data.append(char(quint8(std::lround((clipped + 1.0) * 127.5))));
            } else {
                const qint64 maxv = (qint64(1) << (bits - 1)) - 1;
                const qint32 s = qint32(std::llround(clipped * maxv));
                for (int b = 0; b < bytesPer; ++b)
                    data.append(char((s >> (8 * b)) & 0xff));
            }
        }
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    auto u32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        f.write(b, 4);
    };
    auto u16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        f.write(b, 2);
    };
    f.write("RIFF");
    u32(36 + quint32(data.size()));
    f.write("WAVEfmt ");
    u32(16);
    u16(1);
    u16(quint16(nch));
    u32(quint32(sampleRate));
    u32(quint32(sampleRate * nch * bytesPer));
    u16(quint16(nch * bytesPer));
    u16(quint16(bits));
    f.write("data");
    u32(quint32(data.size()));
    f.write(data);
    return f.commit();
}
