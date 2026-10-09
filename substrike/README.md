# Substrike

A kick drum designer for Linux and Windows as a native CLAP instrument (plus
VST3 through clap-wrapper). Everything is synthesised; nothing is sampled.

The goal is one instrument for every kind of kick, from ambient through hip
hop, techno and trance to hardstyle and hardcore: up to eight layers, each with
its own effect chain in free order, layers feeding one another for rumble, a
master chain, and modulation. The design and the phases are in
[docs/PLAN.md](docs/PLAN.md).

## Status: 1.1.0, released

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
- **Bus**: no sound of its own; the lane plays the sum of the lanes switched
  on in its Bus panel, taken before or after their chains (after means after
  their guard too, before their level and pan). This is the rumble lane: a
  copy of the kick through its own drive, reverb and filter. Lanes run in
  the order their inputs need, whatever their numbers; a lane never hears
  itself, and two bus lanes feeding each other hear silence from the one
  that would close the loop.
- **Transient guard** at the end of every lane: a window that keeps the lane
  silent for a time after each of its hits and then fades it in (Delay,
  Fade), and a duck that lowers it while another lane sounds (Duck source,
  Depth at that lane's full scale, Release). It keeps a rumble out of the
  click and lets the tail carry the pressure.
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
  output is mono), an Output Clip (Off, Soft, Hard, or Limit: a limiter
  holding -0.3 dBFS) and the output level.
- **Modulation** (see below): four LFOs, four curve envelopes, eight macros,
  velocity, note, a random value per note and a follower per lane, routed
  through a 32-slot matrix to any continuous parameter.
- **121 factory presets** in 18 categories, a preset browser, user presets,
  and the presets in the host's own browser (see below).
- **Limiters**: a Limiter slot for any lane or the master chain, and Limit
  as the master's Output Clip.
- **Tune**: one knob on the master that moves every lane (±12 semitones, to
  the cent); each Body lane shows the note it ends on ("ends on G1 -36 ct").
- **Undo and redo** for every edit (Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y, and the
  arrows in the top bar); the history lives in the plugin, so closing the
  window keeps it, and loading a song starts it afresh.
