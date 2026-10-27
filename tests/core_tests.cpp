// Core library tests. Run without arguments for unit tests; with
//   core_tests --corpus <examples dir>
// it parses and netlists every .asc file found (requires the library root to be
// configured via LINSPICE_LIBRARY_ROOT) and translates each netlist for ngspice.

#include "core/AppSettings.h"
#include "core/Expression.h"
#include "core/Fra.h"
#include "core/Measure.h"
#include "core/NetlistTranslator.h"
#include "core/Netlister.h"
#include "core/PlotSettings.h"
#include "core/RawFile.h"
#include "core/Schematic.h"
#include "core/StepSweep.h"
#include "core/SymbolLibrary.h"
#include "core/Units.h"
#include "core/TextIO.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cmath>
#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b));
}

static void testUnits()
{
    CHECK(near(*units::parseSpiceNumber("1k"), 1e3));
    CHECK(near(*units::parseSpiceNumber("4.7u"), 4.7e-6));
    CHECK(near(*units::parseSpiceNumber("2Meg"), 2e6));
    CHECK(near(*units::parseSpiceNumber("1M"), 1e-3));
    CHECK(near(*units::parseSpiceNumber("10uF"), 10e-6));
    CHECK(near(*units::parseSpiceNumber("1e-3"), 1e-3));
    CHECK(near(*units::parseSpiceNumber(QString::fromUtf8("2.2µ")), 2.2e-6));
    CHECK(near(*units::parseSpiceNumber("5mil"), 5 * 25.4e-6));
    CHECK(!units::parseSpiceNumber("abc").has_value());
    CHECK(units::formatEng(1500, "V") == QStringLiteral("1.5KV"));
    CHECK(units::formatSpice(1e-6) == QStringLiteral("1u"));
    CHECK(units::formatSpice(2.2e6) == QStringLiteral("2.2Meg"));
}

