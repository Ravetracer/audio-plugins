# Substrike

A kick drum designer for Linux and Windows as a native CLAP instrument (plus
VST3 through clap-wrapper). Everything is synthesised; nothing is sampled.

The goal is one instrument for every kind of kick, from ambient through hip
hop, techno and trance to hardstyle and hardcore: up to eight layers, each with
its own effect chain in free order, layers feeding one another for rumble, a
master chain, and modulation. The design and the phases are in
[docs/PLAN.md](docs/PLAN.md).

## Status: 0.4.0, phase 4

What exists:

- **Eight lanes**, each playing one source, with level, pan, velocity
  sensitivity, a trigger delay of up to 100 ms (sample-exact), polarity
  invert, a note filter (any note, or one note, so Substrike can be played as
  a kit), transpose (-24 to +36 semitones) and Variation, which randomises
  pitch, level, decay and noise from hit to hit. At 0 % every hit repeats
  bit for bit.
- **Body**: sine, triangle, saw, square (band-limited) or 8-partial additive
  (tilt, even partials, stretch), with a pitch drop from Pitch Start to Pitch
  End along a curve, attack, hold and a curved decay, start phase, key
  tracking against a root note (MIDI 36, shown as C2 with C4 = 60), tanh
  shaping, FM from its own modulator (amount, ratio, decay), phase feedback
  and drift.
- **Click**: impulse, noise burst, blip or zap (a sweep through up to 8
  octaves), with a decay and its own filter, peak-normalised.
- **Noise**: white, pink, brown or crackle (with a density), stereo width,
  a filter with a decaying cutoff envelope, and attack, hold and a curved
  decay.
- **Resonator**: 2-8 damped modes (membrane, harmonic, odd or bar) struck
  by an impulse, a mallet or a noise burst, with tune, key tracking, decay,
  damping of the higher modes, brightness, hardness and a pitch drop.
- **Pitch Link**: a lane can follow another lane's Body pitch -- the whole
  drop for a Body, the end pitch for a Resonator or a Click blip -- so layers
  stay in tune.
- **Outputs**: a stereo main output plus one stereo output per lane. Each
  lane goes to Main, its own output, or both. The master level applies to
  the main output only.
- One-shot behaviour: note-off does nothing; a new hit fades the old one out
  over 3 ms, so retriggering does not click. Note choke is honoured.
- Plain-text state: parameters are saved in their own units (Hz, ms, dB), so
  a state stays valid when a range is widened later. A 0.1.0 state loads.
- **Effect chains**: six slots per lane and six on the master. See below.
- **Master**: after its slots, Mono Below (an LR4 split under which the
  output is mono), an Output Clip (Off, Soft, Hard) and the output level.
- **The editor** (see below), with the Body's pitch and amplitude curves as
  breakpoint curves, a preview of the hit, and the hit export.

## Effect slots

Every slot has a Type, a Band, Mix, Bypass and six controls A-F, whose
meaning, name, range and default come from the type. Picking a type resets
A-F to its defaults, and the host is told their new names. Type is not
automatable; everything else is, and values glide over 5 ms.

| Type | A-F |
|---|---|
| Distortion | Model (the ten drive models from SaeureKiste), Drive, Bias, Tone, Output |
| Clipper | Drive, Knee (hard to soft), Ceiling |
| Wavefolder | Drive, Bias, Shape (sine to triangle fold), Output |
| Bitcrush | Bits (1-16), Rate (sample and hold; off at the top), Output |
| Filter | Mode (LP 12/24, HP 12/24, Band Pass, Notch, Peak), Cutoff, Reso, Env (octaves from each hit), Env Decay, Gain |
| EQ | Low shelf, Mid, Mid Freq, Mid Q, High shelf, Tilt |
| Compressor | Threshold, Ratio, Attack, Release, Knee, Makeup |
| Transient | Attack, Sustain, Speed, Output |
| Gate | Mode (Gate opens on level, Hit runs from every hit), Threshold, Attack, Hold, Release, Range |

**Band** processes Full, Low, Mid, High, Low+Mid or Mid+High of the lane,
split at the lane's Crossover Low and High (LR4); the rest passes by, and the
sum is flat. **Quality** (1x, 2x, 4x) oversamples Distortion, Clipper,
Wavefolder, Bitcrush and the Output Clip. A chain that rings on after its hit
keeps the lane running until it has been quiet for 50 ms, then sleeps. The
state stores each slot's controls under their names and units, e.g.
`l1.slot1.drive=73`.

## The editor

The eight lanes and the master sit in a rack on the left, each with its
on/off switch, source, level and its share of the hit as a small waveform.
Click a row to edit it on the right:

- **Lane strip**: source, level, pan, velocity, delay, transpose, variation,
  invert, note filter, output and pitch link.
