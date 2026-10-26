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

## Accuracy

Simulation results are checked against LTspice by running the same examples in both and comparing waveforms (`tools/compare_raw.py`). Of the 80 runnable examples in LTspice's `Educational` folder, 58 match within 1% normalized RMS error and 65 within 5%. Most of the rest are free-running oscillators, where tiny numerical differences grow into phase drift.

Known gaps:

- The hysteretic core inductor model (`Hc=`, `Br=`, `Bs=`) is treated as a linear inductor.
- `.machine` state machines, `.savestate`/`.loadstate` and `.savebias` are not supported.
- IGBT switching transients are approximate; static characteristics match closely.
- ngspice is slower than LTspice on some large behavioral circuits.

## Development

```sh
build/core_tests                                           # unit tests
QT_QPA_PLATFORM=offscreen build/gui_tests                  # GUI interaction tests
build/core_tests --corpus <examples-dir> --translate       # parse, netlist and translate every example
build/core_tests --netlist file.asc                        # show the LTspice and ngspice netlists
python3 tools/compare_raw.py reference.raw result.raw      # compare two waveform files
```

Source layout:

- `src/core` — file formats, netlisting, the LTspice→ngspice translator, expressions, `.meas`, `.step`, FFT, FRA and two-port analysis
- `src/sim` — the simulation worker (ngspice in a child process) and controller
- `src/gui` — schematic editor, symbol editor, waveform viewer and dialogs
- `tests`, `tools`, `packaging`