- **A live oscilloscope** of the output at the foot of the rack.
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
| Transient | Attack, Sustain (each a share of up to 12 dB of lift or cut), Speed, Output |
| Gate | Mode (Gate opens on level, Hit runs from every hit), Threshold, Attack, Hold, Release, Range |
| Reverb | Size, Decay (to -60 dB), Damping, Pre-Delay, Low Cut, Width: an 8-line FDN for small rooms and smear, output wet only |
| Delay | Time, Sync (Free or 1/64 to 1/1, dotted and triplet, from the host's tempo), Feedback, Color (lowpass or highpass in the feedback), Drive, Ping-Pong |
| Warp | Time, Sync, Feedback, Pitch (each repeat shifted, -12 to +12 st), Mode (Forward, Reverse, Taps), Diffusion |
| Smear | Time (spread over the stages), Diffusion, Stages (4-16), Width: an allpass chain that smears a hit without a tail |
| Ring Mod | Mode (Ring, Shift Up, Shift Down), Frequency, Env, Env Decay; restarts on every hit |
| Stereo | Width (0-200 %), Haas (up to 30 ms on either side) |
| Utility | Gain, Polarity, Channels (Stereo, Mono, Swap, Left, Right) |
| Limiter | Gain (into it), Ceiling, Release: a peak limiter without lookahead or latency, holding 25 ms before it lets go |
| Comb | Start, Spacing, Bands (1-32), Gain (cut or boost), Width (each band's width as a share of the spacing, the same in Hz all the way up), Taper (the gain fading out or in along the comb): a bank of bells, since 1.1.0 |

Delay and Warp add their echoes to what comes in, so Mix sets the echo level
and never takes the hit away; Reverb gives the reverb alone, so Mix is a
dry/wet balance and 100 % is what a rumble chain wants. A slot's time
effects share one block of delay memory (two seconds of stereo at the
host's rate), so the 54 slots take about 45 MB at 48 kHz.

**Band** processes Full, Low, Mid, High, Low+Mid or Mid+High of the lane,
split at the lane's Crossover Low and High (LR4); the rest passes by, and the
sum is flat. **Chain Key Track** (0-100 %, since 1.1.0) moves a lane's
tuned letters with the note, from each hit on: Filter Cutoff, EQ Mid Freq,
Comb Start and Spacing, and Ring Mod Frequency, by the same law as the
Body's Key Track, so a notch on the second harmonic stays there in every
key. The master chain has none.
**Quality** (1x, 2x, 4x) oversamples Distortion, Clipper,
Wavefolder, Bitcrush and the Output Clip. A chain that rings on after its hit
keeps the lane running until it has been quiet for 50 ms, then sleeps; a
delay's gap before its next echo does not count as quiet. The
state stores each slot's controls under their names and units, e.g.
`l1.slot1.drive=73`.

## Modulation

Every route in the matrix has a **Source**, a **Destination**, an **Amount**
and a **Curve**. The amount is a share of the destination's whole range,
either way (+25 % moves a knob a quarter of its travel at the source's
full value), so it means the same on every parameter; routes on the same
destination add up, and the result stays inside the range. Curve bends the
source: above 0 it rises early, below 0 late. Source and destination are
settings, not automatable; amount and curve are.

| Source | Range | What it is |
|---|---|---|
| Velocity | 0..1 | the velocity of the last note the destination's lane played |
| Note | -1..1 | that note against the root note, +-1 at two octaves |
| Random | -1..1 | a new value with every hit, each lane its own |
| LFO 1-4 | -1..1 | Sine, Triangle, Saw Up, Saw Down, Square, S&H or Smooth random; Rate in Hz (0.01-200) or Sync (1/64 to 4/1 from the host's tempo); start Phase; Retrigger restarts it with every note, otherwise it runs free (a synced one locked to the song position) |
| Env 1-4 | 0..1 | a breakpoint curve over Time (1 ms - 10 s) from every note, once or looped; drawn like the Body curves |
| Macro 1-8 | 0..1 | eight automatable knobs, for one control over several parameters |
| Follow 1-8 | 0..1 | lane N's output level (instant up, 50 ms down), after its chain and guard |

The matrix is applied every 32 samples, counted from the host's sample
clock rather than from the block, and again at every note, so a hit starts
with its own velocity, note and random value, and a render is the same at
any block size. Values that glide (slots, levels) glide between the steps.
With no route set the engine runs exactly as without modulation.

The state stores a destination by the parameter's name and an envelope's
curve in the `[Curves]` section (`mod.env1=...`).

## Presets

A preset is a state file with a name, a category, tags and a description
in its header; the factory presets are `presets/*.substrike`, compiled into
the plugin, so the `.clap` is all that gets installed. They list only what
differs from the defaults, which is why **a default must never change**
without checking every preset (render them before and after).

| Category | What is there |
|---|---|
| Init | the default kick |
| Techno | hard and overdriven first -- long slow drops, bodies saturated between two lowpasses and clipped into a brick, hard clicks (Concrete Thump, Brick Wall, Schranz Hammer, Tail Driver, ...) -- and the classics beside them (Minimal Tick, Detroit Round, Dub Chamber, Acid Floor) |
| House, Trance, Psytrance, Drum & Bass, Pop | the club and studio standards |
| Rumble | a deep rumble floor under a kick, from a bus lane: a big dark mono reverb clipped flat into a level, lowpassed and ducked by the kick; hard techno kicks over it (Hard Rumble and Crash Rumble among them) and variations: gated, rolling, LFO-moved, pitched, diffused, extra deep |
| Hip Hop, Trap | boom bap and dusty knocks; long tuned sub booms that follow the keys (root C1) |
| Hardstyle, Hardcore | a zap into a distorted tail, with the hardstyle pitch rise; gabber, terror, frenchcore, uptempo, doomcore |
| Industrial, Lo-Fi | metal plates, factory stomps, rust; tape, bit boxes, vinyl and cassette wobble |
| Acoustic, Ambient | studio, jazz, rock, felt, marching and frame drums; distant thunder, heartbeats, clouds, shimmers |
| Experimental | zap guns, glitches, rubber bands, reverse swells, chaos engines, stutters, broken robots |
| Showcase | velocity morphing, macros to perform with, a humanizer, a wobbling tail, a follower duck, all eight lanes at once, a kick kit across four notes, a harmonic stack, split outputs, a wide top |

Every factory preset peaks at -1 dBFS for a full-velocity hit on its root
note, and the clicks, sweeps and starts of the club and hard categories were
calibrated against the reference set with `substrike-metrics` (see the
plan).

**The browser**: click the preset name in the top bar (or use the arrows
beside it). Categories on the left, the presets of one in the middle, the
description and tags of the one under the pointer on the right. Click a
preset to load it and hear it (the menu's "Play Presets When Loaded" turns
the playing off); Up and Down step through the list, Esc closes. **Save
As...** saves the current state as a user preset in
`$XDG_DATA_HOME/Substrike/Presets` (`%APPDATA%\Substrike\Presets` on
Windows); right-click a user preset to delete it. The state remembers the
preset it came from.

Loading a preset or a state is a fresh start: what still sounds fades out
over 5 ms as it was, then the engine starts clean with the new settings, so
nothing glides or echoes from the old preset into the new one.

**In the host**: Substrike has CLAP preset discovery and preset-load, so a
host with a preset browser (Bitwig's, for one) lists the factory presets
with their categories and tags, and the user folder once it exists.

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
  shows the lane's waveform. A Bus lane shows a switch per lane and the tap.
- **Chain**: the six slots in signal order. Click one to edit it below, drag
  it onto another to swap the two (parameters included; automation stays
  with the slot number), click its dot to bypass it, right-click or
  double-click to pick a type. Below the chain: Type, Band, Mix, Bypass, the
  type's controls and the chain's two crossovers. **Guard**, at the end of a
  lane's chain, shows the transient guard's controls there instead; its dot
  is lit when it does anything.
- **Master**: output, root note, quality, mono below and output clip, the
  whole hit as a waveform, and the master chain.
- **Modulation** (the rack's last row, with the number of routes in use):
  the eight macros; the LFOs and envelopes behind tabs, an LFO with two
  cycles of its shape, an envelope with its curve; and the matrix, sixteen
  routes per page. Click a destination for a menu of every parameter by lane
  and module (only the source the lane plays and the slots in use), right-click
  it to clear it.
- **Right-click any knob** to modulate it: Add picks a source and puts it in
  the first free route at +25 %; the routes already on it are listed with
  their amounts, to remove or find in the matrix. A knob under modulation
  shows the reach of its routes as a teal arc around its ring, and a bright
  dot on the arc where the matrix has it right now.

Velocity, Note and Random are per lane: a route onto a lane's parameter
reads the note that lane played (its note filter decides), so in a kit
every lane keeps its own velocity, and each lane draws its own random value.
A route onto a master parameter reads the last note of all.

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
build/substrike-render --preset techno-concrete-thump --out thump.wav   # a factory preset by its load key
build/substrike-render --state my.substrike --save-state full.substrike  # any preset or state file
build/substrike-render --list-presets
build/substrike-render --all-presets --seconds 8 --outdir renders/
build/substrike-render --selftest
```

`clap-validator validate -x "preset-discovery-(crawl|load)" build/Substrike.clap`:
those two tests hang in clap-validator 0.4.1 on any plugin whose presets
are inside the plugin (a deadlock in its metadata receiver); the self-test
crawls the presets itself.

`--lead N` starts the first hit N samples in. `substrike-metrics` measures
kicks from WAV files -- final and start pitch, sweep time, body and tail
length, click share, body harmonics, crest factor, stereo and tail drift --
and `--compare references/ renders/` shows how far one set covers the other
and the nearest of the second set for every file of the first.
`substrike-preset-check` parses every factory preset and reports unknown
keys, values out of range and missing metadata; `install.sh` runs it.

For the release:

- `tools/make-manual.sh [outdir]` builds the manual (PDF and HTML) from
  `docs/manual.md`, with the parameter summary and the preset library
  generated by `tools/docgen.cpp` from the plugin's own tables; `release.sh`
  calls it.
- `tools/make-screenshots.sh` renders the manual's pictures into
  `docs/images/` offscreen with `substrike-gui-snapshot` (which takes
  `--preset KEY` and `--browser`), cutting the panels out of the window.
- `tools/make-demos.sh` renders the listening examples into
  `dist/demos/Substrike/`: eighteen presets as short loops at their style's
  tempo, one gain towards -16 LUFS within -1 dBTP, MP3 256 kbps.

All three work in `build/` and overwrite their output; none deletes
anything.

`substrike-gui-snapshot out.png [--state file] [--lane 1..8|master|mod]
[--slot N|guard] [--mod-tab 1..8] [--tab pitch|amp] [--scale S]` renders the editor offscreen and
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
and the output clip. For phase 5: delay echo times, levels, ping-pong and
sync to a host tempo; a long delay's gaps not ending its tail; reverb decay
time, width and pre-delay, and its level; smear keeping the energy; ring
modulation and both frequency shifts; stereo width and Haas; utility
polarity, gain and channels; Warp's pitch, reverse and taps; bus lanes
(post and pre chain, either order, loops, sums); the guard's window to the
sample, the duck; and a full rumble setup, bit-identical across block
sizes. For phase 6: a macro on a level, velocity and note on a pitch to the
expected frequency, a random value per hit, retriggered and free LFOs (and
bit-identity at another block size), a synced LFO switching on the eighth
notes, an envelope and a looped one, a follower ducking another lane, and
routes and envelope curves through the state; a modulated level reaching a
hit's first sample after a pause. For phase 7: preset discovery (every
factory preset described with its tags), every factory preset loaded
through preset-load, named in the state, at -1 dBFS and ending within 10 s,
and an unknown load key refused; a preset switch while the old one rings
fading it out cleanly and then playing the new one exactly; both limiters holding their ceilings at 1x
and 4x, and the output limiter bending a held body far less than a clip.

## Reference material

`!references/` holds third-party kick samples used to measure what the engine
must cover. It is gitignored and is never committed or shipped.

## License

MIT, see the repository's LICENSE.
