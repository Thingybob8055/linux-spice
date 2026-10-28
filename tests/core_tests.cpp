// Core library tests. Run without arguments for unit tests; with
//   core_tests --corpus <examples dir>
// it parses and netlists every .asc file found (requires the library root to be
// configured via LINSPICE_LIBRARY_ROOT) and translates each netlist for ngspice.

#include "core/AppSettings.h"
#include "core/Expression.h"
#include "core/Fourier.h"
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
#include <QTemporaryDir>

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
    // The unit letter in place of the decimal point (checked in LTspice).
    CHECK(near(units::parseSpiceNumber("4k7").value_or(0), 4700));
    CHECK(near(units::parseSpiceNumber("7m999").value_or(0), 7.999e-3));
    CHECK(near(units::parseSpiceNumber("1u5").value_or(0), 1.5e-6));
    CHECK(near(units::parseSpiceNumber("2meg2").value_or(0), 2.2e6));
    CHECK(near(units::parseSpiceNumber("10k").value_or(0), 1e4));
    CHECK(near(units::parseSpiceNumber("1.5k").value_or(0), 1500));
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

// An LTspice raw file saved from a start time stores times relative to it, with the start in
// "Offset:"; the reader restores absolute times.
static void testRawOffset()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("o.raw"));
    QByteArray hdr = QStringLiteral("Title: t\nDate: x\nPlotname: Transient Analysis\nFlags: real forward\nNo. Variables: 2\n"
                                    "No. Points: 2\nOffset:    2.9500000000000002e+00\nVariables:\n\t0\ttime\ttime\n\t1\tV(a)\tvoltage\nBinary:\n").toUtf8();
    QByteArray file;
    for (char c : hdr) {
        file.append(c);
        file.append('\0');
    }
    for (double t : {0.0, 0.05}) {
        const float v = 1.0f;
        file.append(reinterpret_cast<const char *>(&t), 8);
        file.append(reinterpret_cast<const char *>(&v), 4);
    }
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly) && f.write(file) == file.size());
    f.close();
    QString err;
    const auto plots = rawfile::read(path, &err);
    CHECK(!plots.isEmpty());
    if (!plots.isEmpty()) {
        CHECK(near(plots[0]->runs[0].re[0][0], 2.95));
        CHECK(near(plots[0]->runs[0].re[0][1], 3.0));
    }
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
    // NoiseFigure.plt's trace: Boltzmann's constant k and "1K" as a number.
    CHECK(near(Expression("10*log10(x*x/(4*k*300.15*1K))").evaluateScalar(ctx, &ok), 10 * std::log10(9 / (4 * 1.3806503e-23 * 300.15 * 1000))) && ok);
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
    // Pins touching without a wire are P001, P002, ... in the order of the first symbol in the
    // file that touches them, not by position (checked against LTspice's netlists).
    {
        const char *pins =
            "Version 4\nSHEET 1 880 400\n"
            "FLAG 16 16 a\nFLAG 16 176 0\nFLAG 216 -284 a\nFLAG 216 -124 0\n"
            "SYMBOL res 0 0 R0\nSYMATTR InstName R1\nSYMATTR Value 1k\n"
            "SYMBOL res 0 80 R0\nSYMATTR InstName R2\nSYMATTR Value 1k\n"
            "SYMBOL res 200 -300 R0\nSYMATTR InstName R3\nSYMATTR Value 1k\n"
            "SYMBOL res 200 -220 R0\nSYMATTR InstName R4\nSYMATTR Value 1k\n";
        Schematic sp;
        CHECK(sp.parse(QString::fromLatin1(pins)));
        sp.resolveSymbols();
        const NetlistResult rp = Netlister(sp).build();
        CHECK(rp.text.contains("R1 a P001") && rp.text.contains("R3 a P002"));
    }
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