- **Source**: the controls of the lane's source. For a Body, the pitch and
  amplitude **curves** beside them: drag a point to move it (Shift for fine
  steps), drag the diamond on a segment to bend it, double-click to add or
  remove a point or straighten a segment, right-click for a menu. A curve has
  up to 16 points. What is drawn is what plays: the curve is shown over Sweep
  Time (pitch) or Body Decay (amplitude) and bent by the Bend macro under it.
  The macros are parameters and can be automated; the curves themselves are
  saved with the song and the preset. For the other sources the right half
  shows the lane's waveform.
- **Chain**: the six slots in signal order. Click one to edit it below, drag
  it onto another to swap the two (parameters included; automation stays
  with the slot number), click its dot to bypass it, right-click or
  double-click to pick a type. Below the chain: Type, Band, Mix, Bypass, the
  type's controls and the chain's two crossovers.
- **Master**: output, root note, quality, mono below and output clip, the
  whole hit as a waveform, and the master chain.

The top bar shows the hit as it sounds now, re-rendered whenever something
changes. **Click it** (or Play) to hear it through the plugin without a
keyboard; **drag it** into the DAW to drop it as a 24-bit WAV. Export saves
it wherever you choose. Dragged hits are kept in
`$XDG_DATA_HOME/Substrike/Exports` (`%APPDATA%\Substrike\Exports` on
Windows) so the DAW can go on referring to them. The menu has the window
size, scaling and "Normalise Exported Hits" (to -0.3 dBFS; off by default,
so an export is as loud as the hit).

Every control: drag vertically (Shift for fine), wheel, double-click to type
a value, Ctrl-click for the default. Window size and scaling are kept in
`$XDG_CONFIG_HOME/Substrike/settings.ini` (`%APPDATA%\Substrike` on
Windows).

## Build

Requirements: CMake 3.21+, a C++20 compiler, Ninja (optional). The CLAP,
clap-wrapper and VST3 SDKs are expected in the repository's `CLAP/` folder
(`../CLAP`, override with `-DSUBSTRIKE_SDK_DIR=...`).

```sh
./install.sh                # configure, build, self-test, install to ~/.clap and ~/.vst3
```

Or manually:

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

### Windows

Cross-compiled from Linux with mingw-w64:

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=../shared/cmake/mingw-w64-x86_64.cmake
ninja -C build-win          # build-win/Substrike.clap, build-win/vst3/Substrike.vst3
```

`substrike-render.exe --plugin build-win/Substrike.clap --selftest` runs the
whole self-test against the Windows binary under wine, and
`substrike-gui-host.exe` opens the win32 window there. No Windows DAW has
loaded Substrike yet. Building needs the cross-built Cairo from
`../setup-winbuild.sh`.

## Tools

`substrike-render` loads the built `.clap` like a host does:

```sh
build/substrike-render --plugin build/Substrike.clap --out kick.wav
build/substrike-render --param "L1 Pitch Start=1.6 kHz" --param "L1 Body Decay=1.5 s"
build/substrike-render --param "L2 On=On" --param "L2 Output=Aux" --aux 2 --out click.wav
build/substrike-render --hits 4 --bpm 140 --seconds 2 --key 38
build/substrike-render --list-params
build/substrike-render --selftest
```

`substrike-gui-snapshot out.png [--state file] [--lane 1..8|master]
[--slot N] [--tab pitch|amp] [--scale S]` renders the editor offscreen and
times a repaint. `substrike-gui-host build/Substrike.clap [seconds]
[state-out] [state-in]` is a minimal host that opens the window (run it on a
private Xvfb display with scratch `XDG_CONFIG_HOME` and `XDG_DATA_HOME`);
it prints parameter events and when the output sounds.
`SUBSTRIKE_TEST_DROP=1` opens an XDND drop target beside it that prints what
is dropped on it, which is how the hit drag is tested.

Parameters are addressed by their display name (spaces and case do not
matter); values are read by the plugin's own text parser, so "1.6k", "C2"
(for a frequency) and "1.5 s" all work. The output is a 32-bit float WAV of
the main output, or of lane N's output with `--aux N`.

The self-test covers parameter metadata and text round trips, state round
trips (and loading a 0.1.0 state), curves (a drawn curve plays, is saved as
written, plays back bit-identically, and a state without curves restores the
defaults), the default hit (pitch, silence, sleep),
bit-identical output across block sizes, MIDI and CLAP note dialects,
click-free retriggering, key tracking and other sample rates, and for the
lanes: every source variant finite and asleep after its tail, the note
filter, the trigger delay to the sample, polarity, output routing, pitch link
and transpose, resonator tuning, and Variation on and off. For the slots:
names, defaults and text of every type's controls, their state keys, every
type at its defaults and extremes on a full and a split band, mix and bypass
bit-transparent, a flat band split, band isolation, aliasing at each
quality, what each effect does (EQ gain, compression, gating, transient
lift, filter slope, quantisation, distortion harmonics, folding), hits
reaching a slot on their sample, chain tails, the master clip, mono below
and the output clip.

## Reference material

`!references/` holds third-party kick samples used to measure what the engine
must cover. It is gitignored and is never committed or shipped.

## License

MIT, see the repository's LICENSE.
