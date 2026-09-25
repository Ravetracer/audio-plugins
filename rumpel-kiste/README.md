# RumpelKiste

A rhythm composer for Linux and Windows, as a native **CLAP** plugin and a
VST3.

It models the Roland TR-909's voicing board from the June 1984 service notes.
The seven analog voices are built as circuits, with every time constant, filter
corner and oscillator ratio computed from the component values:

- the bass drum's reset triangle core and its pitch envelope, where Tune sets
  the sweep time and not the pitch
- the snare's two oscillators and its split noise path
- three oscillators per tom
- the rim shot's three bridged-T resonators and its clamp diodes
- the hand clap's four bursts and its tail

The hi-hats, crash and ride were six-bit ROM samples on the machine. Here they
run through the same six-bit converter, ROM clock and decay circuitry, with a
synthesised stand-in for the recordings, which are not reproduced.

Where the service notes leave something out -- how far and how fast each
drum's pitch bends, how its envelopes hold and fall, how the toms' oscillators
are balanced, where the clap's bursts land, how fast the cymbal ROMs are read,
the stand-ins' spectra, the accent response and the balance between the voices
-- the numbers are measured from 96 kHz recordings of a machine and the voices
are fitted to them. Nothing is sampled: the recordings are a reference, not a
source. `tools/analysis/README.md` lists every number and where it came from.

It plays from the host on the machine's own MIDI key numbers, or from its own
**step sequencer**: twelve rows (total accent and eleven voices), 1 to 64 steps,
local and total accent, flams, the machine's shuffle and scale settings, a bank
of sixty-four patterns with per-pattern chains, a MIDI pattern map for pads, a
seeded groove generator, and a MIDI drag-out. The sequencer and bank come from
SäureKiste.

The one stage the machine does not have is a **drive bus**: SäureKiste's ten
distortion models, taking the whole mix or only the voices routed to it.

Seventy-six parameters, nineteen presets, three of them sound-only kits.

Not affiliated with or endorsed by Roland Corporation. *TR-909* is their
trademark and is used here only to name what was modelled.

Vibe coded with Claude, from the 1984 service notes.

## Build and install

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
```

Or by hand:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build            # defaults to ~/.clap/RumpelKiste
```

Needs a CLAP SDK checkout at `../CLAP/clap` (or `-DCLAP_INCLUDE_DIR=`), and
X11 and Cairo for the window. The Windows and VST3 builds need what the
repository root's `CLAUDE.md` lists; `./release.sh` at the root builds all four
binaries.

## Tools

```sh
build/rumpelkiste-render --selftest              # the suite, 182 checks
build/rumpelkiste-render --list                  # the preset library
build/rumpelkiste-render --all --outdir /tmp/rk  # every preset to WAV
build/rumpelkiste-render --preset Warehouse --bpm 130 --seconds 16 --out w.wav
build/rumpelkiste-render --voice bd --accent 1 --out kick.wav   # one hit, bare engine
build/rumpelkiste-render --defaults              # a preset with every default
build/rumpelkiste-guihost build/RumpelKiste.clap presets/breaks.rumpelkiste 10
```

`--param key=value` sets a parameter for a render, in the units its display
shows (percentages 0 to 100).

## Layout

```
rumpel-kiste/
├── src/
│   ├── dsp/drums.{h,cpp}     the eleven voices, the noise source, the ROM voices
│   ├── dsp/drive.{h,cpp}     SäureKiste's drive stage, copied unchanged
│   ├── pattern.{h,cpp}       the bank, its text format, the generator
│   ├── midifile.{h,cpp}      the key map and the MIDI drag-out
│   ├── plugin.cpp            CLAP glue, sequencer clock, state
│   └── gui/                  panels and theme (gui.cpp), the forked window
├── tools/render.cpp          offline renderer and self-test
├── tools/guihost.cpp         standalone window host
├── tools/analysis/README.md  every number and where it came from
├── presets/                  the factory library
└── docs/manual.md            the manual
```
