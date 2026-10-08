# Substrike – Implementation Plan

A kick drum designer for every style: ambient, pop, hip hop, house, techno,
rumble techno, trance, hardstyle, hardcore. It is not a model of one machine.
A kick is built from up to eight synthesised layers. Each layer has its own
effect chain, the layers can feed one another, and a master chain shapes the
sum. Everything is synthesised; there are no sample sources.

Substrike is **standalone in the same way Aurum is**: it has its own
parameter model, its own preset system and its own Cairo GUI, and it does
not use `shared/`. Code is copied in where that helps (the drive models from
SäureKiste, the window backends from Aurum) and from then on belongs to
Substrike.

## Targets

- Formats: CLAP (native), VST3 (clap-wrapper). Linux x86-64 (X11), Windows
  x86-64 (win32, cross-built with mingw-w64).
- C++20, the same build setup as Aurum: the mingw toolchain file,
  `../winbuild/cairo-mingw`, a static runtime and a per-module window class.
- Identity: `de.ravetracer.substrike`, vendor `Ravetracer`, display name
  "Substrike". Presets use the `.substrike` extension.
- Instrument: note in, stereo main out plus one stereo aux out per lane.

## What the reference set asks for

`!references/` (gitignored, never commit) holds about 90 third-party kicks
across these styles. Measurements from a scratch analysis (zero-crossing
pitch track, 2 ms RMS envelope) give the ranges the engine has to cover:

| Property | Range found | Consequence |
|---|---|---|
| Final pitch | 35-70 Hz typical, 80-145 Hz for tight tonal kicks | tuning by note or Hz, key tracking |
| Start pitch | 60 Hz (no sweep) to ~1.8 kHz | pitch range 10 Hz-10 kHz |
| Sweep settle time | 15 ms to 400 ms | sweeps need both a fast exponential and a long tail |
| Pitch shape | some rise again mid-body (hardstyle, terror) | free breakpoint curve, not monotonic |
| Body to -20 dB | 20-400 ms | |
| Tail to -40 dB | 50 ms to 3.4 s (808-style boom) | long hold and decay, curve per segment |
| Click share (>1.5 kHz, first 12 ms) | -80 dB (pure sine) to -1 dB | a dedicated transient source |
| Body harmonics (>3.5 x f0, 40-200 ms) | -62 dB (clean) to -9 dB | distortion from subtle to total |
| Crest factor of the body | 3 dB (hardstyle, squashed) to 17 dB | clipper and compressor in the chain |
| Stereo (side/mid) | mostly mono, a few at -11 to -16 dB | stereo tops allowed, mono below a crossover |
| Tail pitch stability | unstable on rumble and hardcore tails | rumble needs noise-like, smeared tails |

The zero-crossing tracker fails on the most heavily distorted kicks. The
calibration tool in phase 7 needs a YIN-style tracker.

## Signal flow

```
note on ─ trigger ─┬─ lane 1: source ─ slot1 ─ ... ─ slot6 ─ guard ─ level/pan ─┬─────┐
                   ├─ lane 2: ...                                                │     │
                   │   ...                                                       │     │
                   └─ lane 8: source = BUS(lane n, pre/post) ─ slots ─ guard ────┤     │
                                                                                 │     │
                                                     sum ─ master slot1..6 ─ mono-below ─ out
```

### Trigger and voices

- Monophonic per lane. A new hit fades the previous one out over 2-5 ms. An
  optional overlap mode lets the old tail ring under the new hit.
- Each lane triggers on any note or on one chosen note, so Substrike can
  also be played as a drum kit.
- Pitch follows the note through a key-track amount. 0 % gives a fixed kick
  and 100 % a chromatically playable 808.
- Per-lane trigger delay of 0-100 ms and a polarity switch, for aligning
  layers.

### Lane sources

1. **Body**: the tonal oscillator.
   - Waveforms: sine, triangle, saw, square, and an additive mode with 8
     partials; tanh shaping on top.
   - Start phase 0-360°.
   - FM from its own modulator (ratio, index, envelope) and feedback FM.
   - Drift.
   - Pitch curve and amp curve as breakpoint envelopes, with up to 16
     points and a curvature per segment.
   - Pitch can be linked to another lane's Body, so clicks and harmonics
     stay in tune.
