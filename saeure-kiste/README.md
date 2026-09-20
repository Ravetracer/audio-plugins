# SäureKiste

A monophonic acid bass synthesiser for Linux, as a native **CLAP** plugin.

It models the Roland TB-303's main board from the February 1982 service notes:
the relaxation-oscillator VCO, the four-stage transistor ladder with the unequal
input capacitor that gives it its shallow corner, the envelope-modulation bias
trick Roland's own documentation calls a gimmick, the accent circuit and its
68 ms time constant, and an overdrive stage after all of it because nobody has
ever played one clean.

It plays two ways. From the **host**, where velocity above a threshold makes an
accent and overlapping notes make a slide; or from its **own sequencer**, locked
to the host's transport, 1 to 128 steps long, with a piano-roll grid, a bank of
sixty-four patterns and a seeded pattern generator. Either way a slid note does
not retrigger the envelope, exactly as a slid step on the machine does not,
because the gate never goes low.

It also models **Robin Whittle's Devil Fish** modification of the same machine,
from his own manual: the overdrive into the filter, the audio-rate filter FM,
the Muffler, the soft attack, the volume envelope as a control, the three accent
sweep speeds, and the widened ranges. Every one of them defaults to the stock
circuit, so the plugin is a TB-303 until you ask it not to be.

Sixty-three parameters, twenty-seven presets.

Not affiliated with or endorsed by Roland Corporation. *TB-303* is their
trademark and is used here only to name what was modelled. Not affiliated with
or endorsed by Robin Whittle; *Devil Fish* is his and is used only to name the
modification that was modelled.

Vibe coded with Claude, from the 1982 service notes.

## Build and install

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
```

Or by hand:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build            # defaults to ~/.clap/SaeureKiste
```

Needs a CLAP SDK checkout; the build looks for `../CLAP/clap/include` first, or
point it somewhere else with `-DCLAP_INCLUDE_DIR=`. The window needs X11 and
Cairo, both of which are already on any machine that runs a DAW.

### All four binaries

`./release.sh 0.3.0` at the repository root builds CLAP and VST3 for both Linux
and Windows and packs them with the manual into `dist/`:

```
SaeureKiste-0.3.0/
├── linux/    SaeureKiste/SaeureKiste.clap + presets,  SaeureKiste.vst3
├── windows/  SaeureKiste/SaeureKiste.clap + presets,  SaeureKiste.vst3
├── SaeureKiste-0.3.0-Manual.pdf
├── README.md  LICENSE  INSTALL.txt  BUILD-INFO.txt
```

Windows is cross-compiled with mingw-w64 against a Cairo built once by
`./setup-winbuild.sh`; both Windows binaries link the runtime in statically and
import nothing but system DLLs, so nothing has to be copied beside them. The
VST3 is the same plugin behind free-audio's clap-wrapper and needs
`CLAP/clap-wrapper` and `CLAP/vst3sdk` checked out — see the repository README.

**The Windows binaries have not been run in a DAW.** They build, they link
cleanly and they import only what Windows already has. That is not the same as
working, and the window is the part most likely to surprise.

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
| **Drive** | -- | How hard the signal is pushed into the stage after the filter. Not in the schematic. |
| **Tone** | -- | A lowpass after the drive. Not in the schematic. |
| **Type** | -- | Which model that stage is: seven of them, each an equation out of the literature. See below. |
| **Bias** | -- | Where the model sits on its own curve. The centre is the operating point its source gives it. |
| **Dist Mix** | -- | How much of the driven signal is heard against the clean one. At zero the stage is bypassed. |
| **Volume** | VR8 | Stops at unity, because the output stage does. |

### The drive stage's seven models

Everything in front of this stage is fixed by the service notes. There is no
schematic for the stage itself, so it is built the other way round -- from the
literature. Every model is an equation out of a named source, and the source is
in the code beside it:

