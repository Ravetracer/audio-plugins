# ThreeOhThree

A monophonic acid bass synthesiser for Linux, as a native **CLAP** plugin.

It models the Roland TB-303's main board from the February 1982 service notes:
the relaxation-oscillator VCO, the four-stage transistor ladder with the unequal
input capacitor that gives it its shallow corner, the envelope-modulation bias
trick Roland's own documentation calls a gimmick, the accent circuit and its
68 ms time constant, and an overdrive stage after all of it because nobody has
ever played one clean.

It plays two ways. From the **host**, where velocity above a threshold makes an
accent and overlapping notes make a slide; or from its **own sixteen-step
sequencer**, locked to the host's transport, with a piano-roll grid and a seeded
pattern generator. Either way a slid note does not retrigger the envelope,
exactly as a slid step on the machine does not, because the gate never goes low.

Forty parameters, twenty-six presets.

Not affiliated with or endorsed by Roland Corporation. *TB-303* is their
trademark and is used here only to name what was modelled.

Vibe coded with Claude, from the 1982 service notes.

## Build and install

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
```

Or by hand:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build            # defaults to ~/.clap/ThreeOhThree
```

Needs a CLAP SDK checkout; the build looks for `../CLAP/clap/include` first, or
point it somewhere else with `-DCLAP_INCLUDE_DIR=`. The window needs X11 and
Cairo, both of which are already on any machine that runs a DAW.

**Linux only, CLAP only.** That is deliberate and it is in the CMakeLists.

## The controls

Eight are the machine's front panel. The rest are not, and every one of them
says so in its own tooltip.

| Control | On the machine | What it does |
|---|---|---|
| **Waveform** | S1 | Sawtooth or square. The square arrives at the filter 6.7 dB down, because on the hardware it does. |
| **Tuning** | VR2 | +/- 700 cents, which is the range the service notes give. |
| **Cutoff** | VR3 | 100 Hz to 2.5 kHz. The centre is 500 Hz because the factory alignment procedure puts it there. |
| **Resonance** | VR4 | Stops just below oscillation, because the machine does. |
| **Env Mod** | VR5 | Deepens the sweep *and* lowers the resting cutoff, together, from one knob. This is the gimmick. |
| **Decay** | VR6 | 200 ms to 2.5 s, the printed range. An accented note ignores it. |
| **Tracking** | -- | Filter key follow. The machine has none; 0 % is the machine. |
| **Accent** | VR7 | Louder, brighter and shorter, all at once. |
| **Accent At** | -- | Which velocities count as an accent. On the machine this was one bit per step. |
| **Sweep Time** | -- | The accent's own time constant. 68 ms is the circuit's; this exposes it. |
| **Slide Time** | -- | Fixed on the machine; a control here because the notes now come from a piano roll. |
| **Drive** | -- | A soft clipper after the filter. Not in the schematic. |
| **Tone** | -- | A lowpass after the drive. Not in the schematic. |
| **Volume** | VR8 | Stops at unity, because the output stage does. |

### The mods

Ten more, on their own row. Every one of them was a hard-coded constant in the
engine, and every one was a number the schematic does not give -- fitted to a
printed waveform, or chosen so that the instrument behaved the way the machine
is described as behaving. Leaving them hidden would have meant asking everyone
to accept one particular guess, and no two 303s agreed anyway: matched pairs, a
posistor and twenty years of drift saw to that.

They are also, not by coincidence, close to the list of things people soldered
into their own machines.

| Control | |
|---|---|
| **Env Bias** | How far Env Mod drops the resting cutoff as it deepens the sweep -- the size of Q9's trick. At zero the gimmick is off and Env Mod is an ordinary envelope-amount knob. |
| **Env Depth** | How deep a full sweep is, in octaves. |
| **Acc Sweep** | How far a full accent opens the filter. |
| **Acc Build** | How much of C62 one accent fills. Below 100 % a run of accents builds instead of repeating. |
| **Acc Gain** | How much louder an accent is. |
| **Acc Decay** | The decay an accented note is forced onto. Set it equal to Decay and accents stop being shorter -- the single most-requested modification there has ever been. |
| **Droop** | The tilt on the square wave. |
| **Ladder** | How hard the ladder's feedback is driven into its own saturation. |
| **Res Range** | How much feedback Resonance can ask for. Past 100 % the filter oscillates, which a stock 303 cannot do. |
| **Drift** | Oscillator instability. Deterministic, so a render is still repeatable. |

## The sequencer

Set **Mode** to Sequencer and the plugin plays itself, locked to the host's beat
timeline -- scrubbing, looping and tempo changes all land where they should, and
notes are placed to the sample rather than to the block. A held MIDI note
**transposes** the pattern instead of sounding, which is what the machine's own
keyboard did; C2 plays it as written.

The grid below the panels is sixteen steps across:

