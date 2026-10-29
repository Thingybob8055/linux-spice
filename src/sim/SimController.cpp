#include "SimController.h"

#include "core/AppSettings.h"
#include "core/Expression.h"
#include "core/Fourier.h"
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
#include <algorithm>
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
    for (const QString &l : ltNetlist) // the netlister's notes, such as parts left out
        if (l.startsWith(QLatin1String("* WARNING: ")))
            m_preLog += QStringLiteral("Warning: %1\n").arg(l.mid(11));
    m_seedBase = 1;
    static const QRegularExpression seedRe(QStringLiteral("^\\.opt\\w*\\b.*\\bseed\\s*=\\s*(\\d+)"), QRegularExpression::CaseInsensitiveOption);
    for (const QString &l : ltNetlist)
        if (const auto m = seedRe.match(l.trimmed()); m.hasMatch())
            m_seedBase = m.captured(1).toUInt() + 1;
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

    // A .dc second sweep ends on its stop value in LTspice (".dc V1 0 1 .3 V2 0 1 .4" gives V2 =
    // 0, .4, .8, 1), which ngspice's nested sweep doesn't do; as a .step it gets LTspice's rule.
    QStringList netlist = ltNetlist;
    for (int i = 0; i < netlist.size(); ++i) {
        QStringList t = netlist[i].trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (t.isEmpty() || t[0].compare(QLatin1String(".dc"), Qt::CaseInsensitive) != 0)
            continue;
        const int first = (t.size() > 1 && t[1].compare(QLatin1String("lin"), Qt::CaseInsensitive) == 0) ? 2 : 1;
        if (t.size() == first + 8 && t[first].compare(QLatin1String("param"), Qt::CaseInsensitive) != 0) {
            netlist[i] = t.mid(0, first + 4).join(' ');
            netlist.insert(i + 1, QStringLiteral(".step %1").arg(t.mid(first + 4).join(' ')));
        }
        break;
    }
    StepPlan plan;
    QString err;
    if (!plan.parse(netlist, &err)) {
        failNow({err});
        return;
    }
    const int runs = plan.runCount();
    m_stepAxes = plan.axes();
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
        const QStringList lines = plan.isEmpty() ? netlist : plan.apply(netlist, r);
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
    m_mainRuns = int(m_jobs.size());
    m_netSpecs.clear();
    if (!m_translations.isEmpty() && m_translations[0].analysis == QLatin1String("ac")) {
        for (int r = 0; r < runs; ++r) {
            const QStringList lines = plan.isEmpty() ? netlist : plan.apply(netlist, r);
            QStringList errs;
            const QList<netparams::Spec> specs = netparams::parse(lines, &errs);
            if (r == 0)
                for (const QString &e : errs)
                    m_preLog += QStringLiteral("Warning: %1\n").arg(e);
            m_netSpecs << specs;
            for (int sp = 0; sp < specs.size(); ++sp) {
                for (int port = 1; port <= 2; ++port) {
                    NetlistTranslator tr;
                    const TranslationResult res = tr.translate(netparams::excitationNetlist(lines, specs[sp], port), opts);
                    if (!res.errors.isEmpty())
                        continue;
                    Job job;
                    job.index = int(m_jobs.size());
                    job.netStep = r;
                    job.netSpec = sp;
                    job.netPort = port;
                    job.netlistPath = m_tmp->filePath(QStringLiteral("net%1_%2_%3.cir").arg(r).arg(sp).arg(port));
                    job.rawPath = m_tmp->filePath(QStringLiteral("net%1_%2_%3.raw").arg(r).arg(sp).arg(port));
                    QFile f(job.netlistPath);
                    if (!f.open(QIODevice::WriteOnly))
                        continue;
                    f.write(res.netlist.toUtf8());
                    f.close();
                    m_translations << res;
                    m_jobs << job;
                }
            }
        }
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
        // Each run gets its own random seed so Monte Carlo steps differ, derived from the step
        // index so that re-running the same schematic gives the same results (as in LTspice).
        const quint32 seed = m_seedBase * 7919u + quint32(j.index) * 104729u + 1u;
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

// Longest run of consecutive time steps in which a node voltage alternates up and down by more
// than max(20mV, 2% of its range): trapezoidal ringing left after an edge was crossed in one step
// (the NE555 example's output sat 0.8V above its supply, zigzagging 1.8V every step). LTspice's
// own results never show more than 14 such steps; that one went on for 45.
static int ringingRun(const Dataset &ds)
{
    int best = 0;
    if (ds.kind != Dataset::Kind::Transient || ds.runs.isEmpty())
        return 0;
    for (int v = 1; v < ds.vars.size(); ++v) {
        if (ds.vars[v].type != QLatin1String("voltage") || ds.vars[v].name.contains(QLatin1String("ls_"), Qt::CaseInsensitive))
            continue;
        const QVector<double> &y = ds.runs[0].re[v];
        if (y.size() < 10)
            continue;
        const auto [lo, hi] = std::minmax_element(y.begin(), y.end());
        const double th = std::max(0.02, 0.02 * (*hi - *lo));
        int run = 0;
        for (int i = 2; i < y.size(); ++i) {
            const double d1 = y[i] - y[i - 1], d0 = y[i - 1] - y[i - 2];
            run = (d1 * d0 < 0 && std::abs(d1) > th && std::abs(d0) > th) ? run + 1 : 0;
            best = std::max(best, run);
        }
    }
    return best;
}

void SimController::onFinished(Job &job, int code)
{
    if (job.proc) {
        onOutput(job);
        job.proc->deleteLater();
    }
    // Convergence trouble: retry with convergence aids. First looser current tolerances
    // (ngspice checks branch currents to 1pA; a hard-on switch carrying nA at hundreds of volts
    // never meets that from rounding noise alone, where LTspice runs), then a shunt on every node
    // with more gmin/source steps and iterations.
    // The comparison locators (hidden "ls_loc" switches, see NetlistTranslator::crossingLocators)
    // are dropped before the last resort, in case they are what stalls the run.
    static const QStringList retryOptions = {
        QStringLiteral("-tmax"),
        QStringLiteral(".options abstol=1e-9 itl4=100"),
        QStringLiteral(".options abstol=1e-6 itl4=100"),
        QStringLiteral("-loc .options abstol=1e-9 itl4=100"),
        QStringLiteral("-loc .options rshunt=1e12 gminsteps=200 srcsteps=200 itl1=1000 itl2=500 itl4=100 method=gear"),
    };
    const bool failed = (job.exitCode < 0 ? code : job.exitCode) != 0 || !QFileInfo::exists(job.rawPath);
    const bool convergence = job.log.contains(QLatin1String("gmin stepping failed"), Qt::CaseInsensitive)
        || job.log.contains(QLatin1String("singular matrix"), Qt::CaseInsensitive)
        || job.log.contains(QLatin1String("timestep too small"), Qt::CaseInsensitive)
        || job.log.contains(QLatin1String("source stepping failed"), Qt::CaseInsensitive);
    if (failed && convergence && job.retries < retryOptions.size() && !m_aborted) {
        QFile f(job.netlistPath);
        if (job.originalNetlist.isEmpty() && f.open(QIODevice::ReadOnly)) {
            job.originalNetlist = QString::fromUtf8(f.readAll());
            f.close();
        }
        if (!job.originalNetlist.isEmpty()) {
            QString extra = retryOptions[job.retries++];
            QString text = job.originalNetlist;
            QString note = extra;
            // ngspice's smallest time step is 1e-11 x the maximum step, so a large user maximum
            // (".tran 0 0.5 0 1": 10ps) stops a switching circuit that LTspice runs; LTspice's
            // own default, Tstop/1024, keeps it in the femtoseconds.
            if (extra == QLatin1String("-tmax")) {
                static const QRegularExpression tranRe(QStringLiteral("^(\\.tran\\s+\\S+\\s+)(\\S+)(\\s+\\S+\\s+)(\\S+)"),
                                                       QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
                const auto m = tranRe.match(text);
                bool ok1 = false, ok2 = false;
                const double tstop = m.hasMatch() ? m.captured(2).toDouble(&ok1) : 0, tmax = m.hasMatch() ? m.captured(4).toDouble(&ok2) : 0;
                if (!ok1 || !ok2 || tmax <= tstop / 1024 * 1.0001) {
                    job.proc = nullptr;
                    onFinished(job, code); // nothing to change: go on to the next retry
                    return;
                }
                text.replace(m.capturedStart(4), m.capturedLength(4), QString::number(tstop / 1024, 'g', 12));
                job.originalNetlist = text; // later retries keep the smaller step
                extra.clear();
                note = QStringLiteral("the maximum time step reduced to Tstop/1024");
            }
            if (extra.startsWith(QLatin1String("-loc "))) {
                extra = extra.mid(5);
                static const QRegularExpression loc(QStringLiteral("^([BSR]ls_\\S+_loc\\d+ )"), QRegularExpression::MultilineOption);
                text.replace(loc, QStringLiteral("*\\1"));
                note = extra + QStringLiteral(" and no comparison locators");
            }
            const int endIdx = text.lastIndexOf(QLatin1String("\n.end"));
            if (extra.isEmpty())
                ;
            else if (endIdx >= 0)
                text.insert(endIdx, QLatin1Char('\n') + extra);
            else
                text += QLatin1Char('\n') + extra + QStringLiteral("\n.end\n");
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(text.toUtf8());
                f.close();
                job.log += QStringLiteral("\n*** Convergence failed; retrying with %1 ***\n").arg(note);
                job.proc = nullptr;
                job.exitCode = -1;
                job.percent = 0;
                launchNext();
                return;
            }
        }
    }
    // Sustained ringing: run again with a tighter time-step tolerance. LTspice resolves such edges
    // in nanosecond steps; ngspice at LTspice's trtol=2 can cross them in one step, and trtol=1
    // everywhere would detune LC oscillators that match LTspice at 2.
    if (!failed && !job.ringingChecked && !m_aborted) {
        job.ringingChecked = true;
        QFile f(job.netlistPath);
        QString text;
        if (f.open(QIODevice::ReadOnly)) {
            text = QString::fromUtf8(f.readAll());
            f.close();
        }
        if (text.contains(QLatin1String("\n.options trtol=2\n")) && QFileInfo(job.rawPath).size() < 300'000'000) {
            QString err;
            const QList<DatasetPtr> plots = rawfile::read(job.rawPath, &err);
            const int ring = plots.isEmpty() ? 0 : ringingRun(*plots.first());
            if (ring >= 25 && f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                text.replace(QLatin1String("\n.options trtol=2\n"), QLatin1String("\n.options trtol=0.5\n"));
                f.write(text.toUtf8());
                f.close();
                if (!job.originalNetlist.isEmpty())
                    job.originalNetlist.replace(QLatin1String("\n.options trtol=2\n"), QLatin1String("\n.options trtol=0.5\n"));
                job.log += QStringLiteral("\n*** Integration ringing after a fast edge (%1 steps); rerunning with .options trtol=0.5 ***\n").arg(ring);
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
            return parts.join(':') + ':' + tr.elementNames.value(leaf.toLower(), leaf).section(QChar(0xA7), -1);
        }
        QString orig = tr.elementNames.value(s.toLower(), s);
        // "M§Q1": a MOSFET whose instance name doesn't start with M; LTspice shows "Q1".
        if (const int sect = orig.indexOf(QChar(0xA7)); sect >= 0)
            orig = orig.mid(sect + 1);
        return orig;
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
    // Per-device output noise ("onoise_q1_rb"): LTspice names it V(Q1.rb); shot noise of a
    // terminal current is "s<current>", flicker noise "f<current>", the device total V(Q1).
    if (low.startsWith(QLatin1String("onoise_"))) {
        const QString rest = low.mid(7);
        QString dev = rest, src;
        static const QStringList sources = {QStringLiteral("_1overf"), QStringLiteral("_thermal"), QStringLiteral("_rb"), QStringLiteral("_rc"),
                                            QStringLiteral("_re"), QStringLiteral("_rs"), QStringLiteral("_rd"), QStringLiteral("_rg"),
                                            QStringLiteral("_ic"), QStringLiteral("_ib"), QStringLiteral("_id"), QStringLiteral("_ig")};
        for (const QString &sfx : sources) {
            if (rest.endsWith(sfx) && rest.size() > sfx.size()) {
                dev = rest.left(rest.size() - sfx.size());
                src = sfx.mid(1);
                break;
            }
        }
        if (isHelper(dev)) {
            // A helper resistor (e.g. a source's Rser) is the parent device's noise.
            static const QRegularExpression parent(QStringLiteral("^[a-z](.+)_[a-z]+$"));
            const auto pm = parent.match(dev);
            if (!src.isEmpty() || !pm.hasMatch())
                return QString();
            dev = pm.captured(1);
        }
        const QString name = origElem(dev);
        const QChar kind = name.isEmpty() ? QChar() : name[0].toLower();
        if (src.isEmpty())
            return QStringLiteral("V(%1)").arg(name);
        if (kind == 'r' || src == QLatin1String("thermal"))
            return QString(); // a resistor's parts: LTspice shows the total only
        if (src == QLatin1String("1overf"))
            src = kind == 'q' ? QStringLiteral("fib") : kind == 'd' ? QStringLiteral("fid") : QStringLiteral("fid");
        else if (src == QLatin1String("ic") || src == QLatin1String("ib") || (kind == 'd' && src == QLatin1String("id")))
            src = 's' + src;
        return QStringLiteral("V(%1.%2)").arg(name, src);
    }
    if (low.startsWith(QLatin1String("inoise_")) || low.startsWith(QLatin1String("onoise_total")))
        return QString();
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
        // In AC analysis these are operating-point values; AC currents come from probes.
        if (tr.analysis == QLatin1String("ac"))
            return QString();
        elem = m.captured(1);
        param = m.captured(2).toLower();
    } else {
        // Transmission line: ngspice's port-1 current is LTspice's Ia. Its other terminal
        // currents (Ib..Id) come from a different line model and have no ngspice counterpart.
        static const QRegularExpression tline(QStringLiteral("^(t[^#.]*)#i1$"));
        if (const auto tm = tline.match(low); tm.hasMatch() && tr.elementNames.contains(tm.captured(1)))
            return QStringLiteral("Ia(%1)").arg(origElem(tm.captured(1)));
        // Internal branches of devices ("t1#int2", "m_q1#gate") have no LTspice name.
        if (ng.contains('#'))
            return QString();
        // Plain node names in some plots (op/tf).
        return ng;
    }
    if (const auto pin = tr.pinCurrents.constFind(elem.toLower()); pin != tr.pinCurrents.constEnd())
        return *pin;
    // Probed device terminals ("q1:c#branch") -> Ic(Q1).
    static const QRegularExpression terminal(QStringLiteral("^([qmjz][^:.]*):([a-z_]+)$"));
    if (const auto tm = terminal.match(elem.toLower()); tm.hasMatch() && tr.elementNames.contains(tm.captured(1))) {
        if (tm.captured(2).size() != 1)
            return QString(); // internal terminals such as a VDMOS's "b_tj"
        return QStringLiteral("I%1(%2)").arg(tm.captured(2), origElem(tm.captured(1)));
    }
    if (isHelper(elem))
        return QString();
    QString name;
    if (!elem.contains('.') && !tr.elementNames.contains(elem.toLower())) {
        // Generated by the translator. The behavioral source that implements a controlled
        // source or switch ("be1", "be1_lap", "bs1") carries that element's current.
        static const QRegularExpression impl(QStringLiteral("^b(.+?)(_lap)?$"));
        const auto im = impl.match(elem.toLower());
        const QString orig = im.hasMatch() ? tr.elementNames.value(im.captured(1)) : QString();
        const bool isCurrent = param.isEmpty() || param == QLatin1String("i") || param == QLatin1String("c");
        if (orig.isEmpty() || !QStringLiteral("EGFHSW").contains(orig.section(QChar(0xA7), -1).left(1).toUpper()) || !isCurrent)
            return QString();
        name = origElem(im.captured(1));
    } else {
        name = origElem(elem);
    }
    const QChar kind = elem.section('.', -1).isEmpty() ? QChar() : elem.section('.', -1)[0].toLower();
    if (param.isEmpty() || param == QLatin1String("i") || param == QLatin1String("c") || param == QLatin1String("current"))
        return QStringLiteral("I(%1)").arg(name);
    if (kind == 'q' && (param == QLatin1String("ic") || param == QLatin1String("ib") || param == QLatin1String("ie") || param == QLatin1String("is")))
        return QStringLiteral("I%1(%2)").arg(param.mid(1), name);
    if ((kind == 'm' || kind == 'j' || kind == 'z') && (param == QLatin1String("id") || param == QLatin1String("ig") || param == QLatin1String("is") || param == QLatin1String("ib")))
        return QStringLiteral("I%1(%2)").arg(param.mid(1), name);
    if (kind == 'd' && param == QLatin1String("id"))
        return QStringLiteral("I(%1)").arg(name);
    if (kind == 'l' && param == QLatin1String("flux"))
        return QStringLiteral("%1:flux").arg(name);
    // Anything else (power, ngspice-only quantities such as a JFET's igd) has no LTspice
    // counterpart; power is computed on demand in the viewer.
    return QString();
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
    if (!m_translations.isEmpty())
        result.pinProbes = m_translations[0].pinProbes;
    QString log = m_preLog;

    DatasetPtr merged;
    QList<int> varMap; // merged var index -> run var index (per run computed)
    for (int i = 0; i < m_mainRuns; ++i) {
        Job &job = m_jobs[i];
        if (m_mainRuns > 1 && !job.log.isEmpty())
            log += QStringLiteral("\n--- step %1: %2 ---\n").arg(i + 1).arg(m_stepLabels.value(i));
        if (m_mainRuns == 1 || job.exitCode != 0)
            log += job.log;
        if (!QFileInfo::exists(job.rawPath)) {
            if (job.exitCode != -2)
                result.errors << QStringLiteral("Simulation%1 failed (exit code %2)")
                                     .arg(m_mainRuns > 1 ? QStringLiteral(" of step %1").arg(i + 1) : QString())
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
        // LTspice's name for the noise spectrum; .plt files key their sections by it.
        if (main->kind == Dataset::Kind::Noise && !main->vars.isEmpty() && main->vars[0].name.compare(QLatin1String("frequency"), Qt::CaseInsensitive) == 0)
            renamed.plotName = QStringLiteral("Noise Spectral Density - (V/Hz\u00bd or A/Hz\u00bd)");
        renamed.date = main->date;
        renamed.kind = main->kind;
        renamed.complex = main->complex;
        renamed.logScaleX = main->logScaleX;
        // ".ac lin ..." / ".noise ... lin ...": a linear frequency axis.
        static const QRegularExpression linRe(QStringLiteral("^\\.(ac|noise)\\b.*\\blin\\b"), QRegularExpression::CaseInsensitiveOption);
        renamed.linearX = main->linearX || linRe.match(tr.analysisLine.trimmed()).hasMatch();
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
        // A device's current includes its Rpar/Cpar (separate elements in the ngspice netlist).
        for (auto it = tr.parallelParts.constBegin(); it != tr.parallelParts.constEnd(); ++it) {
            const QString target = ltName(it.key() + QStringLiteral("#branch"), tr);
            int ti = -1;
            for (int k = 0; k < renamed.vars.size(); ++k)
                if (renamed.vars[k].name.compare(target, Qt::CaseInsensitive) == 0)
                    ti = k;
            if (ti < 0)
                continue;
            for (const QString &h : it.value()) {
                const QStringList forms = {QStringLiteral("@%1[i]").arg(h), QStringLiteral("@%1[id]").arg(h), QStringLiteral("%1#branch").arg(h),
                                           QStringLiteral("i(%1)").arg(h)};
                for (int v = 0; v < main->vars.size(); ++v) {
                    if (!forms.contains(main->vars[v].name.toLower()))
                        continue;
                    for (int q = 0; q < run.re[ti].size(); ++q) {
                        run.re[ti][q] += main->runs[0].re[v][q];
                        if (main->complex)
                            run.im[ti][q] += main->runs[0].im[v][q];
                    }
                    break;
                }
            }
        }
        for (const auto &[dname, from] : tr.derivedCurrents) {
            const int n = run.re.isEmpty() ? 0 : run.re[0].size();
            QVector<double> re(n, 0.0), im(n, 0.0);
            for (const QString &f : from) {
                for (int k = 0; k < renamed.vars.size(); ++k) {
                    if (renamed.vars[k].name.compare(f, Qt::CaseInsensitive) != 0)
                        continue;
                    for (int q = 0; q < n; ++q) {
                        re[q] -= run.re[k][q];
                        if (main->complex)
                            im[q] -= run.im[k][q];
                    }
                }
            }
            renamed.vars.append({dname, QStringLiteral("current")});
            run.re << re;
            if (main->complex)
                run.im << im;
        }
        for (const QString &d : tr.negatedDevices) {
            QString shown = tr.elementNames.value(d, d);
            shown = shown.section(QChar(0xA7), -1);
            for (int k = 0; k < renamed.vars.size(); ++k) {
                const QString &vn = renamed.vars[k].name;
                if (vn.size() < 4 || vn[0].toUpper() != 'I' || !vn.endsWith(QLatin1Char('(') + shown + QLatin1Char(')'), Qt::CaseInsensitive))
                    continue;
                for (double &x : run.re[k])
                    x = -x;
                if (main->complex)
                    for (double &x : run.im[k])
                        x = -x;
            }
        }
        static const QRegularExpression substrate(QStringLiteral("^Is\\((Q[^)]*)\\)$"), QRegularExpression::CaseInsensitiveOption);
        for (int k = 0; k < renamed.vars.size(); ++k) {
            const auto sm = substrate.match(renamed.vars[k].name);
            if (sm.hasMatch() && tr.noSubstrateBjts.contains(sm.captured(1).toLower())) {
                run.re[k].fill(0.0);
                if (main->complex)
                    run.im[k].fill(0.0);
            }
        }
        // LTspice's noise plot also has "gain", the transfer from the input source to the output.
        if (renamed.kind == Dataset::Kind::Noise && !renamed.complex) {
            int on = -1, in = -1;
            for (int v = 0; v < renamed.vars.size(); ++v) {
                if (renamed.vars[v].name.compare(QLatin1String("V(onoise)"), Qt::CaseInsensitive) == 0) on = v;
                if (renamed.vars[v].name.compare(QLatin1String("V(inoise)"), Qt::CaseInsensitive) == 0) in = v;
            }
            if (on > 0 && in > 0) {
                QVector<double> g(run.re[on].size());
                for (int k = 0; k < g.size(); ++k)
                    g[k] = run.re[in][k] != 0 ? run.re[on][k] / run.re[in][k] : 0.0;
                renamed.vars.insert(1, {QStringLiteral("gain"), QStringLiteral("gain")});
                run.re.insert(1, g);
            }
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
        // .net: two-port parameters from the excitation runs, added to each step's AC data.
        if (!m_netSpecs.isEmpty() && merged->kind == Dataset::Kind::AC) {
            auto portVoltages = [&](const Job &job, const netparams::Spec &spec, QVector<std::complex<double>> *v1,
                                    QVector<std::complex<double>> *v2) {
                QString err;
                const QList<DatasetPtr> plots = rawfile::read(job.rawPath, &err);
                const TranslationResult &tr = m_translations[job.index];
                DatasetPtr main = pickPlot(plots, tr.analysis);
                if (!main)
                    return false;
                Dataset named = *main;
                for (int v = 0; v < named.vars.size(); ++v)
                    if (const QString n = ltName(main->vars[v].name, tr); !n.isEmpty())
                        named.vars[v].name = n;
                named.rebuildIndex();
                DatasetContext ctx(named, 0);
                const QStringList sig = netparams::measuredSignals(spec);
                QVector<std::complex<double>> *outs[] = {v1, v2};
                for (int k = 0; k < 2; ++k) {
                    Expression e(sig[k]);
                    const auto val = e.evaluate(ctx);
                    if (!val)
                        return false;
                    outs[k]->clear();
                    for (int p = 0; p < val->size(); ++p)
                        *outs[k] << val->c(p);
                }
                return true;
            };
            bool namesAdded = false;
            for (int r = 0; r < merged->runs.size() && r < m_netSpecs.size(); ++r) {
                for (int sp = 0; sp < m_netSpecs[r].size(); ++sp) {
                    const netparams::Spec &spec = m_netSpecs[r][sp];
                    const Job *ja = nullptr, *jb = nullptr;
                    for (const Job &j : m_jobs)
                        if (j.netStep == r && j.netSpec == sp)
                            (j.netPort == 1 ? ja : jb) = &j;
                    QVector<std::complex<double>> a1, a2, b1, b2;
                    if (!ja || !jb || !portVoltages(*ja, spec, &a1, &a2) || !portVoltages(*jb, spec, &b1, &b2)) {
                        log += QStringLiteral("Warning: .net parameters for %1 could not be computed\n").arg(spec.source);
                        continue;
                    }
                    const netparams::Result nr = netparams::compute(spec, netparams::portData(spec, 1, a1, a2), netparams::portData(spec, 2, b1, b2));
                    const int npts = merged->runs[r].re.isEmpty() ? 0 : merged->runs[r].re[0].size();
                    for (int k = 0; k < nr.names.size(); ++k) {
                        int idx = -1;
                        for (int v = 0; v < merged->vars.size(); ++v)
                            if (merged->vars[v].name.compare(nr.names[k], Qt::CaseInsensitive) == 0)
                                idx = v;
                        if (idx < 0) {
                            merged->vars.append({nr.names[k], nr.types[k]});
                            idx = int(merged->vars.size()) - 1;
                            for (auto &run : merged->runs) {
                                run.re << QVector<double>(npts, qQNaN());
                                run.im << QVector<double>(npts, 0.0);
                            }
                            namesAdded = true;
                        }
                        QVector<double> re(npts, qQNaN()), im(npts, 0.0);
                        for (int p = 0; p < npts && p < nr.values[k].size(); ++p) {
                            re[p] = nr.values[k][p].real();
                            im[p] = nr.values[k][p].imag();
                        }
                        merged->runs[r].re[idx] = re;
                        merged->runs[r].im[idx] = im;
                    }
                }
            }
            if (namesAdded)
                merged->rebuildIndex();
        }
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
        // .four: harmonic analysis of the transient data, per step.
        if (!tr0.fourStatements.isEmpty() && merged->kind == Dataset::Kind::Transient) {
            for (int r = 0; r < merged->runs.size(); ++r) {
                const TranslationResult &tr = m_translations.value(r, tr0);
                QHash<QString, double> params;
                ParamContext pc;
                for (int pass = 0; pass < 4; ++pass)
                    for (auto it = tr.params.constBegin(); it != tr.params.constEnd(); ++it) {
                        bool ok = false;
                        const double v = Expression(it.value()).evaluateScalar(pc, &ok);
                        if (ok) {
                            params.insert(it.key(), v);
                            pc.vars.insert(it.key(), Value::scalar(v));
                        }
                    }
                const QString four = fourier::evaluate(*merged, r, tr0.fourStatements, params);
                if (four.isEmpty())
                    continue;
                if (merged->runs.size() > 1)
                    log += QStringLiteral("\n--- .four, step %1: %2 ---").arg(r + 1).arg(m_stepLabels.value(r));
                log += four;
            }
        }
        // .fra: extract the frequency response from the transient data.
        if (tr0.fra && merged->kind == Dataset::Kind::Transient) {
            for (const fra::Analyzer &a : tr0.fraAnalyzers) {
                if (!a.enabled)
                    continue;
                // Probes pair with analyzers by instance name; with one analyzer all probes belong to it.
                QList<fra::Probe> probes;
                for (const fra::Probe &p : tr0.fraProbes)
                    if (tr0.fraAnalyzers.size() == 1 || p.name.compare(a.name, Qt::CaseInsensitive) == 0)
                        probes << p;
                auto ds = DatasetPtr::create();
                ds->kind = Dataset::Kind::AC;
                ds->complex = true;
                ds->logScaleX = true;
                ds->plotName = QStringLiteral("Frequency Response Analysis");
                ds->title = merged->title;
                ds->date = merged->date;
                for (int r = 0; r < merged->runs.size(); ++r) {
                    DatasetContext ctx(*merged, r);
                    const fra::Result fr = fra::analyze(a, probes, merged->runs[r].re[0], [&ctx](const QString &expr) {
                        Expression e(expr);
                        const auto v = e.evaluate(ctx);
                        return v ? v->re : QVector<double>();
                    });
                    if (fr.freq.isEmpty())
                        continue;
                    if (ds->vars.isEmpty()) {
                        ds->vars = {{QStringLiteral("frequency"), QStringLiteral("frequency")},
                                    a.impedance ? Dataset::Variable{QStringLiteral("impedance_%1").arg(a.name), QStringLiteral("impedance")}
                                                : Dataset::Variable{QStringLiteral("gain_%1").arg(a.name), QStringLiteral("gain")},
                                    a.impedance ? Dataset::Variable{QStringLiteral("I(stimulus_%1)").arg(a.name), QStringLiteral("current")}
                                                : Dataset::Variable{QStringLiteral("V(stimulus_%1)").arg(a.name), QStringLiteral("voltage")}};
                        for (const auto &x : fr.extra)
                            ds->vars.append({x.first, QStringLiteral("gain")});
                    }
                    Dataset::Run run;
                    run.label = m_stepLabels.value(r);
                    run.re.resize(ds->vars.size());
                    run.im.resize(ds->vars.size());
                    for (int k = 0; k < fr.freq.size(); ++k) {
                        auto put = [&](int v, std::complex<double> c) {
                            run.re[v] << c.real();
                            run.im[v] << c.imag();
                        };
                        put(0, fr.freq[k]);
                        put(1, fr.gain[k]);
                        put(2, fr.stimulus[k]);
                        for (int x = 0; x < fr.extra.size(); ++x)
                            put(3 + x, fr.extra[x].second.value(k));
                    }
                    ds->runs << run;
                    if (!fr.summary.isEmpty())
                        log += (merged->runs.size() > 1 ? QStringLiteral("%1: ").arg(run.label) : QString()) + fr.summary + '\n';
                }
                if (ds->runs.isEmpty())
                    continue;
                ds->rebuildIndex();
                if (!m_outputBase.isEmpty()) {
                    QString werr;
                    rawfile::write(m_outputBase + QStringLiteral(".fra_%1.raw").arg(a.name), *ds, true, &werr);
                }
                result.fraData << ds;
                // LTspice plots the gain when neither terminal is grounded and there are no probes,
                // or the probe when one terminal is grounded and there is exactly one probe.
                const bool grounded = a.in == QLatin1String("0") || a.out == QLatin1String("0");
                QString autoPlot;
                if (!grounded && probes.isEmpty())
                    autoPlot = ds->vars.value(1).name;
                else if (grounded && probes.size() == 1)
                    autoPlot = QStringLiteral("probe_%1").arg(probes[0].name);
                result.fraAutoPlot << autoPlot;
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
        // A single .step sweep of a one-point analysis (.op, single-frequency .noise/.ac) is one
        // trace against the stepped quantity in LTspice, e.g. noise vs. a resistor value.
        if (m_stepAxes.size() == 1 && merged->runs.size() > 1 && merged->runs.size() == m_stepAxes[0].values.size()) {
            bool onePoint = true;
            for (const auto &run : merged->runs)
                if (run.re.isEmpty() || run.re[0].size() != 1)
                    onePoint = false;
            if (onePoint) {
                const StepAxis &ax = m_stepAxes[0];
                // Operating-point data has no scale vector; the others replace theirs.
                const bool hasScale = merged->kind != Dataset::Kind::OperatingPoint && merged->kind != Dataset::Kind::TransferFunction;
                QString xName = ax.type == StepAxis::Type::Temp ? QStringLiteral("temperature") : ax.label().toLower();
                Dataset::Run run;
                run.label.clear();
                const int first = hasScale ? 1 : 0;
                run.re << ax.values;
                if (merged->complex)
                    run.im << QVector<double>(ax.values.size(), 0.0);
                for (int v = first; v < merged->vars.size(); ++v) {
                    QVector<double> re, im;
                    for (const auto &r : merged->runs) {
                        re << r.re[v][0];
                        if (merged->complex)
                            im << (r.im.isEmpty() ? 0.0 : r.im[v][0]);
                    }
                    run.re << re;
                    if (merged->complex)
                        run.im << im;
                }
                QList<Dataset::Variable> vars{{xName, ax.type == StepAxis::Type::Temp ? QStringLiteral("temperature") : QStringLiteral("parameter")}};
                vars << merged->vars.mid(first);
                merged->vars = vars;
                merged->runs = {run};
                merged->logScaleX = false;
                merged->linearX = true;
                if (!hasScale)
                    merged->kind = Dataset::Kind::DCSweep; // plotted like a sweep
                merged->rebuildIndex();
                result.stepLabels.clear();
            }
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