| Type | Source | What it is |
|---|---|---|
| **Soft Clip** | -- | The stage this plugin has always had: a rational tanh, and the default. |
| **Overdrive** | Zölzer, *DAFX* 2/e, eq 4.14 (Schetzen) | Linear below a third of full scale, compressing to two thirds, flat above. It stops distorting as a note decays, which none of the others do. |
| **Tube** | Zölzer, *DAFX* 2/e, eq 4.13 (Bendiksen) | Asymmetric around a work point: roughly linear one way, hard-limited the other. Almost all of its harmonics are even. Carries the M-file's own highpass and lowpass. |
| **Valve Stack** | Pirkle, *Designing Audio Effect Plugins in C++* 2/e, 19.12/19.13 | Four class-A triode stages in series, each with a DC blocker and a cathode-bypass shelf, and the book's tone stack between the third and the fourth. The one model with a frequency response of its own. |
| **Fuzz** | *DAFX* eq 4.15 + Pirkle's FEXP1 | Exponential from the first volt, no linear region, asymmetric by default -- which is how both books describe a Fuzz Face. |
| **Rectifier** | *DAFX* 4.3.3 | Folds the negative half onto the positive one and doubles the fundamental: an octave over the note rather than an edge on it. |
| **Crush** | Pirkle, eq 19.1 | The quantiser, twelve bits down to three. |

Both books insist that a nonlinearity needs oversampling, so every model runs at
twice the sample rate. Soft Clip is the exception, deliberately: it predates all
of this and renders what it always did.

The models are matched to each other in **loudness**, measured on a real line
rather than on a sine, so switching between them compares character and not
level. Soft Clip is the loud one and always was -- its gain reaches 24 at the
top of the knob.

**Why seven and not fourteen.** There were fourteen, and they all sounded the
same: any two memoryless clippers driven hard enough become the same square
wave, level matching removes what is left, and none of them had filtering of its
own. What tells these seven apart is that they are built differently. The
self-test measures it now instead of assuming it -- each model's drive is
searched for the setting that gives 25 % THD and their harmonic distributions
are compared there, so two models that measure the same at the same distortion
fail the build.

**Soft Clip is the default**, so every preset written before the models existed
sounds exactly as it did. That is checked rather than asserted: the whole
factory library renders byte for byte what it rendered before, at 44.1 and
48 kHz.

### The delay

The second stage the machine does not have, built the same way the drive stage
was -- out of the literature, with the source beside every part of it in
`src/dsp/delay.h`. It sits after everything else and it is **off by default**,
so nothing that already sounded a particular way has moved.

| Control | |
|---|---|
| **Enable** | Whether the stage is in the path. Off keeps what is in the lines, so switching back on carries on rather than starting from silence. |
| **Sync** | Free takes the time from the Time knob; Synced takes it from the host's tempo and the Division chip. |
| **Time** | 20 ms to two seconds, when Sync is Free. Turning it bends the repeats: the read head glides to a new setting over about 50 ms, the way a tape delay does. |
| **Division** | The time as a note value, a thirty-second to a half note, when Sync is Synced. A dotted eighth is the default. |
| **Feedback** | 0 to **130 %**. See below. |
| **Mix** | The repeats against the dry instrument, as a crossfade. At zero the stage is bypassed. |
| **Routing** | Mono, Stereo, or Ping-Pong. |
| **Width** | A mid/side matrix over the repeats and nothing else -- the dry instrument does not move. Nothing to do in Mono. |

| Part | Source |
|---|---|
| The line, and its feedback | Zölzer, *DAFX* 2/e, eq 2.61 and 2.62 -- the IIR comb filter |
| Reading it at a fractional delay | *DAFX* 2.5.4 |
| Stereo and Ping-Pong | Pirkle, *Designing Audio Effect Plugins in C++* 2/e, figures 14.12 and 14.13 |
| Width | M. Gruhn, musicdsp.org entry 256, public domain |

**Feedback above 100 %, and why it does not blow up.** Up to unity the stage is
the comb filter its source prints and the repeats die away. Past it they do not:
*DAFX* states the stability condition outright -- above unity "the signal would
grow endlessly" -- and this breaks it on purpose. The loop is closed through the
same soft clipper the instrument's own output stage uses, so instead of
overflowing, the repeats grow, saturate, and sit there as a self-oscillating
drone to play over. Thirty seconds at the top of the knob peaks at 1.000 and
stays finite, and the self-test asserts it. It will not stop on its own: Enable
off, or Mix at zero, is how it is stopped.

**Ping-Pong does not follow its figure exactly**, and that is deliberate.
Pirkle's diagram crosses the inputs as well as the feedback, which is right for
a stereo source and does nothing at all for a monophonic one -- both lines would
be fed the same signal and both taps would stay equal for ever. Here the
instrument goes into the left line only and the feedback crosses, so the repeats
alternate. The self-test checks that the first repeat is on one side and the
second is on the other.

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

The grid below the panels is sixteen steps across -- or thirty-two, at half the
cell width, for a pattern longer than sixteen, with the rest scrolled to:

