# Shapeau

A multiband drawing rack for Linux and Windows, as a native **CLAP** plugin and
a VST3.

A chain of effects -- **volume**, **filter**, **drive**, **crush**, **time**,
**width**, **pan** -- each driven by a shape you draw rather than a knob you
sweep, in whatever order you put them in. The input is split into three bands,
and every effect has its own shape in each band, locked to the host's beat by
its own note-value rate and phase. The editor draws the audio behind the shape
as it plays -- input, output, the playhead and the modulation the effect really
applied -- so a duck is drawn onto the kick it ducks for.

What that buys:

- **Ducking, with or without a sidechain.** Draw the dip on the low band and
  leave the mid and high bands flat: the bass gets out of the kick's way and
  the top of the mix does not pump with it. Route the kick into the sidechain
  input and set the effect's Trigger to Audio, and the duck starts on every
  kick, wherever it falls -- with the kick drawn in amber behind the shape, so
  the dip can be fitted to it by eye.
- **A filter sweep as a line.** The shape is a cutoff position — the top of the
  editor is the Cutoff knob, the bottom is that value pushed down by Depth
  octaves — so one drawing is a two-octave wobble at 400 Hz or an eight-octave
  slam at 8 kHz.
- **Distortion only where you want it.** The drive shape scales the Drive
  knob, so a hard step in the curve is a burst of grit on the hits and silence
  of grit between them.
- **A stutter that holds a moment still.** The time shape is the read head's
  position in a look-back buffer: fall at the rate the buffer fills and one
  moment repeats; fall faster and it plays backwards.
- **Grit in rhythm.** The crush shape scales a bit-depth and sample-rate
  reducer, so a stepped pattern of grit lands with the drums.
- **A stereo image that moves.** Width takes a band from mono to double width;
  pan walks it across the field. Both are drawn, both are per band.

Any effect can also be restarted by a MIDI note or a hit -- once, holding at an
End marker until the next one, or looping -- instead of following the beat.

Three Linkwitz-Riley fourth-order bands, a chain of up to seven effects per
band, one mixed output, a sidechain input. No latency.

Eighty-three parameters, ten presets. Shapes are stored in the preset as text, so
a drawing survives a save, a reload and a different machine.

## The window

The editor fills the top: the **effect bar** on the left (the chain, in order --
a dot bypasses an effect, × takes it out, drag moves it, + adds one), the band
tabs and the editing aids on the right, and the shape over the audio it shapes.
Under it are GLOBAL and TRIGGER (the audio and MIDI trigger setup), and the
selected effect's controls. An empty
chain shows the seven effects in the editor, one click from being added.

## The shape editor

- **Click** adds a point, **drag** moves it, **double-click** deletes it,
  **right-click** cycles its corner weight (hard, medium, soft).
- **Drag the line** between two points to bend the segment up or down;
  **right-click** the line to straighten it again.
- **STEP** paints a sixteen-cell step shape; **SNAP** rounds points to a
  sixteenth of a cycle.
- **SHAPE** opens twelve generators: flat, duck, gate, ramp up, ramp down,
  triangle, square, pulse, sine, stairs, random and an exponential rise.
- **ALL** edits all three bands at once, which is the default. Pick LOW, MID or
  HIGH to draw one band on its own; all three are drawn at once and the
  selected one is bright.
- Drawing a shape into a stage that is switched off switches it on, and the
  bands being edited with it. A dot on a tab marks a stage or band that is off.

## Build and install

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
```

Or by hand:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build            # defaults to ~/.clap/Shapeau
```

Needs a CLAP SDK checkout at `../CLAP/clap` (or `-DCLAP_INCLUDE_DIR=`), and for
the window `cairo` and `x11` development headers — without them it builds and
runs headless rather than failing. `-DSHAPEAU_BUILD_VST3=ON` adds the VST3
through `CLAP/clap-wrapper`.

The self-test is part of `install.sh` and can be run alone:

```sh
build/shapeau-render --plugin build/Shapeau.clap --selftest
```

It parses every factory preset, checks the band split's flatness, each stage's
behaviour and the curve text round trip, then loads the built `.clap` through
the CLAP API and plays a preset through it.

## Manual

`shared/tools/make-manual.sh shapeau` builds `dist/manuals/Shapeau-<version>-Manual.pdf`
with the HTML it was rendered from. The parameter reference and the preset
library in it are generated from the build, not written by hand.

## Layout

```
shapeau/
├── CMakeLists.txt
├── install.sh              wrapper over ../shared/tools/install-plugin.sh
├── docs/manual.md          the manual; images/ its screenshots
├── presets/                factory presets, one .shapeau file each
├── src/
│   ├── shapeau.h           identity, the preset bindings
│   ├── params.{h,cpp}      the 56-parameter table
│   ├── plugin.cpp          CLAP glue, the audio callback, curve hand-off
│   ├── dsp/curve.{h,cpp}   the shape: points, weights, evaluation, text
│   ├── dsp/engine.{h,cpp}  the band split and the seven stages
│   └── gui/                the window fork, the shape editor, its theme
└── tools/                  render + self-test, gui host, screenshots
```

`src/gui/window.cpp` is a fork of the shared PluginCore window: a step grid of
knobs cannot host a drawable curve, and the rest of the collection should not
have to change because this plugin has one. Everything else is the shared
library — the parameter model, the preset format, the preset discovery provider
and the drawing toolkit.