- a **piano roll**, twelve semitones, C at the bottom. Click to place a note,
  click it again to clear it, drag to paint. An accented step is drawn bright.
- an **octave** lane above it: click the top half for +1, the bottom half for
  -1, and again to centre it.
- **slide**, **accent** and **vibrato** lanes below. Click or drag.
- **CLEAR**, two **shift** buttons that walk the pattern sideways under the bar,
  the **seed** with its - and + buttons, and **GEN**.

`Rate`, `Steps`, `Gate` and `Swing` are on the SEQUENCER panel. Steps takes 1 to
16, as the machine did, and the interesting part of that range is the bit that
is not 16.

## The pattern generator

A seed, a scale, a root and five densities. The same settings always give the
same sixteen steps, so a line worth keeping is a number you can write down. Step
the seed with the buttons beside the grid and the pattern regenerates as you go,
which is how it is meant to be used.

Two things make it a generator rather than a random number generator wired to a
piano roll:

- **Every step draws all of its random values whether or not it uses them**, so
  turning Accents up changes which steps are accented and leaves the notes
  exactly where they were. Octaves are the one deliberate exception -- a jump is
  far likelier on an accented note, because accent-plus-octave is *the* gesture.
- **It knows some things about bass lines.** The root comes up far more often
  than any other degree and almost always starts the pattern; the first step of
  each beat is likelier to sound; and a slide into a rest is removed afterwards,
  because it slides into nothing.

Measured over 400 seeds: the root is 44 % of all generated notes, rests are
19 %, octave jumps 21 % with a third of them downward.

## Playing it

- **Accent**: play a note at velocity 100 or above (the threshold is a control).
- **Slide**: overlap two notes. The second slides from the first and nothing
  retriggers. In a piano roll, extend a note so it runs past the start of the
  next one.
- It is **monophonic** and tells the host so. Overlapping notes are a slide, not
  a second voice, and releasing the upper of two held notes slides back down to
  the lower one.

## What is in the box

Twenty-six factory presets. `Factory Reset` is the machine's own middle
position and `Dry Reference` has every added stage switched off, for A/B'ing
against the real thing.

Eight of them drive their own sequencer, including four written for a mood:
**Daylight** (happy -- major pentatonic, short, bright), **Homesick** (sad --
natural minor at half speed, long decay, a delayed vibrato), **Teeth** (beasty
-- 92 % resonance, the ladder driven half again as hard as the circuit drives
it, almost every step accented) and **Basement** (dark -- the square an octave
down, minor seconds and tritones, seven notes in sixteen steps).

## The offline renderer

```sh
./build/threeohthree-render --list
./build/threeohthree-render --preset dark_engine --out acid.wav --seconds 16
./build/threeohthree-render --all --outdir /tmp/acid --bpm 138
./build/threeohthree-render --selftest
```

Unlike the rest of this repository's renderers it plays a **sixteen-step acid
line** rather than one held note, because the two behaviours that make the
instrument what it is -- an accent decaying into the notes after it, and a slide
that does not retrigger -- are inaudible in a single note. `--hold` gives the
held note back.

A preset in Sequencer mode is rendered differently and the renderer works that
out for itself: it supplies a running transport and no notes at all, and rounds
the length up to a whole number of times round the pattern, never fewer than
three, so a demo ends where the loop does instead of halfway through a bar.

`--seed N --seeds 8` prints what the generator makes, as the grid draws it.

The engine has no random state at all, so the same preset at the same sample
rate renders the same samples every time. The self-test checks it.

## Where this sits, and how to build it

This plugin lives in the
[audio-plugins](https://github.com/Ravetracer/audio-plugins) repository, one
folder among several. It shares that repository's foundation, which is why it
looks the way it does: the parameter model, the preset format and the window all
come from `shared/`.

Two directories have to sit beside the plugin folder to build it, and a normal
checkout already arranges that:

```
audio-plugins/
├── shared/           the PluginCore library, in this repository
├── CLAP/clap/        https://github.com/free-audio/clap
└── three-o-three/    this plugin
```

Only `CLAP/` has to be fetched, because it is third-party code and gitignored:

```sh
git clone https://github.com/Ravetracer/audio-plugins.git
cd audio-plugins
mkdir -p CLAP && git -C CLAP clone https://github.com/free-audio/clap.git
cd three-o-three && ./install.sh
```

`shared/` is never modified from this side. A change there has to be verified
against every plugin in the suite, and this one does not get a vote -- which is
also why the window here is a *fork* of the shared one rather than a change to
it. See `TODO.md`.

## Status

See `STATUS.md` for what is measured and `TODO.md` for what is not. The short
version: the circuit is modelled from the schematics and verified against the
service notes' own alignment figures, and **it has never been compared against a
recording of a real 303**. That is the next thing.

## Licence

MIT, the same as the rest of the tree. The service-note PDFs in `!dev/` are
Roland's and are not redistributed.