- a **piano roll**, twelve semitones, C at the bottom. Click to place a note,
  click it again to clear it, drag to paint. An accented step is drawn bright.
- an **octave** lane above it, reaching **two octaves either way**: click the top
  half to step up and the bottom half to step down, one octave a click; the
  right button centres a step. Up is drawn in the accent green and down in
  amber, at half the box for one octave and most of it for two.
- **slide**, **accent** and **vibrato** lanes below. Click or drag.
- **CLEAR**, two **shift** buttons that walk the pattern sideways under the bar,
  the **seed** with its - and + buttons, **GEN**, and **MIDI**, which drags the
  pattern into the host. The seed runs **0 to 4,294,967,295** -- wide enough to
  take a Unix timestamp, so seeding from the clock gives a line you have never
  heard. Click the reading to type one.
- a **scrollbar** under the lanes, for a pattern longer than the grid draws. The
  wheel works anywhere over the grid, and while the sequencer runs the grid
  follows the playing step.

`Rate`, `Steps`, `Gate` and `Swing` are on the SEQUENCER panel. **Steps takes 1
to 128.** Below sixteen is the machine's own trick -- fifteen steps against a
four-four bar walks the pattern around the beat -- and above it the sequencer
stops being a figure that repeats every bar: 64 steps is four bars of
sixteenths, 128 is eight, which is long enough to hold a melody. The generator
fills exactly the length that is set, and because its draws are per step and in
order, turning Steps up *extends* the line the seed already described instead of
replacing it.

### Live mode: patterns from a pad

Set **Mode** to **Live** and the keyboard stops transposing the pattern and
starts selecting them. Everything else about the sequencer is unchanged.

That one swap is the point. Transposing from the keys is the machine's own
behaviour and it is the wrong thing entirely in front of a pad controller,
where what the pads should do is change pattern. In Live mode:

- a key the map does not know about **runs the pattern**, at the pitch it was
  written at;
- a mapped key **selects its pattern**, or steps the bank one either way;
- stepping **stops at the ends** rather than wrapping -- nothing happening at
  the end of the bank beats landing on pattern 1 halfway through a bar;
- **Pattern Oct**, the `OCT` reading with its `-` and `+` above the grid, moves
  the whole running pattern by octaves, since the keyboard no longer does. It
  works in the other two modes too, where it adds to the held-key transpose,
  and it is an ordinary automatable parameter. Click the reading to zero it.

**To build the map**, press `MAP`. The bank becomes the pad layout; click a
pattern, or the `PREV` or `NEXT` button, then play the note you want to reach
it. The cell shows the note from then on. Right-click a cell to unbind it,
right-click `MAP` to clear the whole map. One note does one thing: learning a
note onto a new target takes it off whatever it did before.

**The map is saved with the project, not with the preset** -- a pad layout
belongs to the rig, and browsing presets mid-set must not remap your
controller.

### The pattern bank

Beside the grid is a bank of **sixty-four patterns**, eight by eight. Click one
to edit it; the pattern sounding is ringed, the ones with something written in
them are filled, and the ones the chain will reach are lit. The wheel steps
through the bank.

**COPY** and **PASTE** in the bank's title row turn a pattern into a variation of
another one: copy, click an empty slot, paste, change the two steps you meant to
change. **DEL**, beside them, empties the selected slot -- the same edit CLEAR
makes on the grid, put where the patterns are. None of the three is undoable,
which is the deal a hardware sequencer offers.

Under the bank are the two controls that say what happens when a pattern has
played through:

| Chain | After each time round |
|---|---|
| **Stay** | repeat the selected pattern. The hardware's behaviour, and the default. |
| **Next** | step to the following pattern, wrapping round at **Length**. Four patterns make a sixty-four step line. |
| **First** | play the selected pattern once, then stay on pattern 1. |
| **Random** | pick one from inside the chain each time round. |

**Length** is how many patterns the chain covers, counting from pattern 1. Stay
and First ignore it.

All three -- the selected pattern, the chain mode and its length -- are ordinary
parameters, so a host can automate a pattern change like any other knob, and all
of it is saved in a preset. Which pattern plays is worked out from the host's
beat position rather than counted up as the sequencer goes, so a loop, a seek or
a scrub lands on exactly the pattern it should, Random included.

The whole bank travels in the preset file and in the plugin's state. Only the
patterns with something in them are written out.

### Getting the notes out

The sequencer's line can leave the plugin as MIDI, two ways:

- **The note output port.** In Sequencer mode everything the sequencer plays goes
  out of it, transposition included. Route it to another track and record the
  line as it plays.
