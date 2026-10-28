# Patched pedal simulations

Copies of four files from `../ltspice-guitar-pedals` (downloaded from
https://github.com/Cushychicken/ltspice-guitar-pedals) that don't run in LTspice
either as published, because the author used parts that only exist in their own
library. Each copy adds the missing piece as a directive at the top of the
schematic, so LTspice and LinSpice simulate the same circuit:

| File | Problem in the original | Added |
|---|---|---|
| `ts808_tube_screamer.asc` | `2SC4081` transistor model undefined | generic NPN stand-in |
| `dunlop-crybaby-wah.asc` | `TFZ9_1B` zener model undefined | 9.1 V zener stand-in |
| `schaller-tremolo.asc` | `TFZ9_1B` zener model undefined | 9.1 V zener stand-in |
| `lovetone-big-cheese.asc` | parameter `Rvolume` never defined | `Rvolume=5k` (mid-travel) |

The stand-in models are generic, not the real parts, so absolute results differ
from a real pedal; they are only for comparing simulators.

The original repository has no licence file: keep these for local testing and
don't redistribute them.