// Vectors LTspice saves that ngspice doesn't by default: subcircuit pin currents, AC
// device currents, inductor flux, per-device noise.
static void testProbeableSignals()
{
    NetlistTranslator::Options o;
    o.saveCurrents = true;
    {
        const QStringList lt = {"* pins", "V1 in 0 SIN(0 1 1k)", "XU1 in out amp", "L1 out 0 1m Cpar=1p", "A1 in 0 0 0 0 0 q 0 BUF",
                                ".subckt amp a mt1", "R1 a mt1 1k", ".ends amp", ".tran 1m", ".end"};
        NetlistTranslator tr;
        const TranslationResult r = tr.translate(lt, o);
        CHECK(r.errors.isEmpty());
        CHECK(r.netlist.contains("Vlspin_xu1_1 in "));
        CHECK(r.pinCurrents.value("vlspin_xu1_2") == "Ix(u1:mt1)");
        CHECK(r.pinCurrents.value("vlspin_a1_7") == "I7(A1)");
        CHECK(r.netlist.contains(".save all @l1[flux]"));
        CHECK(r.parallelParts.value("l1").contains("cl1_cpar"));
        if (g_failures)
            std::fprintf(stderr, "%s\n", qPrintable(r.netlist));
    }
    {
        const QStringList lt = {"* ac", "V1 in 0 AC 1", "R1 in out 1k", "C1 out 0 1u", ".ac dec 10 1 1k", ".end"};
        NetlistTranslator tr;
        const TranslationResult r = tr.translate(lt, o);
        CHECK(r.netlist.contains(".probe i(R1) i(C1)"));
    }
    {
        const QStringList lt = {"* noise", "V1 in 0 AC 1", "R1 in out 1k", "R2 out 0 1k", ".noise V(out) V1 dec 10 1 1k", ".end"};
        NetlistTranslator tr;
        const TranslationResult r = tr.translate(lt, o);
        CHECK(r.netlist.contains(".options noiseall"));
        CHECK(r.netlist.contains(".noise V(out) V1 dec 10 1 1k 1"));
        CHECK(!r.netlist.contains("savecurrents"));
    }
    {
        const QStringList lt = {"* igbt", "V1 c 0 10", "V2 g 0 15", "Z1 c g 0 NIGBT", ".model NIGBT NIGBT", ".dc V1 0 10 1", ".end"};
        NetlistTranslator tr;
        const TranslationResult r = tr.translate(lt, o);
        CHECK(r.pinCurrents.value("vlspin_z1_1") == "Ic(Z1)");
        CHECK(r.pinCurrents.value("vlspin_z1_3") == "Ie(Z1)");
    }
}

