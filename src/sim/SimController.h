#pragma once

#include "core/NetParams.h"
#include "core/NetlistTranslator.h"
#include "core/RawFile.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

struct SimResult {
    bool ok = false;
    bool aborted = false;
    DatasetPtr data;                      // main analysis (all steps)
    QHash<QString, double> operatingPoint; // V(node)/I(dev) -> value (from .op or the initial solution)
    QString log;                          // full log text (also written to <name>.log)
    QStringList errors;
    QString rawPath;                      // LTspice-style .raw written next to the schematic
    QString analysis;                     // "tran", "ac", ...
    QStringList stepLabels;
    double elapsed = 0;
    QStringList measLines;
    // .fra: one complex dataset per analyzer ("Frequency Response Analysis"), and the trace
    // LTspice would plot automatically for it (empty: a blank Bode plot).
    QList<DatasetPtr> fraData;
    QStringList fraAutoPlot;
};

// Runs simulations in worker processes and assembles the results.
class SimController : public QObject
{
    Q_OBJECT
public:
    explicit SimController(QObject *parent = nullptr);
    ~SimController() override;

    // ltNetlist: LTspice-dialect netlist (first line is a title/comment).
    // outputBase: path without extension for .raw/.log/.net files (may be empty).
    void start(const QStringList &ltNetlist, const QString &baseDir, const QString &outputBase);
    bool isRunning() const { return m_running; }
    bool isPaused() const { return m_paused; }
    void pause();
    void resume();
    void stop();

    // Maps ngspice vector names back to LTspice style; returns "" for hidden vectors.
    static QString ltName(const QString &ngName, const TranslationResult &tr);

signals:
    void started(int runs);
    void progress(double percent, const QString &status);
    void logText(const QString &text);
    void finished(const SimResult &result);

private:
    struct Job {
        int index = 0;
        QString netlistPath;
        QString rawPath;
        QPointer<QProcess> proc;
        double percent = 0;
        bool done = false;
        bool retried = false;
        int exitCode = -1;
        QString log;
        QString lineBuffer;
        // .net excitation runs (not part of the stepped results)
        int netStep = -1;
        int netSpec = -1;
        int netPort = 0;
    };

    void launchNext();
    void onOutput(Job &job);
    void onFinished(Job &job, int code);
    void assemble();
    QString workerProgram() const;

    bool m_running = false;
    bool m_paused = false;
    bool m_aborted = false;
    std::unique_ptr<QTemporaryDir> m_tmp;
    QList<Job> m_jobs;
    QList<TranslationResult> m_translations;
    QStringList m_stepLabels;
    QString m_outputBase;
    QString m_baseDir;
    QStringList m_ltNetlist;
    QString m_preLog; // warnings collected before launching
    QElapsedTimer m_timer;
    int m_maxParallel = 1;
    quint32 m_seedBase = 1; // .options seed=N (LTspice); runs are reproducible
    int m_mainRuns = 0;     // jobs [0, m_mainRuns) are the (stepped) analysis; the rest are .net excitations
    QList<QList<netparams::Spec>> m_netSpecs; // per step
};

// Writes a mono 16-bit PCM WAV file from a waveform (.wave directive support).
bool writeWaveFile(const QString &path, const QVector<double> &t, const QList<QVector<double>> &channels, int sampleRate,
                   int bits, QString *error);
