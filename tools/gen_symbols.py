#!/usr/bin/env python3
"""Generates LinSpice's built-in fallback symbols (resources/symbols/*.asy).

The drawings are original; only the pin locations, pin order and attribute
names follow the documented .asy interface so that existing schematics
connect to them correctly. When the user imports a component library, those
symbols take precedence over these.
"""
import os
import sys

OUT = os.path.join(os.path.dirname(__file__), "..", "resources", "symbols")


def L(x1, y1, x2, y2):
    return f"LINE Normal {x1} {y1} {x2} {y2}"


def C(x1, y1, x2, y2):
    return f"CIRCLE Normal {x1} {y1} {x2} {y2}"


def R(x1, y1, x2, y2):
    return f"RECTANGLE Normal {x1} {y1} {x2} {y2}"


def A(x1, y1, x2, y2, x3, y3, x4, y4):
    return f"ARC Normal {x1} {y1} {x2} {y2} {x3} {y3} {x4} {y4}"


def sym(name, shapes, attrs, pins, windows=((0, 36, 40, "Left"), (3, 36, 76, "Left")), stype="CELL", texts=()):
    lines = ["Version 4", f"SymbolType {stype}"]
    lines += shapes
    for t in texts:
        lines.append("TEXT %d %d %s %d %s" % t)
    for w in windows:
        lines.append(f"WINDOW {w[0]} {w[1]} {w[2]} {w[3]} 2")
    for k, v in attrs:
        lines.append(f"SYMATTR {k} {v}")
    for i, p in enumerate(pins):
        x, y, pname = p[0], p[1], p[2]
        just = p[3] if len(p) > 3 else "NONE"
        off = p[4] if len(p) > 4 else 0
        lines.append(f"PIN {x} {y} {just} {off}")
        lines.append(f"PINATTR PinName {pname}")
        lines.append(f"PINATTR SpiceOrder {i + 1}")
    path = os.path.join(OUT, name + ".asy")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


def zigzag(x, y0, y1, w=12, n=6):
    pts = [(x, y0)]
    step = (y1 - y0) / n
    for i in range(n):
        pts.append((x + (w if i % 2 == 0 else -w), int(y0 + step * (i + 0.5))))
    pts.append((x, y1))
    return [L(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]) for i in range(len(pts) - 1)]


def coil(x, y0, y1, n=4):
    out = []
    h = (y1 - y0) // n
    for i in range(n):
        ya = y0 + i * h
        out.append(A(x - 10, ya, x + 10, ya + h, x, ya + h, x, ya))
    return out


def diode_body(extra=()):
    s = [L(16, 0, 16, 20), L(16, 44, 16, 64), L(0, 20, 32, 20), L(0, 20, 16, 44), L(32, 20, 16, 44), L(0, 44, 32, 44)]
    return s + list(extra)


def bjt(pnp=False, sub=False):
    s = [L(16, 16, 16, 80), L(0, 48, 16, 48), L(16, 36, 64, 8), L(64, 8, 64, 0), L(16, 60, 64, 88), L(64, 88, 64, 96)]
    if not pnp:
        s += [L(64, 88, 50, 72), L(64, 88, 46, 86)]
    else:
        s += [L(16, 60, 34, 62), L(16, 60, 26, 76)]
    if sub:
        s += [L(64, 48, 32, 48)]
    return s


def mos(pmos=False, body=False):
    s = [L(48, 0, 48, 16), L(48, 16, 20, 16), L(20, 8, 20, 24), L(20, 40, 20, 56), L(20, 72, 20, 88),
         L(20, 48, 48, 48), L(20, 80, 48, 80), L(48, 80, 48, 96), L(10, 16, 10, 80), L(0, 80, 10, 80)]
    if not pmos:
        s += [L(20, 48, 32, 42), L(20, 48, 32, 54)]
    else:
        s += [L(48, 48, 36, 42), L(48, 48, 36, 54)]
    if not body:
        s += [L(48, 48, 48, 80)]
    return s


def jfet(pjf=False):
    s = [L(48, 0, 48, 24), L(48, 24, 24, 24), L(24, 16, 24, 80), L(24, 72, 48, 72), L(48, 72, 48, 96), L(0, 64, 24, 64)]
    if not pjf:
        s += [L(24, 64, 12, 58), L(24, 64, 12, 70)]
    else:
        s += [L(0, 64, 12, 58), L(0, 64, 12, 70)]
    return s


