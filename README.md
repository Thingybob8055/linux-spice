# LinSpice

LinSpice is a native Linux schematic editor, waveform viewer and circuit simulator that works with LTspice files: it opens `.asc` schematics, `.asy` symbols, `.net` netlists, `.raw` waveforms and `.plt` plot settings, follows LTspice's editing conventions and keyboard shortcuts, and simulates with [ngspice](https://ngspice.sourceforge.io/) after translating LTspice's netlist dialect.

It is a clean-room implementation built from LTspice's published documentation, its file formats and example files, and black-box comparison of simulation results. It contains no LTspice code and does not ship Analog Devices' component libraries; you import those from your own LTspice installation.

## Building

Requirements:

- Qt 6.5 or newer (Core, Gui, Widgets, Svg, PrintSupport, Concurrent, Test)
- CMake 3.24 or newer and a C++20 compiler
- zlib
- libngspice (the shared library, built with XSPICE code models — most distributions' packages are)

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build      # optional: binary, desktop entry, icon, MIME types
```

With Nix, `nix-shell shell.nix` provides every dependency. The build records where it found libngspice, so the resulting binary also runs outside the shell (as long as that store path isn't garbage-collected); otherwise set `LINSPICE_NGSPICE_LIB` or the path in **Tools ▸ Settings**.

## First run

LinSpice needs LTspice's component library (`sym/`, `sub/`, `cmp/`) for anything beyond basic parts. On first start it offers to import it from:

- an LTspice installation under Wine (it looks in `$WINEPREFIX` and `~/.wine`),
- a folder containing LTspice's `lib.zip` and `examples.zip`, or
- an already extracted `lib` directory.

The library is copied to `~/.local/share/LinSpice/LinSpice/lib`; examples go to `~/.local/share/LinSpice/LinSpice/examples` and open with **File ▸ Open Examples**. **Tools ▸ Import LTspice Library** repeats the import later. Models that Analog Devices distributes encrypted cannot be simulated with ngspice; LinSpice reports them by name.

## Running

```sh
linspice [files...]            # schematics, symbols, netlists or .raw files
linspice -b circuit.asc        # batch mode: writes circuit.raw, .log and .net next to the file
```

Environment variables:

| Variable | Purpose |
|---|---|
| `LINSPICE_NGSPICE_LIB` | Path to `libngspice.so` if it is not on the library path |
| `LINSPICE_LIBRARY_ROOT` | Use this component library instead of the imported one |
| `LINSPICE_EXAMPLES_ROOT` | Use this examples folder |

Everything else is in **Tools ▸ Settings**.

## What is supported

Editing: wires, components, net labels, text and directives, move/drag/duplicate/delete, rotate/mirror, undo/redo, hierarchy (blocks and sub-sheets), the symbol editor with automatic symbol generation from subcircuits, the component browser, BOM and netlist export, printing.

Analyses: `.tran` (including `startup`), `.ac`, `.dc`, `.op`, `.noise`, `.tf`, `.fra` (frequency response analysis with `@` analyzers and `&` probes), `.net` two-port parameters, `.step` (nested, list/lin/oct/dec, parameters, elements, temperature, model parameters; steps run in parallel), Monte Carlo (`mc()`, `gauss()`, `flat()`, reproducible seeds), `.meas`, `.four`, `.wave`, `.param`, `.func`, `.include`/`.lib`.

Devices beyond plain SPICE: LTspice's ideal diode and smooth/level-2 switches, behavioral sources with `if`/`table`/`limit`/`u`/`rand`/…, Laplace sources (including `exp(-s*T)` delays and fractional powers such as `sqrt(1+s*tau)`), VDMOS, IGBT (Hefner model), digital gates, flip-flops, Schmitt triggers, `SAMPLEHOLD`, `MODULATOR`, `PHASEDET`, `OTA`, `VARISTOR`, `wavefile=` sources, `Rser`/`Lser`/`Cpar`/`Rpar` on passives, and LTspice's default component values and model defaults where they differ from ngspice's.

Waveform viewer: multiple panes, Bode/Nyquist/Cartesian AC display, expressions with units, cursors, FFT, zoom and pan, `.plt` files, LTspice-compatible `.raw` output.

## Compatibility with LTspice

LinSpice is checked against LTspice by running the same circuits in both (LTspice under Wine) and comparing the results signal by signal (`tools/compare_raw.py`). The figures below come from LTspice 26.1's `Educational` examples and from purpose-built test circuits.

### Summary

Of the 82 `Educational` examples, 80 run in LinSpice. Of those, 59 match LTspice within 1% normalized RMS error over every saved signal, and 67 within 5%. Most of the rest are free-running oscillators, whose waveforms slowly drift apart in time while their frequency and amplitude still match (see below). The two that don't run use an encrypted vendor model (`Royer`) and the hysteretic core inductor (`NonLinearTransformer`).

### Matches LTspice

| Area | What was checked |
|---|---|
| Files | `.asc`, `.asy`, `.net` and `.plt` open as in LTspice. `.raw` output uses LTspice's layout, plot names and signal names. Saved plot settings (panes, traces, axes, dB/log scales) are applied when a simulation finishes. |
| Integration defaults | Trapezoidal, `trtol=2`, maximum step Tstop/1024, as LTspice documents and as its own time steps show. |
| Signals you can probe | Every signal LTspice saves exists under the same name. This covers node voltages, device and terminal currents (`I(R1)`, `Ic(Q1)`, `Id(M1)`), subcircuit pin currents (`Ix(u1:3)`, named after the subcircuit's ports), special-function pin currents (`I1`…`I8(A1)`), IGBT terminal currents, inductor flux (`L1:flux`), currents in `.ac` analyses, and in `.noise` every device's contribution (`V(R1)`, `V(Q1.rb)`, …) plus `gain`. A component's current includes its `Rpar`/`Cpar`, as in LTspice. Non-zero currents agree with LTspice's within the same tolerance as the voltages. |
| Click-to-probe | Clicking any pin, part or wire in the schematic plots the same signal LTspice would. In a noise analysis, clicking a part plots its noise contribution. All 3,956 pin and net probes in the 80 runnable examples resolve. |
| Noise | Every per-device noise contribution in the `noise` and `NoiseFigure` examples is within 0.1 dB of LTspice. A stepped single-frequency `.noise` or stepped `.op` becomes one trace against the stepped value, as in LTspice. |
| Laplace sources | Rational transfer functions, `exp(-s*T)` delays and fractional powers such as `1/sqrt(1+s*tau)`. Fractional powers are within 0.004 dB and 0.014° of the exact response. In the time domain this is closer to exact than LTspice: in `HalfSlope`'s tail, LTspice is 4–19% off the exact answer and LinSpice is within 1%. |
| IGBT | Hefner's published model (the one LTspice uses), checked against LTspice with six parameter sets and two load types. DC curves are within 0.03%, on-state voltage within 0.15%, and switching delays, rise and fall times within 2%. |
| Oscillators | LC, RC and multivibrator examples oscillate at LTspice's frequency within 0.05% (including LTspice's own numerical frequency shift), with amplitude within 1%. |
| Other features | `.step` (all forms), Monte Carlo functions, `.meas`, `.four`, `.wave`, `wavefile=` sources (`wavein` matches exactly), `.fra` (within 0.3 dB in `fra_eg1`), `.net` two-port parameters, and LTspice's default component values and model defaults where they differ from ngspice's. |

### Differs from LTspice

- **Oscillator waveforms drift apart over time.** Frequency and amplitude match, but over hundreds of cycles the waveforms separate in time. LTspice's own edge timing varies by about 1% from cycle to cycle, and no two simulators reproduce that exactly. Compare oscillators by frequency and amplitude, not by overlaying late waveforms. This accounts for most of the examples outside 1%: `colpits2`, `Pierce`, `Clapp`, `relax`, `NE555`, `astable`, `phaseshift`, `Hartly`, `MC1648`, `colpits`, `phaseshift2`, the parametric oscillators `P2` and `Electrometer`, and the PLL-based `PLL` and `PLL2`.
- **Some switching oscillators run slightly slow.** `relax`, `NE555` and `colpits2` (built from transistor-level or op-amp macromodels) run 0.4–0.8% slow, because LTspice cuts its time step harder at their switching edges. Adding `.options xmu=0.45` to such a schematic brings `relax` and `NE555` within about 0.1% (see the next item for when not to use it).
- **Integration ringing.** With trapezoidal integration, currents in capacitors and diodes often alternate in sign from one time step to the next after a sharp edge. LTspice's results show this too, in the same examples and usually with similar amplitude. Its "modified trapezoidal" method, which ngspice doesn't have, damps it more in some cases, such as a hard-switched diode's reverse recovery (±0.19 A here vs ±0.008 A in LTspice in one test). Node voltages are unaffected. For switching and power circuits, `.options xmu=0.45` damps the ringing; it also makes the `dimmer` example's triac latch reliably and match LTspice. Don't use it on LC or linear oscillators, which it damps out.
- **`dimmer`:** without that option, the triac occasionally fails to latch at a zero crossing.
- **Monte Carlo** (`MonteCarlo` example): `mc()`, `gauss()` and `flat()` behave as documented and are reproducible, but they draw different random numbers from LTspice's, so individual runs differ.
- **Small residual differences:** `ISO16750-2_example`, `ISO7637-2_example`, `160` and `2ndOrderNotch` (at the bottom of the notch) are within 1.3–3.5%.
- **Laplace current source current:** LTspice's `I(G1)` can differ from the current the source actually delivers. In `HalfSlope` it violates Kirchhoff's current law. LinSpice reports the delivered current.
- **Extra signals.** LinSpice also saves subcircuit-internal signals (for probing inside the hierarchy) and the currents of ideal op-amp input pins (zero), which LTspice omits. It ignores `.save` and saves everything.
- **Currents that are effectively zero** (picoamps and below) differ by roundoff.
- **Speed.** ngspice is slower than LTspice on some large behavioral circuits and on IGBT DC sweeps.

### Not supported

- **Encrypted models.** Analog Devices distributes some models encrypted, and ngspice can't simulate them; LinSpice reports them by name. Unencrypted models, including third-party ones, work.
- **Hysteretic core inductors** (`Hc=`, `Br=`, `Bs=`) are simulated as linear inductors.
- **Transmission line terminal currents** other than `Ia(T1)` aren't available, because ngspice's line model doesn't have them.
- **IGBT internal sub-device currents** (`Id(m1:Z1)`, `I(cdg:Z1)`) aren't available; the terminal currents are.
- `.machine` state machines, `.savestate`/`.loadstate` and `.savebias`.

## Development

```sh
build/core_tests                                           # unit tests
QT_QPA_PLATFORM=offscreen build/gui_tests                  # GUI interaction tests
build/core_tests --corpus <examples-dir> --translate       # parse, netlist and translate every example
build/core_tests --netlist file.asc                        # show the LTspice and ngspice netlists
python3 tools/compare_raw.py reference.raw result.raw      # compare two waveform files
linspice --probe-audit file.asc                            # simulate, then check every schematic probe resolves
```

Source layout:

- `src/core` — file formats, netlisting, the LTspice→ngspice translator, expressions, `.meas`, `.step`, FFT, FRA and two-port analysis
- `src/sim` — the simulation worker (ngspice in a child process) and controller
- `src/gui` — schematic editor, symbol editor, waveform viewer and dialogs
- `tests`, `tools`, `packaging`