- **Drag the MIDI button** above the grid into the host's arranger. The selected
  pattern is written to a temporary `.mid` and dropped where you let go.

Both carry an accent as a velocity above the `Acc Thresh` parameter, a slide as
one note still held when the next starts, and a vibrato as CC1 around the note --
the same three conventions the plugin reads back in MIDI mode, so a recording
played into it again sounds like what it came from.

### The collapsible half of the window

The ten mods and the ACCENT, SLIDE and VIBRATO panels sit behind the
**ADVANCED** button on the preset bar. They are the controls for tuning the
engine rather than for playing it, so the window opens without them and grows
when you ask for them. The window remembers which way it was left, per instance,
in the plugin's state.

## The Devil Fish controls

Robin Whittle has modified TB-303s as the **Devil Fish** since the early
nineties, and documents what each addition does -- in numbers -- on his own
site. That is the second source this plugin has, and it is a good one: none of
what follows is guessed.

**Every one of them defaults to the stock machine.** Whittle makes the same
point himself -- a Devil Fish can still sound exactly like a 303, and his manual
has a table of where to leave each control to keep it there. Those are the
defaults here, and every factory preset renders byte for byte what it did before
these existed.

| Control | Panel | |
|---|---|---|
| **Overdrive** | VCF | The oscillator's level into the filter, which is not `Drive` -- this one is in front of the ladder and `Drive` is behind it. 0 dB is the fixed level the machine has. Up from there the ladder's input pair stops being linear and starts switching; the top is Whittle's 66.6 times normal. At the bottom the oscillator is gone altogether, which with `Res Range` past 100 % leaves the filter singing on its own for you to reintroduce the oscillator into. |
| **Filter FM** | VCF | The amplifier's own output fed back into the filter frequency, at audio rate. Loudest where the signal is loudest, so it bites on accents and wherever Overdrive is up, and it needs resonance to have anything to work with. A little is edge; a lot is what Whittle calls a spluttering chaotic mess. |
| **Muffler** | DRIVE | A clipper on the output, Off / Soft / Hard. It only touches what is already loud and it leaves the bottom of the spectrum alone, so it takes the top off the peaks and adds a buzz rather than making the note smaller. |
| **Soft Attack** | AMP | How fast the amplifier opens on an unaccented note, 0.3 to 30 ms. The machine's is fixed by C41 and R134 at 2.2 ms, which is the default; an accented note always uses it. Turned up, the note swells instead of starting. |
| **Amp Decay** | AMP | How long the amplifier takes to fall away under a held note. Not a control on the machine at all: R123 and C42 fix it at 1.5 s, which reaches a tenth in about 3.45 s. |
| **Amp Sustain** | AMP | Where Amp Decay falls to instead of silence, so a held note can run indefinitely. |
| **Sweep Speed** | ACCENT | How the accent circuit answers accents in quick succession. **Normal** is the machine -- charge left in C62 makes the next accent bigger, which is the machine getting worked up. **Fast** is the opposite: the first accent is the strongest. **Slow** rises further and takes longer to cool. |
| **Accent Hold** | ACCENT | Accents every note whatever its step or velocity says. Whittle's front panel has a pushbutton for it. |

Four ranges were widened to his as well, and none of them moved its default:
`Cutoff` now runs 30 Hz to 5 kHz, `Decay` 30 ms to 3 s, `Slide Time` up to
360 ms, `Res Range` to 200 % so the filter really self-oscillates, and
`Tracking` past 100 % into over-tracking.

**What is not modelled** is the part of the modification that is jacks: the
external audio input into the filter, the audio Filter FM input, the Filter Out
tap, the CV and gate sockets, and the MIDI retrofit. Those want an audio input
port and a second output, which is a different shape of plugin. The MIDI ones a
plugin has for free.

## The pattern generator

A seed, a scale, a root and five densities. The same settings always give the
same sixteen steps, so a line worth keeping is a number you can write down. Step
the seed with the buttons beside the grid and the pattern regenerates as you go,
which is how it is meant to be used. **GEN** writes into whichever pattern the
bank has selected, so filling a chain is: pick a slot, step the seed until it is
the one, pick the next slot.

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

Twenty-seven factory presets. `Factory Reset` is the machine's own middle
position and `Dry Reference` has every added stage switched off, for A/B'ing
against the real thing.

### Folders and packs

The preset browser groups the library by folder: *Factory Presets* first, then
whatever you have made, then *Unfiled*. A folder is made by saving as
`Folder/Name` -- there is no second dialog, and a folder cannot be made empty.