2. **Click**: a transient generator.
   - Impulse, coloured noise burst, tonal blip, or zap (a sub-millisecond
     sweep).
   - Short envelope and its own filter.
3. **Noise**: filtered noise with an envelope, for tops, beater and shell
   noise, and rumble texture.
4. **Resonator**: 2-8 damped modes excited by click or noise. Use it for
   acoustic and shell-like kicks and for tonal ambient kicks.
5. **Bus**: the input is another lane's output, pre or post its chain, or a
   sum of chosen lanes. A bus lane makes no sound of its own. This is the
   rumble lane.

### Curves and automation

A breakpoint curve is state, not a parameter. Every curve has automatable
**macro parameters** that scale it: start, end, time and shape for pitch,
and attack, hold, decay and shape for amplitude. A drawn curve and an
automated sweep therefore work together. A curve editor that only redraws
would make the curve impossible to modulate.

### Effect slots

Each lane has 6 slots and the master has another 6. A slot has:

- **Type**: chosen from the list below. Not automatable.
- **Band**: Full, Low, Mid, High, Low+Mid or Mid+High. The lane has two
  crossovers (LR4, 3-way split) and processes only the chosen band(s). The
  other bands pass through and are summed back, and the sum is flat in
  magnitude. Full skips the split entirely. "Filter the mids, keep the
  sub" is a Filter or EQ slot on Mid+High.
- **Mix**, **bypass**, and six generic parameters A-F. The type decides what
  A-F mean, and the GUI labels them accordingly.

Order is the slot order. Dragging a slot swaps its contents with the
target, parameters included. Automation stays with the slot, as in other
modular chains, and the manual has to say so.

Slot types:

| Group | Types |
|---|---|
| Drive | Distortion (SäureKiste's drive models, ported, oversampled), Wavefolder, Hard/soft Clipper, Bitcrush/Downsample |
| Tone | Filter (SVF LP/HP/BP/notch/peak, 12/24 dB, envelope amount), 3-band EQ + tilt |
| Dynamics | Compressor, Transient shaper, Gate / Re-envelope (reshapes a smeared tail into a tight one) |
| Time | Reverb (compact FDN tuned for smear, not halls), Delay (sync, filter and drive in the feedback, ping-pong), Experimental delay (pitch-shifted, reversed and diffused feedback, multi-tap), Smear (allpass diffuser: time smear without a tail) |
| Other | Ring mod / frequency shifter, Stereo (width, Haas), Utility (gain, polarity) |

A global quality setting chooses 1x, 2x or 4x oversampling for the drive
group.

### Rumble and transient guard

The rumble is built as a layer, not as an effect:

```
lane 1  Body + Click ─ Clipper ──────────────────────────────── out   (clean transient)
lane 2  BUS(lane 1, post) ─ Distortion ─ Reverb ─ Distortion ─ Filter LP(Low) ─ guard ─ out
```

**Transient guard** sits at the end of every lane. It is either a
fade-in window (stay silent for X ms, then fade in over Y ms) or a duck
driven by another lane's envelope. It keeps the rumble out of the click,
so the transient stays clean while the tail delivers the pressure. A Gate /
Re-envelope slot after the reverb ties the rumble to the groove.

### Master

The master has 6 slots like a lane. After them come **mono below** (an
adjustable crossover that makes the low end mono), an output clipper and the
output level.

### Modulation

- Sources:
  - velocity, note, a random value per hit
  - 4 LFOs: free, synced, or retriggered per hit
  - 4 modulation envelopes using the same breakpoint editor
  - 8 macros
  - lane envelope followers
- Matrix with 32 slots: source, destination (any continuous parameter),
  amount and curve. Modulation is applied per hit, sample-accurate and
  smoothed.

### Parameter budget

Roughly 115 parameters per lane: source ~40, 6 slots x 10, the rest for the
lane itself. With master, modulation and global parameters that comes to
about 1150 in total. IDs are assigned in blocks per lane and per slot and
must never change.

## Status

Phase 1 is done (0.1.0): the CLAP and VST3 build for Linux and Windows, one
lane with the Body source and its macro parameters, plain-text state, and the
offline renderer with its self-test.

Phase 2 is done (0.2.0): eight lanes; Body completed (five waveforms, FM,
feedback, shaping, drift); the Click, Noise and Resonator sources; trigger
delay, polarity, note filter, transpose, Variation and pitch link per lane;
the eight per-lane outputs. The self-test passes natively and under wine,
and clap-validator is clean. No DAW has loaded it yet.

Decided while building phase 2, and worth keeping:

- Parameter blocks inside a lane: 0-99 the lane, 100-199 Body, 200-299
  Click, 300-399 Noise, 400-499 Resonator, 500-599 spare (the Bus source).
- The trigger delay is 0-100 ms and only later, never earlier: a plugin
  cannot play before the note. It is a countdown to the hit, not an audio
  delay, so it costs nothing.
- Pitch Link reads the named lane's Body pitch parameters and transpose,
  whatever that lane plays and whether it is on. Links do not chain. A Body
  follows the whole drop; a Resonator and a Click blip or zap take the end
  pitch as their tuning.
- Transpose shifts every frequency of the lane's source, filter cutoffs
  included.
- Without Variation every hit uses the same seed, so noise, drift and the
  click repeat bit for bit. With it the seed changes per hit, and pitch
  (up to 50 cents), level (3 dB) and decays (20 %) vary.
- Additive partials and resonator modes are normalised by the sum of their
  levels, so a struck lane never leaves full scale. The Noise filter's
  output falls as 1/sqrt(Q), which keeps resonant settings near the level
  of plain noise.
- The aux outputs carry the lane before the master level.

Phase 3 is done (0.3.0): the slot model with band select and oversampling,
nine effect types, the master chain, mono below and the output clip.

Decided while building phase 3:

- Slot ids: lane offset 600 + 50 per slot, master 9000 + 50 per slot. Within
  a slot: 0 Type, 1 Band, 2 Mix, 3 Bypass, 4-9 A-F, 10-49 spare. Lane
  crossovers at lane offsets 12 and 13; Quality is global id 3; the master's
  crossovers, Mono Below and Output Clip are 9300-9303.
- A-F are generic CLAP parameters whose display name, range, unit, text and
  default come from the slot's type (the shape table in Params.cpp); a type
  change resets them to its defaults, emits the new values as output events
  and asks the host to rescan the names (CLAP_PARAM_RESCAN_INFO). In the state
  they are written under the type's key and unit (`l1.slot1.cutoff=345`), so
  a type's ranges can change later without breaking a saved song. A model or
  mode letter stays a continuous parameter shown as a label, because whether
  it is a choice depends on the type.
- Oversampling is a polyphase IIR halfband (hiir's design, coefficients
  computed offline): 99 dB from 0.58 of the base rate in the first stage,
  90 dB in the second; no latency. It runs around a slot's whole effect,
  for the drive group only.
- The three-way split is LR4, with the low band through the upper
  crossover's allpass, so the sum is flat (checked to 1e-6 dB).
- SaeureKiste's drive stage is ported unchanged except that it no longer
  oversamples itself; its one-pole filters keep double state, because a
  float state stalls short of a constant input at oversampled rates and
  leaves an offset that never decays.
- A lane whose chain rings on keeps running until its output has been under
  -120 dB for 50 ms, counted per sample from where the voices stopped, so
  where it stops does not depend on the block size. The master does the
  same with its own chain.
- The output clip comes before the output level, as the plan said; the
  Output parameter can therefore still push the main output past full
  scale.

Phase 4 is done (0.4.0): the editor, the Body curves as state, and the hit
export with drag and drop. Tested offscreen, in a minimal host on Xvfb
(including a drag onto an XDND target) and the win32 build under wine; no
DAW has opened the window yet.

Decided while building phase 4:

- Layout: a rack of the eight lanes and the master on the left (on/off,
  source, level, a waveform of each lane's share of the hit); the selected
  one on the right as a strip (lane controls), the source panel and the
  chain with the selected slot's controls under it. The master shows the
  whole hit where a lane shows its source.
- Theme: accent `#8F7CF7`, the chassis greys tinted towards it
  (`src/gui/Graphics.h`); the amplitude curve and band labels in a teal
  (`#6CC6C0`) so pitch and level never look alike.
- The curves are the Body's pitch and amplitude curves, two per lane. The
  state keeps them in a `[Curves]` section as `x,y,curvature` triples
  (`l1.pitch=0,1,0;0.4,0.2,0.5;1,0,0`); a state without one gets the default
  falling segment, which with the Bend macro is exactly what 0.3.0 played.
  The main thread owns them and hands each change to the audio thread
  through a queue; the host is told the state is dirty, since a curve is not
  a parameter. The Noise source keeps its parametric envelope for now.
- The editor shows a curve scaled by its macros and bent by the Bend macro,
  so what is drawn is what plays: pitch in Hz between Pitch Start and End
  over Sweep Time, level over Body Decay.
- The preview renders a private engine on a worker thread, 40 ms after the
  last change, up to 8 s or until the engine sleeps. It plays the root note
  at full velocity, so a lane with a note filter for another note shows as
  silent. `EngineParams::tapLanes` sends every lane to its aux bus for it,
  whatever the lane's Output says; the plugin never sets it.
- Play asks the audio thread for a hit and the host for a process call
  (`request_process`), which wakes a sleeping plugin.
- A GUI edit goes through a queue to the audio thread, which applies it the
  way it applies a host event -- a Type change resets the letters there and
  tells the host -- and sends it on to the host. `request_flush` is always
  called, because a sleeping plugin is not processed until something comes.
- A slot swap is one gesture over both slots' twenty parameters, types
  first, then the rest sent even where unchanged, because the type change in
  front of them has just reset the letters.
- The export is a 24-bit stereo WAV at the host's rate, optionally
  normalised to -0.3 dBFS. A drag writes it into the export folder first and
  then starts the drag: X11 as an XDND source offering `text/uri-list` and
  `text/plain`, win32 through OLE `DoDragDrop` with `CF_HDROP`.
- The window backends, widgets, file dialog and settings come from Aurum.
  Panels whose controls depend on the selection are rebuilt from the timer,
  never from inside a widget's handler.

## Phases

1. **Scaffold and first kick.**
   - CMake with Windows and VST3 copied from Aurum, the CLAP skeleton
     (params, state, note ports, audio ports), an offline renderer and
     self-test, clean clap-validator.
   - One lane with a Body source and default curves. 0.1.0.
2. **Sources and lanes.**
   - Body completed; Click, Noise and Resonator sources.
   - 8 lanes with alignment, pitch link and per-lane note.
   - Per-lane aux outputs. Done in 0.2.0.
3. **Effect framework and the first effects.**
   - Slot model, band split, oversampling.
   - Distortion (port), Clipper, Wavefolder, Bitcrush, Filter, EQ,
     Compressor, Transient shaper, Gate.
   - The master chain and mono below. Done in 0.3.0.
4. **GUI.**
   - The window backends and widget base copied from Aurum, with a new
     theme.
   - Editors: the lane strip, the chain row with drag-reorder, the
     breakpoint curve editor, and slot panels.
   - Hit export to WAV, with drag-and-drop into the DAW. Done in 0.4.0.
5. **Time effects and rumble.**
   - Reverb, the delays, Smear, Bus lanes and the transient guard.
6. **Modulation.** LFOs, envelopes, macros and the matrix in the GUI.
7. **Presets and calibration.**
   - Format and browser. Factory presets across every style named above.
   - A metrics tool (YIN pitch, the table above) that checks the presets
     cover the ranges of the reference set.
8. **Release.**
   - Manual with screenshots, `release.sh` integration, Windows under wine.

## Decided extras

- **Per-lane outputs.** Besides the stereo main output, the plugin has eight
  stereo aux ports, one per lane, each carrying that lane's signal after its
  chain and guard. A lane can go to the main output, to its aux port or to
  both, so kick and rumble can be mixed separately in the DAW. This comes
  with the lanes in phase 2.
- **Hit export.** The current hit can be rendered offline to a WAV file
  (length from the tail, peak-normalised as an option). The rendered file
  can also be dragged straight from the window into the DAW's arranger:
  an XDND drag source on X11 and an OLE `DoDragDrop` with `CF_HDROP` on
  win32. This comes in phase 4 with the GUI.
- **Theme: violet.** The accent is a cold violet, `#8F7CF7`, with the chassis
  greys tinted towards it.