def source_circle(y0=16, y1=96, r=32):
    cy = (y0 + y1) // 2
    return [C(-r, cy - r, r, cy + r), L(0, y0, 0, cy - r), L(0, cy + r, 0, y1)]


def main():
    vw = ((0, 24, 16, "Left"), (3, 24, 96, "Left"))
    sym("res", zigzag(16, 16, 96), [("Value", "R"), ("Prefix", "R"), ("Description", "A resistor")],
        [(16, 16, "A"), (16, 96, "B")])
    sym("res2", zigzag(16, 0, 64), [("Value", "R"), ("Prefix", "R"), ("Description", "A resistor")],
        [(16, 0, "A"), (16, 64, "B")])
    cap = [L(16, 0, 16, 26), L(16, 38, 16, 64), L(0, 26, 32, 26), L(0, 38, 32, 38)]
    sym("cap", cap, [("Value", "C"), ("Prefix", "C"), ("Description", "Capacitor")], [(16, 0, "A"), (16, 64, "B")],
        windows=((0, 24, 8, "Left"), (3, 24, 56, "Left")))
    polcap = [L(16, 0, 16, 26), L(0, 26, 32, 26), A(-8, 36, 40, 60, 0, 40, 32, 40), L(16, 38, 16, 64), L(4, 12, 12, 12), L(8, 8, 8, 16)]
    sym("polcap", polcap, [("Value", "C"), ("Prefix", "C"), ("Description", "Polarized capacitor")],
        [(16, 0, "A"), (16, 64, "B")], windows=((0, 24, 8, "Left"), (3, 24, 56, "Left")))
    ind = [L(16, 16, 16, 24), L(16, 88, 16, 96)] + coil(16, 24, 88)
    for nm in ("ind", "ind2"):
        extra = [L(32, 26, 32, 86)] if nm == "ind2" else []
        sym(nm, ind + extra, [("Value", "L"), ("Prefix", "L"), ("Description", "Inductor")],
            [(16, 16, "A"), (16, 96, "B")])
    sym("voltage", source_circle() + [L(-8, 40, 8, 40), L(0, 32, 0, 48), L(-8, 72, 8, 72)],
        [("Value", "V"), ("Prefix", "V"), ("Description", "Voltage source")], [(0, 16, "+"), (0, 96, "-")], windows=vw)
    sym("current", source_circle(0, 80) + [L(0, 20, 0, 60), L(0, 60, -8, 48), L(0, 60, 8, 48)],
        [("Value", "I"), ("Prefix", "I"), ("Description", "Current source")], [(0, 0, "+"), (0, 80, "-")],
        windows=((0, 24, 0, "Left"), (3, 24, 80, "Left")))
    dw = ((0, 24, 0, "Left"), (3, 24, 64, "Left"))
    sym("diode", diode_body(), [("Value", "D"), ("Prefix", "D"), ("Description", "Diode")], [(16, 0, "+"), (16, 64, "-")], windows=dw)
    sym("schottky", diode_body([L(0, 44, 0, 38), L(32, 44, 32, 50)]), [("Value", "D"), ("Prefix", "D"), ("Description", "Schottky diode")],
        [(16, 0, "+"), (16, 64, "-")], windows=dw)
    sym("zener", diode_body([L(0, 44, -6, 38), L(32, 44, 38, 50)]), [("Value", "D"), ("Prefix", "D"), ("Description", "Zener diode")],
        [(16, 0, "+"), (16, 64, "-")], windows=dw)
    sym("LED", diode_body([L(36, 24, 48, 16), L(48, 16, 42, 16), L(36, 34, 48, 26), L(48, 26, 42, 26)]),
        [("Value", "D"), ("Prefix", "D"), ("Description", "Light emitting diode")], [(16, 0, "+"), (16, 64, "-")], windows=dw)
    sym("varactor", diode_body([L(0, 50, 32, 50)]), [("Value", "D"), ("Prefix", "D"), ("Description", "Varactor")],
        [(16, 0, "+"), (16, 64, "-")], windows=dw)
    sym("TVSdiode", diode_body([L(0, 20, 16, 0), L(32, 20, 16, 0)]), [("Value", "D"), ("Prefix", "D"), ("Description", "TVS diode")],
        [(16, 0, "+"), (16, 64, "-")], windows=dw)

    qw = ((0, 56, 32, "Left"), (3, 56, 68, "Left"))
    for nm, pnp in (("npn", False), ("npn2", False), ("npn3", False), ("pnp", True), ("pnp2", True)):
        pfx = "QP" if pnp else "QN"
        sym(nm, bjt(pnp), [("Value", "PNP" if pnp else "NPN"), ("Prefix", pfx), ("Description", "Bipolar transistor")],
            [(64, 0, "C"), (0, 48, "B"), (64, 96, "E")], windows=qw)
    for nm, pnp in (("npn4", False), ("pnp4", True), ("lpnp", True)):
        sym(nm, bjt(pnp, True), [("Value", "PNP" if pnp else "NPN"), ("Prefix", "QP" if pnp else "QN"),
                                  ("Description", "Bipolar transistor with substrate")],
            [(64, 0, "C"), (0, 48, "B"), (64, 96, "E"), (64, 48, "S")], windows=qw)

    mw = ((0, 56, 32, "Left"), (3, 56, 72, "Left"))
    sym("nmos", mos(False), [("Value", "NMOS"), ("Prefix", "MN"), ("Description", "N-channel MOSFET")],
        [(48, 0, "D"), (0, 80, "G"), (48, 96, "S")], windows=mw)
    sym("pmos", mos(True), [("Value", "PMOS"), ("Prefix", "MP"), ("Description", "P-channel MOSFET")],
        [(48, 0, "D"), (0, 80, "G"), (48, 96, "S")], windows=mw)
    sym("nmos4", mos(False, True), [("Value", "NMOS"), ("Prefix", "MN"), ("Description", "N-channel MOSFET (4 terminal)")],
        [(48, 0, "D"), (0, 80, "G"), (48, 96, "S"), (48, 48, "B")], windows=mw)
    sym("pmos4", mos(True, True), [("Value", "PMOS"), ("Prefix", "MP"), ("Description", "P-channel MOSFET (4 terminal)")],
        [(48, 0, "D"), (0, 80, "G"), (48, 96, "S"), (48, 48, "B")], windows=mw)
    sym("njf", jfet(False), [("Value", "NJF"), ("Prefix", "JN"), ("Description", "N-channel JFET")],
        [(48, 0, "D"), (0, 64, "G"), (48, 96, "S")], windows=mw)
    sym("pjf", jfet(True), [("Value", "PJF"), ("Prefix", "JP"), ("Description", "P-channel JFET")],
        [(48, 0, "D"), (0, 64, "G"), (48, 96, "S")], windows=mw)
    sym("mesfet", mos(False), [("Value", "NMF"), ("Prefix", "Z"), ("Description", "MESFET")],
        [(48, 0, "D"), (0, 80, "G"), (48, 96, "S")], windows=mw)

    box = [R(-16, 16, 16, 96)]
    sym("sw", [L(0, 16, 0, 40), L(0, 72, 0, 96), L(0, 72, 14, 42), R(-56, 24, -8, 88), L(-48, 32, -32, 32), L(-48, 80, -32, 80)],
        [("Value", "SW"), ("Prefix", "S"), ("Description", "Voltage controlled switch")],
        [(0, 16, "A"), (0, 96, "B"), (-48, 80, "NC+"), (-48, 32, "NC-")], windows=vw)
    sym("csw", [L(0, 0, 0, 24), L(0, 56, 0, 80), L(0, 56, 14, 26)],
        [("Value", "CSW"), ("Prefix", "W"), ("Description", "Current controlled switch")], [(0, 0, "+"), (0, 80, "-")],
        windows=((0, 24, 0, "Left"), (3, 24, 80, "Left")))
    diamond = lambda y0, y1: [L(0, y0, -24, (y0 + y1) // 2), L(-24, (y0 + y1) // 2, 0, y1), L(0, y1, 24, (y0 + y1) // 2),
                              L(24, (y0 + y1) // 2, 0, y0)]
    sym("e", diamond(32, 80) + [L(0, 16, 0, 32), L(0, 80, 0, 96), L(-48, 32, -32, 32), L(-48, 80, -32, 80)],
        [("Value", "E"), ("Prefix", "E"), ("Description", "Voltage dependent voltage source")],
        [(0, 16, "+"), (0, 96, "-"), (-48, 32, "P"), (-48, 80, "N")], windows=vw)
    sym("e2", diamond(32, 80) + [L(0, 16, 0, 32), L(0, 80, 0, 96), L(-48, 32, -32, 32), L(-48, 80, -32, 80)],
        [("Value", "E"), ("Prefix", "E"), ("Description", "Voltage dependent voltage source")],
        [(0, 16, "+"), (0, 96, "-"), (-48, 80, "P"), (-48, 32, "N")], windows=vw)
    sym("g", diamond(32, 80) + [L(0, 16, 0, 32), L(0, 80, 0, 96), L(0, 70, 0, 42), L(0, 42, -6, 52), L(0, 42, 6, 52)],
        [("Value", "G"), ("Prefix", "G"), ("Description", "Voltage dependent current source")],
        [(0, 96, "+"), (0, 16, "-"), (-48, 32, "NC+"), (-48, 80, "NC-")], windows=vw)
    sym("g2", diamond(32, 80) + [L(0, 16, 0, 32), L(0, 80, 0, 96), L(0, 70, 0, 42), L(0, 42, -6, 52), L(0, 42, 6, 52)],
        [("Value", "G"), ("Prefix", "G"), ("Description", "Voltage dependent current source")],
        [(0, 96, "+"), (0, 16, "-"), (-48, 80, "NC+"), (-48, 32, "NC-")], windows=vw)
    sym("f", diamond(16, 64) + [L(0, 0, 0, 16), L(0, 64, 0, 80), L(0, 56, 0, 24), L(0, 56, -6, 46), L(0, 56, 6, 46)],
        [("Value", "F"), ("Prefix", "F"), ("Description", "Current dependent current source")], [(0, 0, "+"), (0, 80, "-")],
        windows=((0, 24, 0, "Left"), (3, 24, 80, "Left")))
    sym("h", diamond(32, 80) + [L(0, 16, 0, 32), L(0, 80, 0, 96)],
        [("Value", "H"), ("Prefix", "H"), ("Description", "Current dependent voltage source")], [(0, 16, "+"), (0, 96, "-")], windows=vw)
    sym("bv", source_circle() + [L(-8, 40, 8, 40), L(0, 32, 0, 48)],
        [("Value", "V=F(...)"), ("Prefix", "B"), ("Description", "Arbitrary behavioral voltage source")],
        [(0, 16, "+"), (0, 96, "-")], windows=vw)
    sym("bi", source_circle(0, 80) + [L(0, 20, 0, 60), L(0, 60, -8, 48), L(0, 60, 8, 48)],
        [("Value", "I=F(...)"), ("Prefix", "B"), ("Description", "Arbitrary behavioral current source")],
        [(0, 0, "+"), (0, 80, "-")], windows=((0, 24, 0, "Left"), (3, 24, 80, "Left")))
    sym("bi2", source_circle(0, 80) + [L(0, 60, 0, 20), L(0, 20, -8, 32), L(0, 20, 8, 32)],
        [("Value", "I=F(...)"), ("Prefix", "B"), ("Description", "Arbitrary behavioral current source")],
        [(0, 80, "+"), (0, 0, "-")], windows=((0, 24, 0, "Left"), (3, 24, 80, "Left")))
    tl = [R(-32, -24, 32, 24), L(-48, -16, -32, -16), L(-48, 16, -32, 16), L(32, -16, 48, -16), L(32, 16, 48, 16)]
    sym("tline", tl, [("Value", "Td=50n Z0=50"), ("Prefix", "T"), ("Description", "Lossless transmission line")],
        [(-48, -16, "I1"), (-48, 16, "R1"), (48, -16, "I2"), (48, 16, "R2")], windows=((0, 0, -40, "Center"), (3, 0, 40, "Center")))
    sym("ltline", tl, [("Value", "LTRA"), ("Prefix", "O"), ("Description", "Lossy transmission line")],
        [(-48, -16, "I1"), (-48, 16, "R1"), (48, -16, "I2"), (48, 16, "R2")], windows=((0, 0, -40, "Center"), (3, 0, 40, "Center")))
    sym("load", [R(0, 12, 32, 52), L(16, 0, 16, 12), L(16, 52, 16, 64), L(16, 18, 16, 46), L(16, 46, 10, 38), L(16, 46, 22, 38)],
        [("Value", "1"), ("Prefix", "I"), ("SpiceLine", "load"), ("Description", "Current load")],
        [(16, 0, "A"), (16, 64, "B")], windows=dw)
    sym("load2", source_circle(0, 80) + [L(0, 20, 0, 60)],
        [("Value", "1"), ("Prefix", "I"), ("SpiceLine", "load"), ("Description", "Current load")],
        [(0, 0, "+"), (0, 80, "-")], windows=((0, 24, 0, "Left"), (3, 24, 80, "Left")))

    # Generic op amp and its self-contained single-pole model (resources/sub/opamp.sub).
    tri = [L(-32, 32, -32, 96), L(-32, 32, 32, 64), L(32, 64, -32, 96), L(-28, 48, -20, 48), L(-24, 76, -24, 84), L(-28, 80, -20, 80)]
    sym("OpAmps/opamp", tri, [("Prefix", "X"), ("Value", "opamp"), ("SpiceLine", "Aol=100K"), ("SpiceLine2", "GBW=10Meg"),
                              ("Description", "Ideal single-pole operational amplifier (needs .lib opamp.sub)")],
        [(-32, 48, "invin"), (-32, 80, "noninvin"), (32, 64, "out")], windows=((0, 0, 32, "Left"), (3, 0, 104, "Left")))

    gate_pins = [(-32, 32, "a"), (-32, 48, "b"), (-32, 64, "c"), (-32, 80, "d"), (-32, 96, "e"), (32, 80, "_Q"), (32, 48, "Q"), (-16, 96, "com")]
    gate = [L(-16, 24, -16, 104), L(-16, 24, 8, 24), A(-24, 24, 32, 104, 8, 104, 8, 24), L(-16, 104, 8, 104)]
    for g in ("and", "or", "xor"):
        sym(f"Digital/{g}", gate, [("Prefix", "A"), ("SpiceModel", g.upper()), ("Description", f"Behavioral {g.upper()} gate")],
            gate_pins, windows=((0, 16, 24, "Left"), (3, 16, 112, "Left")))
    bufp = [(-32, 48, "a"), (-32, 64, "b"), (-32, 80, "c"), (-32, 96, "d"), (-32, 112, "e"), (32, 64, "_Q"), (32, 48, "Q"), (-16, 112, "com")]
    for g, model in (("buf", "BUF"), ("inv", "INV"), ("buf1", "BUF")):
        sym(f"Digital/{g}", [L(-16, 16, -16, 80), L(-16, 16, 32, 48), L(32, 48, -16, 80)],
            [("Prefix", "A"), ("SpiceModel", model), ("Description", f"Behavioral {model}")], bufp,
            windows=((0, 16, 24, "Left"), (3, 16, 112, "Left")))

    with open(os.path.join(OUT, "..", "sub_opamp.sub"), "w", newline="\n") as f:
        f.write("* LinSpice single-pole op amp model\n"
                ".subckt opamp inn inp out params: Aol=100K GBW=10Meg\n"
                "G1 0 out inp inn {Aol}\n"
                "R1 out 0 1\n"
                "C1 out 0 {Aol/GBW/(2*pi)}\n"
                ".ends opamp\n")

    # Write resources.qrc
    files = []
    for root, _, fs in os.walk(OUT):
        for fn in sorted(fs):
            rel = os.path.relpath(os.path.join(root, fn), os.path.join(OUT, ".."))
            files.append(rel)
    icons_dir = os.path.join(OUT, "..", "icons")
    icon_files = sorted(os.listdir(icons_dir)) if os.path.isdir(icons_dir) else []
    with open(os.path.join(OUT, "..", "resources.qrc"), "w") as f:
        f.write("<RCC>\n  <qresource prefix=\"/\">\n")
        for rel in sorted(files):
            f.write(f"    <file>{rel}</file>\n")
        for ic in icon_files:
            f.write(f"    <file>icons/{ic}</file>\n")
        f.write("    <file alias=\"sub/opamp.sub\">sub_opamp.sub</file>\n")
        f.write("  </qresource>\n</RCC>\n")
    print(f"wrote {len(files)} symbols")


if __name__ == "__main__":
    sys.exit(main())
