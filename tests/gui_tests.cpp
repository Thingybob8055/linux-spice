// GUI interaction tests: drive the schematic editor and waveform viewer with
// synthetic mouse/keyboard events (run with QT_QPA_PLATFORM=offscreen).

#include "core/Netlister.h"
#include "core/RawFile.h"
#include "gui/SchematicView.h"
#include "gui/WaveformView.h"

#include <QtTest>

#include <cmath>

class GuiTests : public QObject
{
    Q_OBJECT

private:
    // Widget position of a world (schematic) point for a view at its default zoom.
    static QPoint at(SchematicView &v, QPointF world)
    {
        // The view maps world -> widget with scale/origin; recover it from two probes.
        Q_UNUSED(v);
        return world.toPoint();
    }

    static void prepare(SchematicView &v)
    {
        v.resize(800, 600);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        // Make the view an identity transform: scale 1 at origin (0,0), via zoom-to-fit of an empty sheet
        // is not deterministic, so we place items with known world coordinates through the API where needed.
    }

private slots:
    void initTestCase()
    {
        Q_INIT_RESOURCE(resources);
    }

    void placeAndWire()
    {
        SchematicView v;
        v.resize(800, 600);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));

        // Nothing to undo or redo on a fresh sheet.
        QVERIFY(!v.canUndo());
        QVERIFY(!v.canRedo());
        QSignalSpy undoSpy(&v, &DocumentWidget::undoStateChanged);

        // Place a resistor by clicking in Place mode.
        v.startPlaceSymbol(QStringLiteral("res"));
        QCOMPARE(v.mode(), SchematicView::Mode::Place);
        QTest::mouseMove(&v, QPoint(200, 200));
        QTest::mouseClick(&v, Qt::LeftButton, {}, QPoint(200, 200));
        QCOMPARE(v.schematic().symbols.size(), 1);
        QCOMPARE(v.schematic().symbols[0].instName(), QStringLiteral("R1"));
        QVERIFY(v.canUndo());
        QVERIFY(!v.canRedo());
        QVERIFY(undoSpy.count() > 0);
        // Placement snaps to the 16-unit grid.
        const QPoint pos = v.schematic().symbols[0].pos;
        QCOMPARE(pos.x() % 16, 0);
        QCOMPARE(pos.y() % 16, 0);

        // A second click places R2 (mode stays active).
        QTest::mouseClick(&v, Qt::LeftButton, {}, QPoint(400, 200));
        QCOMPARE(v.schematic().symbols.size(), 2);
        QCOMPARE(v.schematic().symbols[1].instName(), QStringLiteral("R2"));
        QTest::mouseClick(&v, Qt::RightButton, {}, QPoint(400, 200));
        QCOMPARE(v.mode(), SchematicView::Mode::Normal);

        // Wire from R1's top pin to R2's top pin through two clicks + automatic end on a pin.
        const QList<QPoint> p1 = v.schematic().symbols[0].pinPositions();
        const QList<QPoint> p2 = v.schematic().symbols[1].pinPositions();
        QCOMPARE(p1.size(), 2);
        // Convert world -> widget using the view's own mapping: probe by placing the cursor.
        auto widgetFor = [&v](QPoint world) {
            // The view starts with scale 1.5 and origin (0,0).
            return QPoint(int(world.x() * 1.5), int(world.y() * 1.5));
        };
        v.setMode(SchematicView::Mode::Wire);
        QTest::mouseClick(&v, Qt::LeftButton, {}, widgetFor(p1[0]));
        QTest::mouseMove(&v, widgetFor(p1[0] + QPoint(64, 0)));
        QTest::mouseClick(&v, Qt::LeftButton, {}, widgetFor(p2[0]));
        // Both pins are now connected by wire(s).
        const Connectivity c = Netlister::connectivity(v.schematic());
        QCOMPARE(c.netAt(p1[0]), c.netAt(p2[0]));
        QVERIFY(!c.netAt(p1[0]).isEmpty());
        // Wires are orthogonal.
        for (const Wire &w : v.schematic().wires)
            QVERIFY(w.a.x() == w.b.x() || w.a.y() == w.b.y());
        v.setMode(SchematicView::Mode::Normal);

        // Rotate R1 and undo/redo.
        const Orient before = v.schematic().symbols[0].orient;
        v.editSymbol(-1); // no-op on invalid index
        v.clearSelection();
        // Select R1 by clicking on its body.
        const QRect body = v.schematic().symbols[0].worldBounds();
        QTest::mouseClick(&v, Qt::LeftButton, {}, widgetFor(body.center()));
        QVERIFY(v.hasSelection());
        v.rotateSelection();
        QVERIFY(v.schematic().symbols[0].orient != before);
        v.undo();
        QCOMPARE(v.schematic().symbols[0].orient, before);
        QVERIFY(v.canRedo());
        v.redo();
        QVERIFY(v.schematic().symbols[0].orient != before);
        v.undo();
    }

    void dragStretchesWires()
    {
        SchematicView v;
        v.resize(800, 600);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        Schematic s = SchematicView::emptySchematic();
        s.parse(QStringLiteral("Version 4.1\nSHEET 1 880 680\nWIRE 96 16 32 16\n"
                               "SYMBOL res 80 0 R0\nSYMATTR InstName R1\nSYMATTR Value 1k\n"));
        v.setSchematic(s);
        // R1 pin A is at (96,16), wire runs to (32,16).
        QCOMPARE(v.schematic().symbols[0].pinPositions()[0], QPoint(96, 16));
        const QPoint bodyW = v.schematic().symbols[0].worldBounds().center();
        const QPoint press(int(bodyW.x() * 1.5), int(bodyW.y() * 1.5));
        QTest::mousePress(&v, Qt::LeftButton, {}, press);
        // Move in steps so the drag is recognised.
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(&v, press + QPoint(0, i * 6));
        QTest::mouseRelease(&v, Qt::LeftButton, {}, press + QPoint(0, 48));
        const QPoint newPin = v.schematic().symbols[0].pinPositions()[0];
        QVERIFY(newPin != QPoint(96, 16));
        // The wire still reaches the moved pin.
        const Connectivity c = Netlister::connectivity(v.schematic());
        QCOMPARE(c.netAt(newPin), c.netAt(QPoint(32, 16)));
    }

    void deleteCopyPasteAndNetlist()
    {
        SchematicView v;
        v.resize(800, 600);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        Schematic s = SchematicView::emptySchematic();
        s.parse(QStringLiteral("Version 4.1\nSHEET 1 880 680\n"
                               "WIRE -48 64 -48 112\nWIRE 80 64 -48 64\nWIRE 256 64 160 64\nWIRE 256 112 256 64\n"
                               "WIRE -48 240 -48 192\nWIRE 256 240 256 176\n"
                               "FLAG -48 240 0\nFLAG 256 240 0\nFLAG 256 64 out\n"
                               "SYMBOL voltage -48 96 R0\nSYMATTR InstName V1\nSYMATTR Value AC 1\n"
                               "SYMBOL res 176 48 R90\nSYMATTR InstName R1\nSYMATTR Value 1k\n"
                               "SYMBOL cap 240 112 R0\nSYMATTR InstName C1\nSYMATTR Value 100n\n"
                               "TEXT -48 296 Left 2 !.ac dec 100 1 10Meg\n"));
        v.setSchematic(s);
        NetlistResult r = v.netlist();
        QVERIFY(r.errors.isEmpty());
        QVERIFY(r.text.contains(QStringLiteral("R1 out N001 1k")));
        QVERIFY(r.text.contains(QStringLiteral("C1 out 0 100n")));

        // Select C1 and copy/paste it: a new capacitor C2 appears.
        const QPoint c = v.schematic().symbols[2].worldBounds().center();
        QTest::mouseClick(&v, Qt::LeftButton, {}, QPoint(int(c.x() * 1.5), int(c.y() * 1.5)));
        QVERIFY(v.hasSelection());
        v.copySelection();
        v.paste();
        QTest::mouseMove(&v, QPoint(600, 400));
        QTest::mouseClick(&v, Qt::LeftButton, {}, QPoint(600, 400));
        QCOMPARE(v.schematic().symbols.size(), 4);
        QCOMPARE(v.schematic().symbols[3].instName(), QStringLiteral("C2"));

        // Delete the pasted part.
        v.deleteSelection();
        QCOMPARE(v.schematic().symbols.size(), 3);
        v.undo();
        QCOMPARE(v.schematic().symbols.size(), 4);
    }

    void waveformTracesAndCursors()
    {
        auto ds = DatasetPtr::create();
        ds->kind = Dataset::Kind::Transient;
        ds->plotName = QStringLiteral("Transient Analysis");
        ds->vars = {{QStringLiteral("time"), QStringLiteral("time")}, {QStringLiteral("V(out)"), QStringLiteral("voltage")},
                    {QStringLiteral("I(R1)"), QStringLiteral("current")}};
        Dataset::Run run;
        run.re.resize(3);
        for (int i = 0; i <= 1000; ++i) {
            const double t = i * 1e-6;
            run.re[0] << t;
            run.re[1] << 2.0 * std::sin(2 * M_PI * 1e3 * t);
            run.re[2] << 1e-3 * std::cos(2 * M_PI * 1e3 * t);
        }
        ds->runs << run;
        ds->rebuildIndex();

        WaveformView w;
        w.resize(900, 600);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setDataset(ds);
        w.addTrace(QStringLiteral("V(out)"));
        w.addTrace(QStringLiteral("I(R1)"));
        WaveformPane *pane = w.activePane();
        QVERIFY(pane);
        QCOMPARE(pane->traces.size(), 2);
        QVERIFY(pane->traces[0].error.isEmpty());
        // Volts and amps go to separate axes.
        QCOMPARE(pane->traces[0].axis, 0);
        QCOMPARE(pane->traces[1].axis, 1);
        // Autorange covers the sine amplitude.
        QVERIFY(pane->yLeft.min <= -2.0 && pane->yLeft.max >= 2.0);
        QVERIFY(pane->yLeft.max < 3.0);
        QVERIFY(std::fabs(w.xAxis.max - 1e-3) < 1e-9);

        // Expression trace with units: power = V*I.
        w.addTrace(QStringLiteral("V(out)*I(R1)"));
        QCOMPARE(pane->traces[2].unitText, QStringLiteral("W"));

        // Cursor readout value at a known point.
        w.attachCursor(0, pane, 0);
        w.cursors[0].x = 0.25e-3;
        double re = 0, im = 0;
        QVERIFY(pane->traceValue(0, 0, 0.25e-3, &re, &im));
        QVERIFY(std::fabs(re - 2.0) < 1e-3);
        w.moveCursorToPoint(0, 1);
        QVERIFY(w.cursors[0].x > 0.25e-3);

        // Panes: add and delete.
        w.addPane(false);
        QCOMPARE(w.panes().size(), 2);
        w.deletePane(w.activePane());
        QCOMPARE(w.panes().size(), 1);
    }
};

QTEST_MAIN(GuiTests)
#include "gui_tests.moc"