// Op-amp macromodel building blocks (LTspice's AD820 and similar).
static void testMacromodelParts()
{
    NetlistTranslator::Options o;
    const QStringList lt = {"* parts", "V1 3 0 9", "V2 in 0 1",
                            // An OTA input wired to node 0 is ground, not the OTA's common node.
                            "A1 in 0 m m m m x m OTA g=1m", "R1 x 0 1k", "R2 m 0 1k",
                            // Ideal diode whose Roff is an expression: never folded to a number.
                            "D5 n 3 X1", ".param Ro=1Meg", ".model X1 D(Ron=1m Roff={2*Ro} Vfwd=-3m epsilon=10m)",
                            "R3 n 0 1k", ".op", ".end"};
    NetlistTranslator tr;
    const TranslationResult r = tr.translate(lt, o);
    CHECK(r.errors.isEmpty());
    CHECK(r.netlist.contains("V(0)-V(in)") || r.netlist.contains("V(in)-V(0)"));
    CHECK(r.netlist.contains("2*Ro"));
    CHECK(!r.netlist.contains("V(n,3)*0.5"));
    // A G source with vto/dir is a square-law transconductor.
    {
        NetlistTranslator t2;
        const TranslationResult g = t2.translate({"* g", "V1 c 0 1", "G1 o 0 c 0 100m vto=670m dir=1", "R1 o 0 1", ".op", ".end"}, o);
        CHECK(g.netlist.contains("uramp((1)*(V(c,0)-(670m)))*uramp((1)*(V(c,0)-(670m)))"));
    }
    // A long .wav input streams through XSPICE's filesource with the step held to one sample.
    {
        QTemporaryDir dir;
        QFile wav(dir.filePath(QStringLiteral("long.wav")));
        CHECK(wav.open(QIODevice::WriteOnly));
        const quint32 n = 30000, rate = 44100;
        auto u32 = [&](quint32 v) { wav.write(reinterpret_cast<const char *>(&v), 4); };
        auto u16 = [&](quint16 v) { wav.write(reinterpret_cast<const char *>(&v), 2); };
        wav.write("RIFF"); u32(36 + 2 * n); wav.write("WAVEfmt "); u32(16); u16(1); u16(1); u32(rate); u32(2 * rate); u16(2); u16(16);
        wav.write("data"); u32(2 * n);
        for (quint32 k = 0; k < n; ++k)
            u16(quint16(qint16(1000 * std::sin(k * 0.05))));
        wav.close();
        NetlistTranslator t6;
        NetlistTranslator::Options wo = o;
        wo.baseDir = dir.path();
        const TranslationResult w = t6.translate({"* wav", "V1 in 0 wavefile=long.wav", "R1 in 0 1k", ".tran 0.5", ".end"}, wo);
        CHECK(w.errors.isEmpty());
        CHECK(w.netlist.contains("filesource"));
        CHECK(w.netlist.contains("V1 in ls_v1_71 DC 0"));
        CHECK(w.netlist.contains(QStringLiteral(" %1\n").arg(1.0 / rate, 0, 'g', 12))); // .tran maximum step: one sample
    }
    // PWL REPEAT FOREVER (...) ENDREPEAT, with LTspice's "7m999" notation.
    {
        NetlistTranslator t7;
        const TranslationResult p = t7.translate({"* pwl", "V1 a 0 PWL REPEAT FOREVER (0 1 4m 1 7m999 1 8m 0) ENDREPEAT", "R1 a 0 1k", ".tran 30m", ".end"}, o);
        CHECK(p.netlist.contains("PWL(0 1 0.004 1 0.007999 1 0.008 0) r=0"));
    }
    // pwr/pwrs keep LTspice's meaning for negative bases (checked in LTspice).
    {
        NetlistTranslator t8;
        const TranslationResult pw = t8.translate({"* pwr", "V1 n 0 -8", "B1 a 0 V=pwr(V(n),1.1)+pwrs(V(n),2)", "R1 a 0 1", ".op", ".end"}, o);
        CHECK(pw.netlist.contains("pow(abs(V(n)),(1.1))"));
        CHECK(pw.netlist.contains("(sgn(V(n))*pow(abs(V(n)),(2)))"));
    }
    // A decimal comma in a value is an error, not a silently different part.
    {
        NetlistTranslator t9;
        const TranslationResult dc = t9.translate({"* comma", "V1 a 0 1", "R1 a 0 4,7k", ".op", ".end"}, o);
        CHECK(!dc.errors.isEmpty() && dc.errors[0].contains("4.7k"));
    }
    // A periodic PULSE gets a silent companion whose period starts at the fall, so ngspice
    // cannot step over a pulse when floor(t/PER) rounds down at a period start.
    {
        NetlistTranslator t10;
        const TranslationResult pu = t10.translate({"* pulse", "V1 c 0 PULSE(0 1 0 1n 1n 2.7u 50u)", "R1 c 0 1k", ".tran 1m", ".end"}, o);
        CHECK(pu.netlist.contains("Vls_v1_bp ls_v1_72 0 PULSE(0 0 2.701e-06 1e-09 4.7298e-05 1e-09 5e-05)"));
    }
    // A capacitor with Lser gets Cpar >= C*1e-6, as LTspice logs "Increased Cpar"; PSpice-style
    // "R name a b MODEL value" keeps its value.
    {
        NetlistTranslator t12;
        const TranslationResult cp = t12.translate({"* cpar", "V1 a 0 1", "C1 a b 56u Rser=11.5m Lser=13.4n", "C2 b 0 1u Lser=0",
                                                    "R1 b 0 RNOISE 40E3 $ comment", ".model RNOISE RES(T_ABS=-273.15)", ".op", ".end"}, o);
        CHECK(cp.netlist.contains("CC1_cpar a b 56p"));
        CHECK(!cp.netlist.contains("CC2_cpar"));
        CHECK(cp.netlist.contains("R1 b 0 40E3"));
    }
    // A switch without hysteresis stays off while its control sits exactly at Vt, as in LTspice.
    {
        NetlistTranslator t13;
        const TranslationResult sw = t13.translate({"* sw", "V1 c 0 0", "V2 a 0 5", "S1 a b c 0 SW", "R1 b 0 10k", ".model sw sw()",
                                                    "S2 a d c 0 SH", "R2 d 0 1k", ".model sh sw(vt=1 vh=0.5)", ".op", ".end"}, o);
        CHECK(sw.netlist.contains(QRegularExpression("\\.model sw sw\\(\\s*vt=2n vh=1e-9\\)", QRegularExpression::CaseInsensitiveOption)));
        CHECK(sw.netlist.contains(QRegularExpression("\\.model sh sw\\(vt=1 vh=0.5\\)", QRegularExpression::CaseInsensitiveOption)));
    }
    // Floating nodes: C*2^-43 S across each capacitor plus 2^-43 S to ground from the group's
    // last node, so x settles at sum(C*V)/(1F+sum(C)); a dangling node just follows its far side.
    {
        NetlistTranslator t11;
        const TranslationResult fl = t11.translate({"* fl", "V1 a 0 5", "R1 a 0 1k", "C3 x a 1u", "R9 x y 1k", "C5 y 0 2u",
                                                    "C6 z a 1u", ".op", ".end"}, o);
        CHECK(fl.netlist.contains(" x a 8.796093022e+18") && fl.netlist.contains(" y 0 4.398046511e+18") && fl.netlist.contains(" y 0 8796093022208"));
        CHECK(fl.netlist.contains(" z a 1T") && !fl.netlist.contains(" z 0 "));
    }
    // A comparison in a behavioral source gets a hidden switch that locates its crossing.
    {
        NetlistTranslator t14;
        const TranslationResult lc = t14.translate({"* loc", "V1 a 0 SIN(0 1 1k)", "B1 b 0 V=if(V(a)>=0.5,10,0)", "R1 b 0 1k",
                                                    ".tran 0 10m 9m", ".end"}, o);
        CHECK(lc.netlist.contains(QRegularExpression("Bls_B1_loc0 \\S+ 0 V=1e6\\*\\(\\(V\\(a\\)\\)-\\(0.5\\)\\)")));
        CHECK(lc.netlist.contains(".model ls_loc_sw sw("));
        // Inside the saved window LTspice's step is at most (Tstop-Tstart)/250.
        CHECK(lc.netlist.contains(".tran 9.765625e-07 0.01 0.009 4e-06"));
    }
    // ".include../lib/x.sub" (no space) is read like LTspice does.
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("lib");
        QFile f(dir.path() + "/lib/x.sub");
        f.open(QIODevice::WriteOnly);
        f.write(".subckt xx a b\nR1 a b 1k\n.ends xx\n");
        f.close();
        NetlistTranslator t15;
        NetlistTranslator::Options oi = o;
        oi.baseDir = dir.path();
        const TranslationResult inc = t15.translate({"* inc", "V1 a 0 1", "X1 a 0 xx", ".include./lib/x.sub", ".op", ".end"}, oi);
        CHECK(inc.errors.isEmpty() && inc.netlist.contains(".subckt xx", Qt::CaseInsensitive));
    }
    // A zener's low-level breakdown (Ibvl/Nbvl) becomes a parallel diode.
    {
        NetlistTranslator t3;
        const TranslationResult z = t3.translate({"* z", "I1 0 a 1u", "D1 0 a ZZ", ".model ZZ D(BV=4.7 IBV=20m Ibvl=1.96m Nbvl=15)", ".op", ".end"}, o);
        CHECK(z.netlist.contains("D1_bvl 0 a D1_bvl"));
        CHECK(z.netlist.contains("IBV=1.96m NBV=15"));
        CHECK(!z.netlist.contains("Ibvl"));
    }
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
    // LTspice ends every sweep on its stop value and sweeps upward (checked in LTspice).
    StepPlan q;
    CHECK(q.parse({".step param R 1 10k 1k"}, &err) && q.runCount() == 11 && q.axes()[0].values.last() == 10000);
    StepPlan o2;
    CHECK(o2.parse({".step oct param R 1 5 1"}, &err) && o2.axes()[0].values == QVector<double>({1, 2, 4, 5}));
    StepPlan d;
    CHECK(d.parse({".step param R 10 1 -4"}, &err) && d.axes()[0].values == QVector<double>({1, 5, 9, 10}));
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

