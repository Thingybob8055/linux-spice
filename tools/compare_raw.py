#!/usr/bin/env python3
"""Compare two SPICE raw files (e.g. a LinSpice result against an LTspice reference).

For every node voltage present in both files the waveform of the test file is
interpolated onto the reference's sweep axis and a normalized RMS error is
computed (RMS difference divided by the reference's peak-to-peak swing).
Stepped files are compared step by step (first step only by default).

usage: compare_raw.py reference.raw test.raw [--all] [--json]
"""
import json
import math
import struct
import sys


def read_raw(path):
    data = open(path, "rb").read()
    utf16 = len(data) > 4 and data[1] == 0 and data[3] == 0
    if utf16:
        marker = "Binary:\n".encode("utf-16-le")
        alt = "Values:\n".encode("utf-16-le")
    else:
        marker, alt = b"Binary:\n", b"Values:\n"
    pos = data.find(marker)
    binary = True
    if pos < 0:
        pos = data.find(alt)
        binary = False
        marker = alt
    header = data[:pos].decode("utf-16-le" if utf16 else "latin-1")
    body = data[pos + len(marker):]
    nvars = npts = 0
    flags = ""
    names = []
    in_vars = False
    plot = ""
    for line in header.splitlines():
        if in_vars:
            parts = line.split()
            if len(parts) >= 3 and parts[0].isdigit():
                names.append(parts[1])
                continue
            in_vars = False
        if ":" not in line:
            continue
        key, val = line.split(":", 1)
        key = key.strip().lower()
        if key == "no. variables":
            nvars = int(val)
        elif key == "no. points":
            npts = int(val)
        elif key == "flags":
            flags = val.strip().lower()
        elif key == "plotname":
            plot = val.strip()
        elif key == "variables":
            in_vars = True
    complex_ = "complex" in flags
    double = "double" in flags or not utf16
    cols = [[] for _ in range(nvars)]
    if not binary:
        raise SystemExit("ASCII raw files are not supported by this tool")
    off = 0
    if complex_:
        row = 16 * nvars
        for p in range(min(npts, len(body) // row)):
            vals = struct.unpack_from("<%dd" % (2 * nvars), body, p * row)
            for v in range(nvars):
                cols[v].append(complex(vals[2 * v], vals[2 * v + 1]))
    else:
        other = 8 if double else 4
        row = 8 + other * (nvars - 1)
        fmt = "<d" + ("d" if double else "f") * (nvars - 1)
        for p in range(min(npts, len(body) // row)):
            vals = struct.unpack_from(fmt, body, p * row)
            cols[0].append(abs(vals[0]) if "transient" in plot.lower() else vals[0])
            for v in range(1, nvars):
                cols[v].append(vals[v])
    # Split steps where the sweep variable restarts.
    x = [c.real if isinstance(c, complex) else c for c in cols[0]]
    starts = [0]
    for i in range(1, len(x)):
        if x[i] < x[i - 1] or (x[i] == x[0] and x[i - 1] != x[0]):
            starts.append(i)
    steps = []
    for s, e in zip(starts, starts[1:] + [len(x)]):
        steps.append({names[v].lower(): cols[v][s:e] for v in range(nvars)})
    return {"names": names, "complex": complex_, "steps": steps, "plot": plot}


def interp(xs, ys, x):
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    lo, hi = 0, len(xs) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if xs[mid] <= x:
            lo = mid
        else:
            hi = mid
    t = (x - xs[lo]) / (xs[hi] - xs[lo]) if xs[hi] != xs[lo] else 0
    return ys[lo] + t * (ys[hi] - ys[lo])


def compare(ref, test, step=0):
    rs = ref["steps"][min(step, len(ref["steps"]) - 1)]
    ts = test["steps"][min(step, len(test["steps"]) - 1)]
    xname = ref["names"][0].lower()
    xr = [c.real if isinstance(c, complex) else c for c in rs[xname]]
    tx = test["names"][0].lower()
    xt = [c.real if isinstance(c, complex) else c for c in ts[tx]]
    results = {}
    for name in rs:
        if not name.startswith("v(") or name not in ts or name.startswith("v(nc_"):
            continue
        yr = rs[name]
        yt = ts[name]
        if ref["complex"]:
            # Compare magnitude in dB, floored at -200 dB so numerical noise on
            # nodes with no AC signal does not dominate.
            yr = [20 * math.log10(max(abs(c), 1e-10)) for c in yr]
            yt = [20 * math.log10(max(abs(c), 1e-10)) for c in yt]
            span = max(max(yr) - min(yr), 1.0)
        else:
            span = max(max(yr) - min(yr), 1e-3 * max(1.0, max(abs(v) for v in yr)), 1e-6)
        lo, hi = max(xr[0], xt[0]), min(xr[-1], xt[-1])
        acc, n = 0.0, 0
        for x, y in zip(xr, yr):
            if x < lo or x > hi:
                continue
            d = interp(xt, yt, x) - y
            acc += d * d
            n += 1
        if n:
            results[name] = math.sqrt(acc / n) / span
    return results


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    ref, test = read_raw(args[0]), read_raw(args[1])
    steps = range(len(ref["steps"])) if "--all" in sys.argv else [0]
    worst = 0.0
    out = {}
    for s in steps:
        r = compare(ref, test, s)
        out[s] = r
        if r:
            worst = max(worst, max(r.values()))
    if "--json" in sys.argv:
        print(json.dumps({"worst": worst, "steps": out}))
    else:
        for s, r in out.items():
            for k, v in sorted(r.items(), key=lambda kv: -kv[1]):
                print("step %d  %-24s %.4f" % (s, k, v))
        print("worst normalized RMS error: %.4f" % worst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