A **preset pack** is a whole folder as one file, `<name>.saeurekistepack`, which
is the preset format again with a separator line between the presets, so it can
be read and edited by hand like everything else here. **EXPORT** writes the
selected folder to `<config>/SaeureKiste/packs`, **EXPORT AS...** puts it
wherever the desktop's file chooser can reach, and **IMPORT...** lists the packs
it can see plus *Other file...* for one from anywhere else. An import never
overwrites: the pack becomes a new folder named after itself.

There are two ways back to the start, and the difference between them is the
sequencer. **Factory Reset** puts every parameter back to the value the table
gives it and leaves whatever you had written in the pattern bank alone.
**Blank Slate** does the same and then clears the bank as well: one bar of C in
pattern 1 -- sixteen notes on the root, no octaves, no slides, no accents, no
vibrato -- and nothing at all in the other sixty-three. That is the one to load
when you want to write a line rather than edit one.

Eight of them drive their own sequencer, including four written for a mood:
**Daylight** (happy -- major pentatonic, short, bright), **Homesick** (sad --
natural minor at half speed, long decay, a delayed vibrato), **Teeth** (beasty
-- 92 % resonance, the ladder driven half again as hard as the circuit drives
it, almost every step accented) and **Basement** (dark -- the square an octave
down, minor seconds and tritones, seven notes in sixteen steps).

## The offline renderer

```sh
./build/saeurekiste-render --list
./build/saeurekiste-render --preset dark_engine --out acid.wav --seconds 16
./build/saeurekiste-render --all --outdir /tmp/acid --bpm 138
./build/saeurekiste-render --selftest
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

### Moving knobs while it renders

A demo that holds every control still for sixteen seconds is a photograph of a
preset. `--demo-moves` makes it a recording of somebody playing one:

```sh
./build/saeurekiste-render --all --demo-moves --outdir demos \
   --seconds 16 --tail 3 --rate 48000
```

Three controls move, all three **relative to whatever the preset sets**, so a
demo shows the instrument's range without losing what makes it that preset:

| | Over the take |
|---|---|
| **Cutoff** | down about an octave and a half, up to an octave above where it started, then settling a little under it |
| **Resonance** | a quarter of its travel, so the sweep gets more vocal as it goes |
| **Drive** | a third of its travel, starting halfway in, so the end leans into the clipper |

Resonance and Drive **reflect rather than clamp**: a preset already near the top
travels the same distance downward instead, because a knob sitting against the
ceiling for sixteen seconds is the thing this exists to avoid. The moves run
over the held seconds and not the tail, so the last note decays wherever the
sweep left the filter.

For anything else, `--move` writes one out by hand, in real units, and can be
given more than once:

```sh
--move "Cutoff=200 Hz..2 kHz"          # a straight ramp over the take
--move "Cutoff=800..2400..600"         # there and back
--move "Drive=20%..90%@8:16"           # only the second half
```

Interpolation happens in the parameter's own domain, so a Log control like
Cutoff sweeps evenly **in octaves** rather than crawling through its top one.

Without either flag nothing moves and a render is byte-for-byte what it always
was.

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
└── saeure-kiste/    this plugin
```

Only `CLAP/` has to be fetched, because it is third-party code and gitignored:

```sh
git clone https://github.com/Ravetracer/audio-plugins.git
cd audio-plugins
mkdir -p CLAP && git -C CLAP clone https://github.com/free-audio/clap.git
cd saeure-kiste && ./install.sh
```

The window here is a *fork* of the shared one rather than a change to it: a step
grid is not expressible as panels of knobs, and a change to the shared window is
a change every plugin inherits. See `TODO.md`.

`shared/` itself is touched only for things that are plugin-agnostic by
construction. 0.5.0 added two: the preset **pack** format, which takes a
`PresetContext` like the rest of the preset code and knows nothing about this
plugin, and a `folder` field on the browser's preset entries with the folder and
pack calls on the delegate, all of which default to "this plugin does not do
that" -- so a plugin that keeps a flat preset directory gets the browser it
always had.

## Status

See `STATUS.md` for what is measured and `TODO.md` for what is not. The short
version: the circuit is modelled from the schematics and verified against the
service notes' own alignment figures, and **it has never been compared against a
recording of a real 303**. That is the next thing.

## Licence

MIT, the same as the rest of the tree. The service-note PDFs in `!dev/` are
Roland's and are not redistributed.
