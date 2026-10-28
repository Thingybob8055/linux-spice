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

Of the 82 `Educational` examples, 80 run in LinSpice. Of those, 61 match LTspice within 1% normalized RMS error over every saved signal, and 70 within 5%. Most of the rest are free-running oscillators, whose waveforms slowly drift apart in time while their frequency and amplitude still match (see below). The two that don't run use an encrypted vendor model (`Royer`) and the hysteretic core inductor (`NonLinearTransformer`).

### Matches LTspice

| Area | What was checked |
|---|---|
| Files | `.asc`, `.asy`, `.net` and `.plt` open as in LTspice. `.raw` output uses LTspice's layout, plot names and signal names. Saved plot settings (panes, traces, axes, dB/log scales) are applied when a simulation finishes. |
| Integration defaults | Trapezoidal, `trtol=2`, maximum step Tstop/1024, as LTspice documents and as its own time steps show. With a start time, the step inside the saved window is also kept to (Tstop−Tstart)/250, as LTspice does (a Hartley oscillator saved over its last 5 µs went from 0.65% slow to within 0.05%). If a run shows sustained step-to-step ringing after a fast edge, it is rerun with `trtol=0.5` (see NE555 below). |
| Signals you can probe | Every signal LTspice saves exists under the same name. This covers node voltages, device and terminal currents (`I(R1)`, `Ic(Q1)`, `Id(M1)`), subcircuit pin currents (`Ix(u1:3)`, named after the subcircuit's ports), special-function pin currents (`I1`…`I8(A1)`), IGBT terminal currents, inductor flux (`L1:flux`), currents in `.ac` analyses, and in `.noise` every device's contribution (`V(R1)`, `V(Q1.rb)`, …) plus `gain`. A component's current includes its `Rpar`/`Cpar`, as in LTspice. Non-zero currents agree with LTspice's within the same tolerance as the voltages. |
| Click-to-probe | Clicking any pin, part or wire in the schematic plots the same signal LTspice would. In a noise analysis, clicking a part plots its noise contribution. All 3,956 pin and net probes in the 80 runnable examples resolve. |
| Noise | Every per-device noise contribution in the `noise` and `NoiseFigure` examples is within 0.1 dB of LTspice. A stepped single-frequency `.noise` or stepped `.op` becomes one trace against the stepped value, as in LTspice. |
| Laplace sources | Rational transfer functions, `exp(-s*T)` delays and fractional powers such as `1/sqrt(1+s*tau)`. Fractional powers are within 0.004 dB and 0.014° of the exact response. In the time domain this is closer to exact than LTspice: in `HalfSlope`'s tail, LTspice is 4–19% off the exact answer and LinSpice is within 1%. |
| IGBT | Hefner's published model (the one LTspice uses), checked against LTspice with six parameter sets and two load types. DC curves are within 0.03%, on-state voltage within 0.15%, and switching delays, rise and fall times within 2%. |
| Oscillators | LC, RC and multivibrator examples oscillate at LTspice's frequency within 0.05% (including LTspice's own numerical frequency shift), with amplitude within 1%. |
| Guitar pedals | Nine pedal simulations from [Cushychicken/ltspice-guitar-pedals](https://github.com/Cushychicken/ltspice-guitar-pedals): TS808 Tube Screamer, Big Cheese fuzz, Crybaby wah and Boss GE-7 EQ within 0.004 dB of LTspice across their swept settings; ProCo RAT within 0.9 dB (its LT1008 op-amp model's bandwidth is ~9% low at 1 MHz); Green Ringer and Schaller tremolo waveforms within 0.07%; Colorsound Phase-X LFO frequency within 0.3% and bias voltages within 1 mV. |
| More pedal projects | Eleven independent GitHub projects (16 schematics, see `more-pedal-tests/README.md` if present): a germanium PNP Fuzz Face (three variants, including a 20 s guitar render from a `.wav`), a silicon Fuzz Face, a modded Tube Screamer, an op-amp distortion with germanium diodes, an "ultimate overdrive" and an LPB-1 booster all match LTspice: AC sweeps within 0.002 dB, waveforms within 0.3% except right at fuzz clipping edges (see below). The other schematics fail identically in LTspice (models or input files the authors didn't publish). |
| Other GitHub projects | 23 independent projects, 72 schematics (amplifiers, filters, power supplies, converters, servo and class-D amplifiers, more pedals). 22 can be compared; the rest use encrypted models, symbols the authors didn't publish, or LTspice itself reports errors on them. 15 match within 1%: box-a-lator (19 swept steps), an LM13700 OTA test, resonant and BJT amplifiers, IGBT and MOSFET characteristics, rectifiers, Dumble/Two-Rock tone stacks and others. A Fuzz Face, an adjustable supply, a JFET amplifier and a 380 V buck converter's output ripple are within 1–4%. A 201-step MOSFET servo frequency sweep agrees within 0.15% in its `.meas` results, but 2 of its steps stop with "timestep too small". The remaining two are a filter driven by `white()` noise and a transformer with a floating primary (outputs agree within 2 mV). |
| Node names | Auto-named nets match LTspice's netlists, including `P001`, `P002`, … for pins that touch without a wire (numbered in the order of the first symbol in the file that touches them), so probes and saved plots refer to the same signals. |
| Floating nodes | A node reached only through capacitors settles where LTspice puts it: each capacitor on it conducts C × 2⁻⁴³ S/F and the group has 2⁻⁴³ S to ground, so it sits at Σ(C·V)/(1 F + ΣC); a dangling node (one connection, or a chain of them) follows the far side. Checked with 29 test circuits; the absolute value 2⁻⁴³ S comes from an AC sweep starting at 1 µHz (`contrib/gr_del` matches exactly). Circuits with such groups find their operating point by gmin stepping, which lands on it exactly: ngspice's direct method stops within 1 µV, enough to kick an oscillator whose tank floats into oscillating where LTspice keeps it still. |
| More GitHub projects | 10 further schematics from 10 projects (EMG and ECG front ends, Moog/Buchla filters, a Fuzz Face, an overdrive, an EMI filter, a pulsed ECM supply, a protected linear supply, a heartbeat monitor). Overdrive, Fuzz Face, Moog ladder and EMG board are identical; the ECM supply and heartbeat monitor are within 3%; the linear supply's protection latch trips and resets at the same times. The EMI filter's mains side has no DC path to ground: voltages across its parts agree, but its level relative to ground differs (LTspice ≈ 0 V, LinSpice ≈ −340 V). |
| Switches and vendor models | A voltage/current switch without hysteresis stays off while its control sits exactly at Vt, as in LTspice (ngspice turns it on). A capacitor with `Lser` gets `Cpar` ≥ C × 1e-6 like LTspice's "Increased Cpar". Resistors written `R n1 n2 MODEL value` (TI op-amp models) keep their value. |
| Behavioral comparators | A comparison inside a behavioral source (`if(V(a)>V(b),…)`, `V(x)>=0.5`) switches at the crossing, as in LTspice, which locates such crossings to about a nanosecond. ngspice alone stepped over them, so a PWM comparator switched microseconds late: an SPWM inverter's load current had a 0.23 A DC offset and was 2.4% high. It now matches LTspice's `.four` results (10.29 A, −37.31° vs −37.30°). |
| Convergence | When ngspice reports "timestep too small" or a failed operating point, the run is retried automatically with looser current tolerances, then with shunts and gear integration. The log says which settings were used. A MOSFET switched fully on but carrying only nanoamps at 270 V never met ngspice's 1 pA tolerance from rounding noise alone, where LTspice runs. |
| Round-5 GitHub projects | 10 further user circuits (Colpitts, Hartley and DTMF oscillators, class-AB and discrete amplifiers, a boost converter, an SPWM inverter, a Peltz oscillator): class-AB and discrete amplifiers within 0.3%, `.four` THD 0.2141% vs 0.2143%; oscillators within 0.1% in frequency and 2% in amplitude; boost output within 0.05%, switch edges within 8 ns. The 8 `Educational` examples in subfolders (`LM741`, `contrib/*`, `PAsystem/*`) also match, except `elip_grd` below. |
| Round-6 GitHub projects | 9 comparable user circuits (a 10th runs over 50 minutes in LTspice). First try, before fixes: 6 matched (discrete and class-AB amplifiers, lossy buck and analog boost converters within 0.2%, a two-transistor RC oscillator within 0.05% in frequency, and a 100-run Monte Carlo buck-boost whose `.meas` statistics agree within the run-to-run spread). Fixed afterwards: a Colpitts oscillator that LTspice holds at its operating point (2403 → 0.004) and a photodiode front end's AC response (3.69 → 0.0000). |
| Behavioral sources in `.ac` | As in LTspice, a behavioral source that uses a device's current (`I(R1)`, `I(D1)`, `I(C1)`, another behavioral source's) holds it at its operating-point value in `.ac` and `.noise`; voltages and the currents of voltage sources and inductors are linearized. |
| Ideal diodes | Default `Ron` is 1 mΩ, as LTspice documents (it was 1 Ω). |
| Pulse sources | Periodic `PULSE` sources never lose a pulse. ngspice can step over a whole pulse late in a long run (a rounding error in its period arithmetic); LinSpice adds a silent breakpoint companion that prevents this. A 380 V buck converter averaged 16.7 V instead of LTspice's 20.12 V before; it now matches. |
| Op-amp macromodels | LTspice-library op-amps built from its special devices: OTAs, ideal diodes, thresholded square-law current sources, VDMOS without subthreshold conduction. An AD820 buffer matches LTspice's output within 0.1 mV and its supply current within 0.2%. |
| Zener diodes | Low-current breakdown (`Ibvl`, `Nbvl`) included: a 1N750 is within 3 mV of LTspice from 1 µA to 20 mA. |
| Other features | `.step` (all forms), Monte Carlo functions, `.meas`, `.four` (computed from the waveforms like LTspice, with its harmonic count and period arguments and `{param}` frequencies), `.wave`, `wavefile=` sources (`wavein` matches exactly), `.fra` (within 0.3 dB in `fra_eg1`), `.net` two-port parameters, and LTspice's default component values and model defaults where they differ from ngspice's. |

### Differs from LTspice

- **Oscillator waveforms drift apart over time.** Frequency and amplitude match, but over hundreds of cycles the waveforms separate in time. LTspice's own edge timing varies by about 1% from cycle to cycle, and no two simulators reproduce that exactly. Compare oscillators by frequency and amplitude, not by overlaying late waveforms. This accounts for most of the examples outside 1%: `colpits2`, `Pierce`, `Clapp`, `relax`, `NE555`, `astable`, `phaseshift`, `Hartly`, `MC1648`, `colpits`, `phaseshift2`, the parametric oscillators `P2` and `Electrometer`, and the PLL-based `PLL` and `PLL2`.
- **Some switching oscillators run slightly slow.** `relax`, `NE555` and `colpits2` (built from transistor-level or op-amp macromodels) run 0.4–0.8% slow, because LTspice cuts its time step harder at their switching edges. Adding `.options xmu=0.45` to such a schematic brings `relax` and `NE555` within about 0.1% (see the next item for when not to use it).
- **Fast regenerative edges** (the 555's output flip, for example) can be crossed in a single time step, where LTspice shrinks its step to a few nanoseconds. Trapezoidal integration then rings: in `NE555` the unloaded output zigzagged 1.8 V every step and sat above the supply. LinSpice detects this ringing and reruns with `trtol=0.5`, which resolves every edge: `NE555`'s high level and its peak are then within 0.01 V of LTspice, with no overshoot, and its frequency within 0.01%. (A tighter `trtol` is not the default because it shifts LC oscillators 0.1–0.8% away from LTspice's frequency.) The rerun makes such circuits about 3× slower.
- **Integration ringing.** With trapezoidal integration, currents in capacitors and diodes often alternate in sign from one time step to the next after a sharp edge. LTspice's results show this too, in the same examples and usually with similar amplitude. Its "modified trapezoidal" method, which ngspice doesn't have, damps it more in some cases, such as a hard-switched diode's reverse recovery (±0.19 A here vs ±0.008 A in LTspice in one test). Node voltages are unaffected. For switching and power circuits, `.options xmu=0.45` damps the ringing; it also makes the `dimmer` example's triac latch reliably and match LTspice. Don't use it on LC or linear oscillators, which it damps out.
- **`dimmer`:** without that option, the triac occasionally fails to latch at a zero crossing.
- **Monte Carlo and noise functions** (`MonteCarlo` example): `mc()`, `gauss()`, `flat()` and `white()` behave as documented and are reproducible, but they draw different random numbers from LTspice's, so individual runs differ. A filter driven by `white()` has the same noise level as in LTspice, not the same waveform.
- **Small residual differences:** `ISO16750-2_example`, `ISO7637-2_example`, `160` and `2ndOrderNotch` (at the bottom of the notch) are within 1.3–3.5%.
- **`uic` start-up.** With `uic`, LTspice starts every node, supplies included, at 0 V and the sources jump on the first step; ngspice starts from a solution consistent with the sources. A marginally stable oscillator then starts from a different seed (a phase-shift oscillator's growth rate matches, but its amplitude stays about 4× smaller).
- **Unstable equilibria.** A circuit started exactly at an unstable operating point (a Peltz oscillator with no start-up kick) stays there in LTspice, while LinSpice's rounding noise starts it oscillating after about 100 µs, as the real circuit would.
- **AC sweeps down to 1 µHz with floating capacitor groups:** in `contrib/elip_grd`, LTspice's leak acts on one of three identical floating sections and not on the other two; LinSpice applies it to all three, so the first (1 µHz) point differs. Every other frequency matches.
- **Laplace current source current:** LTspice's `I(G1)` can differ from the current the source actually delivers. In `HalfSlope` it violates Kirchhoff's current law. LinSpice reports the delivered current.
- **Extra signals.** LinSpice also saves subcircuit-internal signals (for probing inside the hierarchy) and the currents of ideal op-amp input pins (zero), which LTspice omits. It ignores `.save` and saves everything.
- **Currents that are effectively zero** (picoamps and below) differ by roundoff.
- **Speed.** ngspice is slower than LTspice on some large behavioral circuits and on IGBT DC sweeps. Circuits with several op-amp macromodels built from LTspice's special devices are the worst case: the Colorsound Phase-X (six AD820s, one second of audio) takes about 8 minutes where LTspice takes 47 s.
- **Nodes held only by non-conducting junctions** (a node between two diodes that never conduct, such as an unused clipping-diode position) can sit up to a few tenths of a volt from LTspice's value: with both junctions off the node is hundreds of gigaohms, and ngspice accepts a solution once the current error is below its 1 pA tolerance. Nothing connected to such a node is affected.
- **Some stiff behavioural power circuits don't converge.** A servo amplifier whose IGBTs are behavioural switches (a resistance swinging from 100 MΩ to 25 mΩ through `max()`) stops within its first time steps in ngspice, while LTspice runs it. `gear`, `gmin`, `reltol`, `itl` and `xmu` settings don't help.
- **Missing symbols.** When a schematic uses a symbol that isn't installed, LinSpice stops with an error naming it. LTspice silently leaves the part out of the circuit and simulates the rest (the MXR Dyna Comp download is missing its LM13700 symbol: LTspice "runs" it without the chip).

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