static void testPlotSettingsPanes()
{
    // LTspice separates panes with "}," (Transformer.plt from the Educational examples).
    const QString path = QDir::temp().filePath(QStringLiteral("linspice_test.plt"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write("[Transient Analysis]\n{\n   Npanes: 2\n   {\n      traces: 1 {524291,0,\"V(out)\"}\n"
            "      Y[0]: (' ',1,-3,0.5,2)\n   },\n   {\n      traces: 1 {268959746,0,\"V(in)\"}\n"
            "      Y[0]: (' ',1,0,0.1,1)\n   }\n}\n");
    f.close();
    PlotSettingsFile ps;
    CHECK(ps.load(path));
    CHECK(ps.sections.size() == 1);
    if (ps.sections.size() == 1) {
        CHECK(ps.sections[0].panes.size() == 2);
        if (ps.sections[0].panes.size() == 2) {
            CHECK(ps.sections[0].panes[0].traces.size() == 1 && ps.sections[0].panes[0].traces[0].expr == QStringLiteral("V(out)"));
            CHECK(ps.sections[0].panes[1].traces.size() == 1 && ps.sections[0].panes[1].traces[0].expr == QStringLiteral("V(in)"));
        }
    }
    QFile::remove(path);
}

static void testFra()
{
    // Schedule rules observed in LTspice's FRA log for fra_eg1 (oct=4, fcoarse=10k):
    // grid fstart*2^(k/oct), one point per octave below fcoarse, fend included, swept downwards.
    const QStringList t = NetlistTranslator::tokenize(
        QStringLiteral("@1 A B delay=100u fstart=100 fend=10Meg oct=4 fcoarse=10k pp0=100m pp1=10m f0=50k f1=500k tavgmin=100u tsettle=2u"));
    QString err;
    const auto a = fra::parseAnalyzer(t, [](const QString &v) { return units::parseSpiceNumber(v).value_or(0); }, &err);
    CHECK(a.has_value());
    if (!a)
        return;
    CHECK(near(a->segments.first().freq, 10e6));
    CHECK(near(a->segments[1].freq, 100 * std::pow(2.0, 66 / 4.0)));
    CHECK(near(a->segments.first().start, 100e-6));
    CHECK(near(a->segments.first().analyzeEnd, 202e-6));    // tsettle + 1000 periods >= tavgmin
    CHECK(near(a->segments.last().freq, 100));
    CHECK(near(a->segments[a->segments.size() - 2].freq, 200)); // one point per octave below fcoarse
    CHECK(near(a->segments.first().amplitude, 10e-3));       // above f1
    CHECK(near(a->segments.last().amplitude, 100e-3));       // below f0
    for (int i = 1; i < a->segments.size(); ++i) {
        CHECK(a->segments[i].start == a->segments[i - 1].end);  // contiguous
        CHECK(std::fabs(std::sin(a->segments[i].phase)) < 1e-9); // frequency changes at zero crossings
    }
}

static void testGeometry()
{
    // Pin world-coordinate table from the format documentation.
    CHECK(orientApply(Orient::R90, QPoint(16, 96)) == QPoint(-96, 16));
    CHECK(orientApply(Orient::M90, QPoint(64, 0)) == QPoint(0, 64));
    CHECK(orientApply(Orient::M270, QPoint(1, 2)) == QPoint(-2, -1));
    CHECK(orientRotateCW(Orient::R270) == Orient::R0);
    CHECK(orientMirror(orientMirror(Orient::R90)) == Orient::R90);
}

static void testExpression()
{
    ParamContext ctx;
    ctx.vars.insert("x", Value::scalar(3));
    bool ok;
    CHECK(near(Expression("1k*2+x").evaluateScalar(ctx, &ok), 2003) && ok);
    CHECK(near(Expression("if(x>2, 10, 20)").evaluateScalar(ctx, &ok), 10));
    CHECK(near(Expression("2**3").evaluateScalar(ctx, &ok), 8));
    CHECK(near(Expression("-2**2").evaluateScalar(ctx, &ok), 4)); // unary binds tighter, like LTspice
    CHECK(near(Expression("limit(5,0,1)").evaluateScalar(ctx, &ok), 1));
    CHECK(near(Expression("table(1.5, 1,10, 2,20)").evaluateScalar(ctx, &ok), 15));
    CHECK(near(Expression("abs(-4)+sqrt(16)").evaluateScalar(ctx, &ok), 8));
    Expression::defineFromDirective(".func sq(a) {a*a}");
    CHECK(near(Expression("sq(x)+1").evaluateScalar(ctx, &ok), 10));
    Expression bad("1+");
    CHECK(!bad.isValid());
}

static void testSchematicRoundTrip()
{
    const char *asc =
        "Version 4.1\nSHEET 1 880 400\n"
        "WIRE -48 64 -48 112\nWIRE 80 64 -48 64\nWIRE 208 64 160 64\nWIRE 256 64 208 64\nWIRE 256 112 256 64\n"
        "WIRE -48 240 -48 192\nWIRE 256 240 256 176\n"
        "FLAG -48 64 in\nFLAG 208 64 out\nFLAG -48 240 0\nFLAG 256 240 0\n"
        "SYMBOL voltage -48 96 R0\nSYMATTR InstName V1\nSYMATTR Value AC 1\n"
        "SYMBOL res 176 48 R90\nSYMATTR InstName R1\nSYMATTR Value 1k\n"
        "SYMBOL cap 240 112 R0\nSYMATTR InstName C1\nSYMATTR Value 100n\n"
        "TEXT -48 296 Left 2 !.ac dec 100 1 10Meg\n";
    Schematic s;
    CHECK(s.parse(QString::fromLatin1(asc)));
    s.resolveSymbols();
    CHECK(s.unresolvedSymbols().isEmpty());
    Netlister nl(s);
    const NetlistResult r = nl.build();
    CHECK(r.errors.isEmpty());
    CHECK(r.text.contains("V1 in 0 AC 1"));
    CHECK(r.text.contains("R1 out in 1k"));
    CHECK(r.text.contains("C1 out 0 100n"));
    CHECK(r.text.contains(".ac dec 100 1 10Meg"));
    // Re-serialize and re-parse.
    Schematic s2;
    s2.parse(s.serialize());
    CHECK(s2.wires.size() == s.wires.size() && s2.symbols.size() == 3 && s2.texts.size() == 1);
}

static void testTranslator()
{
    const QStringList lt = {
        "* test",
        "V1 in 0 PULSE(0 1 0 1u 1u 0.5m 1m) AC 1 Rser=10",
        "R1 in out 1k tol=1 pwr=0.25",
        "C1 out 0 10µ Rser=5m Lser=1n",
        "L1 out x 1m",
        "B1 y 0 V=if(V(out)>0.5, 1, 0)",
        "D1 x 0 D N=2",
        ".tran 2m",
        ".meas tran vmax MAX V(out)",
        ".backanno",
        ".end",
    };
    NetlistTranslator tr;
    NetlistTranslator::Options o;
    const TranslationResult r = tr.translate(lt, o);
    CHECK(r.errors.isEmpty());
    CHECK(r.analysis == "tran");
    CHECK(r.netlist.contains("Rl1_rser") || r.netlist.contains("RL1_rser"));
    CHECK(r.netlist.contains(".tran 1.953125e-06 0.002 0 1.953125e-06")); // LTspice's default max step: Tstop/1024
    CHECK(r.measStatements.size() == 1);
    CHECK(r.netlist.contains("? (1) : (0)"));
    CHECK(!QString(r.netlist).remove(QStringLiteral(".options trtol=")).contains("tol=")); // component tolerances are stripped
    if (g_failures)
        std::fprintf(stderr, "%s\n", qPrintable(r.netlist));
}

static void testStep()
{
    StepPlan p;
    QString err;
    CHECK(p.parse({".step param R 1k 3k 1k", ".step oct param C 1n 4n 1"}, &err));
    CHECK(p.runCount() == 9);
    const QStringList out = p.apply({"R1 a b {R}", ".param R=5", ".step param R 1k 3k 1k", ".end"}, 1);
    CHECK(out.join('\n').contains(".param R=2000"));
}

static void testMeasure()
{
    Dataset ds;
    ds.kind = Dataset::Kind::Transient;
    ds.vars = {{"time", "time"}, {"V(out)", "voltage"}};
    Dataset::Run run;
    run.re.resize(2);
    for (int i = 0; i <= 100; ++i) {
        run.re[0] << i * 0.01;
        run.re[1] << std::sin(2 * M_PI * i * 0.01);
    }
    ds.runs << run;
    ds.rebuildIndex();
    MeasureEngine me;
    const auto res = me.evaluate(ds, 0, {".meas tran vmax MAX V(out)", ".meas tran t1 WHEN V(out)=0 FALL=1",
                                         ".meas tran avg1 AVG V(out) FROM 0 TO 1", ".meas tran p2 PARAM vmax*2"});
    CHECK(res.size() == 4);
    CHECK(res[0].ok && near(res[0].value.real(), 1.0, 1e-3));
    CHECK(res[1].ok && near(res[1].value.real(), 0.5, 1e-3));
    CHECK(res[2].ok && std::fabs(res[2].value.real()) < 1e-3);
    CHECK(res[3].ok && near(res[3].value.real(), 2.0, 1e-3));
}

static int corpus(const QString &dir, bool translate)
{
    int files = 0, parseFail = 0, unresolved = 0, netErrors = 0, trErrors = 0;
    QMap<QString, int> errorKinds;
    QElapsedTimer timer;
    timer.start();
    QDirIterator it(dir, {"*.asc"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        ++files;
        Schematic s;
        QString err;
        if (!s.load(path, &err)) {
            ++parseFail;
            continue;
        }
        if (!s.unresolvedSymbols().isEmpty()) {
            ++unresolved;
            for (const QString &u : s.unresolvedSymbols())
                errorKinds["unresolved symbol: " + u]++;
        }
        // Round-trip check: serialize -> parse -> serialize is stable.
        Schematic s2;
        s2.parse(s.serialize());
        if (s2.serialize() != s.serialize())
            errorKinds["round-trip mismatch"]++;
        Netlister nl(s);
        const NetlistResult r = nl.build();
        if (!r.errors.isEmpty())
            ++netErrors;
        if (translate) {
            NetlistTranslator tr;
            NetlistTranslator::Options o;
            o.baseDir = QFileInfo(path).absolutePath();
            StepPlan plan;
            const QStringList lines = r.text.split('\n');
            plan.parse(lines);
            const TranslationResult t = tr.translate(plan.isEmpty() ? lines : plan.apply(lines, 0), o);
            if (t.errors.isEmpty() && qEnvironmentVariableIsSet("DUMP")) {
                QFile f(qEnvironmentVariable("DUMP") + '/' + QFileInfo(path).completeBaseName() + ".cir");
                if (f.open(QIODevice::WriteOnly))
                    f.write(t.netlist.toUtf8());
            }
            if (!t.errors.isEmpty()) {
                ++trErrors;
                for (const QString &e : t.errors) {
                    if (qEnvironmentVariableIsSet("SHOW") && e.contains(qEnvironmentVariable("SHOW")))
                        std::printf("%s: %s\n", qPrintable(path), qPrintable(e));
                    QString k = e;
                    k.replace(QRegularExpression("\"[^\"]*\""), "\"…\"");
                    errorKinds[k]++;
                }
            }
        }
    }
    std::printf("files=%d parseFail=%d unresolvedSym=%d netlistErrors=%d translateErrors=%d (%.1fs)\n", files, parseFail, unresolved,
                netErrors, trErrors, timer.elapsed() / 1000.0);
    QList<QPair<int, QString>> sorted;
    for (auto i = errorKinds.constBegin(); i != errorKinds.constEnd(); ++i)
        sorted << qMakePair(i.value(), i.key());
    std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return a.first > b.first; });
    for (int i = 0; i < sorted.size() && i < 40; ++i)
        std::printf("%6d  %s\n", sorted[i].first, qPrintable(sorted[i].second.left(150)));
    return 0;
}

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(resources);
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("LINSPICE_LIBRARY_ROOT"))
        AppSettings::instance().setLibraryRoot(qEnvironmentVariable("LINSPICE_LIBRARY_ROOT"));
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == "--netlist") {
        Schematic s;
        NetlistResult r;
        if (args[2].endsWith(".asc", Qt::CaseInsensitive)) {
            s.load(args[2]);
            r = Netlister(s).build();
        } else {
            r.text = textio::readText(args[2]);
        }
        std::printf("=== LTspice netlist\n%s\n", qPrintable(r.text));
        for (const QString &e : r.errors)
            std::printf("netlist error: %s\n", qPrintable(e));
        NetlistTranslator tr;
        NetlistTranslator::Options o;
        o.baseDir = QFileInfo(args[2]).absolutePath();
        StepPlan plan;
        const QStringList lines = r.text.split('\n');
        plan.parse(lines);
        const TranslationResult t = tr.translate(plan.isEmpty() ? lines : plan.apply(lines, 0), o);
        std::printf("=== ngspice netlist\n%s\n", qPrintable(t.netlist));
        for (const QString &e : t.errors)
            std::printf("error: %s\n", qPrintable(e));
        for (const QString &e : t.warnings)
            std::printf("warning: %s\n", qPrintable(e));
        return 0;
    }
    if (args.size() >= 3 && args[1] == "--netcompare") {
        // Compare node names per element with LTspice-generated .net files in a directory.
        int total = 0, match = 0, files = 0, filesOk = 0;
        QDirIterator it(args[2], {"*.net"}, QDir::Files);
        while (it.hasNext()) {
            const QString net = it.next();
            const QString asc = net.left(net.size() - 4) + ".asc";
            if (!QFileInfo::exists(asc))
                continue;
            Schematic s;
            if (!s.load(asc))
                continue;
            const NetlistResult r = Netlister(s).build();
            QHash<QString, QStringList> ours;
            for (const QString &l : r.text.split('\n')) {
                const QStringList t = l.split(' ', Qt::SkipEmptyParts);
                if (t.size() > 2 && !l.startsWith('.') && !l.startsWith('*'))
                    ours.insert(t[0].toUpper(), t.mid(1));
            }
            bool fileOk = true;
            ++files;
            for (const QString &l : textio::splitLines(textio::readText(net))) {
                const QStringList t = l.split(' ', Qt::SkipEmptyParts);
                if (t.size() < 3 || l.startsWith('.') || l.startsWith('*') || !ours.contains(t[0].toUpper()))
                    continue;
                const QStringList o = ours.value(t[0].toUpper());
                for (int i = 1; i < t.size() && i - 1 < o.size(); ++i) {
                    if (!t[i].startsWith(QLatin1String("N0"), Qt::CaseInsensitive) && !o[i - 1].startsWith(QLatin1String("N0"), Qt::CaseInsensitive))
                        continue;
                    ++total;
                    if (t[i].compare(o[i - 1], Qt::CaseInsensitive) == 0)
                        ++match;
                    else
                        fileOk = false;
                }
            }
            if (fileOk)
                ++filesOk;
            else if (qEnvironmentVariableIsSet("SHOW"))
                std::printf("mismatch: %s\n", qPrintable(asc));
        }
        std::printf("node refs matched %d/%d, files fully matching %d/%d\n", match, total, filesOk, files);
        return 0;
    }
    if (args.size() >= 3 && args[1] == "--corpus")
        return corpus(args[2], args.contains("--translate"));

    testUnits();
    testGeometry();
    testFra();
    testPlotSettingsPanes();
    testExpression();
    testSchematicRoundTrip();
    testTranslator();
    testStep();
    testMeasure();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