// .four: LTspice's harmonic table from the transient data.
static void testFourier()
{
    Dataset ds;
    ds.kind = Dataset::Kind::Transient;
    ds.vars = {{"time", "time"}, {"V(b)", "voltage"}};
    Dataset::Run run;
    run.re.resize(2);
    for (int i = 0; i <= 5000; ++i) {
        const double t = i * 1e-6, x = std::sin(2 * M_PI * 1e3 * t);
        run.re[0] << t;
        run.re[1] << x + 0.1 * x * x * x;
    }
    ds.runs << run;
    ds.rebuildIndex();
    const QString log = fourier::evaluate(ds, 0, {".four {f} 3 V(b)"}, {{"f", 1e3}});
    // x + 0.1x^3 = 1.075 sin(wt) - 0.025 sin(3wt): LTspice prints 1.075e+00, 90.00 and 2.504e-02, -89.99.
    CHECK(log.contains("    1   \t 1.000e+03\t 1.075e+00\t 1.000e+00\t   90.00"));
    CHECK(log.contains("    3   \t 3.000e+03\t 2.50"));
    CHECK(log.contains("Partial Harmonic Distortion: 2.32") || log.contains("Partial Harmonic Distortion: 2.33"));
    if (g_failures)
        std::fprintf(stderr, "%s\n", qPrintable(log));
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
    testRawOffset();
    testExpression();
    testSchematicRoundTrip();
    testTranslator();
    testProbeableSignals();
    testMacromodelParts();
    testStep();
    testMeasure();
    testFourier();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
