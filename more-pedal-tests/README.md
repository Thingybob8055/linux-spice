# More pedal simulations for comparing LinSpice with LTspice

LTspice schematics of guitar pedals from independent GitHub projects, downloaded
unchanged (whole repositories, so their custom models come along). Open the same
`.asc` in LTspice and LinSpice to compare.

| Folder | Source | Licence | What it is |
|---|---|---|---|
| `michael-hirschmugl_pnp_fuzz_simulation` | https://github.com/michael-hirschmugl/pnp_fuzz_simulation | GPL-2.0 | Germanium PNP Fuzz Face (three variants, two render .wav files) |
| `nickmedina5991_mister-fuzz` | https://github.com/nickmedina5991/mister-fuzz | MIT | Silicon NPN Fuzz Face, AC sweep over the fuzz control |
| `PanagiotisPrountzosCS_DistortionPedal` | https://github.com/PanagiotisPrountzosCS/DistortionPedal | GPL-3.0 | Op-amp distortion with germanium diodes, swept gain |
| `thezhe_Ultimate-Overdive` | https://github.com/thezhe/Ultimate-Overdive | MIT | Op-amp overdrive |
| `mbovero_808_EQ-Plus_Guitar_Pedal` | https://github.com/mbovero/808_EQ-Plus_Guitar_Pedal | CC-BY-SA-4.0 / CERN-OHL-S-2.0 (see its `Licenses/`) | Modded Tube Screamer |
| `OscarBazylewich_Phaser_and_Overdrive_Custom_Guitar_Pedal` | https://github.com/OscarBazylewich/Phaser_and_Overdrive_Custom_Guitar_Pedal | GPL-3.0 | Phaser plus overdrive (TL072s, JFETs) |
| `chronometreur_pharaoday-pedals` | https://github.com/chronometreur/pharaoday-pedals | CC-BY-SA-4.0 | LPB-1-style booster |
| `konstantinosKokos_effect-schematics` | https://github.com/konstantinosKokos/effect-schematics | none | Elk Sustainar and Hizumitas (Big Muff family) fuzzes |
| `Aditya-Ramkumar_audio-effects-ltspice` | https://github.com/Aditya-Ramkumar/audio-effects-ltspice | none | TS808 and Crybaby |
| `ujjwaladi_Envelope-Filter-LTSpice-guitarPedal` | https://github.com/ujjwaladi/Envelope-Filter-LTSpice-guitarPedal | none | Envelope filter |
| `JoshFalejczyk_ProCo-RAT-2-LTSpice-Simulation` | https://github.com/JoshFalejczyk/ProCo-RAT-2-LTSpice-Simulation | none | ProCo RAT 2 |

Projects marked "none" have no licence file: use them for local testing only and
don't redistribute or commit them. The licensed ones may be redistributed under
their own terms.

Some of these render 15-20 seconds of audio and take minutes in either simulator.
