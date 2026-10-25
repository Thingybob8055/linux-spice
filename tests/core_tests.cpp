// Core library tests. Run without arguments for unit tests; with
//   core_tests --corpus <examples dir>
// it parses and netlists every .asc file found (requires the library root to be
// configured via LINSPICE_LIBRARY_ROOT) and translates each netlist for ngspice.

#include "core/AppSettings.h"
#include "core/Expression.h"
#include "core/Measure.h"
#include "core/NetlistTranslator.h"
#include "core/Netlister.h"
#include "core/RawFile.h"
#include "core/Schematic.h"
#include "core/StepSweep.h"
#include "core/SymbolLibrary.h"
#include "core/Units.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
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
    CHECK(r.netlist.contains(".tran 2e-06 0.002"));
    CHECK(r.measStatements.size() == 1);
    CHECK(r.netlist.contains("? (1) : (0)"));
    CHECK(!r.netlist.contains("tol="));
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
        s.load(args[2]);
        const NetlistResult r = Netlister(s).build();
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
    if (args.size() >= 3 && args[1] == "--corpus")
        return corpus(args[2], args.contains("--translate"));

    testUnits();
    testGeometry();
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
