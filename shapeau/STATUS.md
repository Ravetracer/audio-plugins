# Shapeau — status

Version 0.3.0. Linux and Windows, CLAP and VST3. 56 parameters, 9 factory
presets.

Builds clean on Linux and cross-built for Windows in the repository's build
container (`./devbox.sh`). `shapeau-render --selftest` passes all 40 checks on
Linux and under wine; the Windows build imports only system DLLs and its win32
editor has been opened and photographed under wine. No DAW has loaded it and
nothing has been listened to -- the user tests on another machine.

`docs/PLAN.md` is the plan: four phases towards the shape-based effects rack it
is modelled on. Phase 1 is this release.

## 0.3.0 -- the scope and the chain

- **The scope.** Behind the shape, the audio it acts on: input in grey, output
  in the accent, laid out along one cycle of the selected effect, plus a
  playhead and the modulation trace (the effect's modulation after Smooth).
  The audio thread writes 480 bins itself, at the phase the shape is read at
  (`Scope` in `dsp/engine.h`), so it is sample-accurate; only while a window is
  open (`setScopeTarget()`), and nothing is written otherwise. With ALL the
  picture is the rack's input and output, with one band that band's. Volume,
  filter, drive, crush and time are drawn as magnitude from the bottom; pan and
  width as a waveform about the middle.
- **One effect at a time.** The window is the editor, the GLOBAL panel and the
  selected effect's panel -- every effect's panel is its own row in the spec,
  and the fork lays out row 0 and row 1 + module. The editor grew from 300 to
  440 pixels, and the window shrank from 996 to 864.
- **The chain.** The effect bar shows the chain in order: a power dot (bypass,
  stays in the chain), a cross (out of the chain, and switched off), drag to
  reorder, + to add. An empty chain shows a picker of the seven. The order is
  the audio's order (`Engine::processBand()` walks it); it lives in `CurveSet`
  beside the shapes, so it reaches the audio thread through the same seqlock,
  and in presets as a `chain = vol flt drv` line and in state version 3.
- **Old presets need nothing.** An effect that is switched on but not listed
  still runs, after the listed ones, in the order the effects always ran in
  (`effectiveChain()`). A preset written before the chain is an empty list and
  some switches, so it plays as it did -- every factory preset renders bit for
  bit what 0.2.0 rendered except the three below -- and a host automating a
  switch does not need the window to have been opened first.
- **Filter and Drive Smooth now act.** Both were read into the engine and never
  applied, so both knobs did nothing. The filter's position is now smoothed per
  control block, the drive amount per sample. That changes the three factory
  presets that use either (Drive Pump, Filter Wobble, Gated Stabs); the other
  six are unchanged.
- The GUI host plays a drum loop instead of a sine, so the scope can be
  developed and photographed; and builds on Linux only, because it is an X11
  program -- the CMake that offered it on Windows did not compile.

## What works

- **The band split.** Three Linkwitz-Riley fourth-order bands at 200 Hz and
  2 kHz by default. A flat shape in all three sums back to the input to within
  **0.08 dB** worst-case band-sum error, which is what makes a shape the only
  thing that colours the sound.
- **The seven stages**, in the chain's order per band; volume, filter, drive,
  crush, time, width, pan when nothing has been reordered.
- **The shape model.** Points in normalised time and value with a corner
  weight and a segment bend, text form in presets and state, evaluated per
  sample. Twenty-one shapes: seven stages × three bands. A step shape is the
  same model with near-vertical cell edges, so it saves, loads and edits like
  any other.
- **The editor.** Click adds, drag moves, double-click deletes, right-click
  cycles the weight; dragging the line between points bends that segment and a
  right-click on the line straightens it; SNAP to a sixteenth; STEP paints
  sixteen cells; twelve generators behind SHAPE; ALL bands or one; a stage that
  is switched off when a shape is drawn is switched on. Verified by driving the
  real window with xdotool and reading the saved state back: the shape, the
  three bands (identical in ALL), the stage switch, the bend and the step
  pattern all follow the mouse.
- **Beat lock.** Per-stage Rate (1/32 to 4 bars) and Phase, taken from the
  host's transport position. With the transport stopped the shapes free-run at
  the host's tempo (120 BPM if the host has none).
- **Presets.** Nine factory presets, compiled in, each carrying its shapes; the
  shared text format with the shapes as extra lines; user presets and CLAP
  preset discovery through the shared provider.

## What is measured

From `--selftest`, on the built plugin:

| Check | Result |
|---|---|
| Band split flatness (unity shapes, all three bands) | worst error 0.08 dB |
| Volume shape at 50 % | -12.0 dB (the curve is squared) |
| Volume shape at 0 | silence |
| Lowpass at 500 Hz, 200 Hz in / 8 kHz in | passes / stops |
| Drive at 0 / at 90 %, soft clip | THD 0.0048 / 0.236 |
| Every drive style | finite, bounded, and each one distinct |
| Time shape at the top (read head at "now") | -0.10 dB against the input |
| Falling time shape | moves the read head, stays bounded |
| Segment bend | bows the curve, keeps its endpoints, survives the text form |
| Sixteen steps | flat inside a cell, jumps at the edge, fits in the point budget |
| Neutral pan and width | 0.000 dB against the same rack with them off |
| Pan at the bottom of its curve | hard left: right channel below 1 % |
| Width at the bottom of its curve | mono to within 1e-5 |
| Crush at 4 bits / 8x | -7.9 dB against the clean signal, bounded |
| Crush at zero amount | 0.000 dB against the clean signal |
| Preset text with shapes | round-trips point for point |
| All nine factory presets | parse with their shapes |
| The built `.clap` through the CLAP API | loads, instantiates, exposes 56 params, one stereo in / one stereo out, processes without producing anything non-finite, offers preset-load, loads a factory preset |

Measured by hand while tuning the filter:

| Resonance | Broadband noise through a 1 kHz lowpass |
|---|---|
| 0 % | -14.9 dB (the band, not the filter: Q 0.5) |
| 25 % (default) | -13.7 dB |
| 55 % | -11.5 dB |
| 80 % | -8.3 dB |
| 95 % | -3.3 dB, peak 0.316 from an input peak of 0.2 |

The filter's resonance maps to the shared SVF's `k` = 2 … 0.02, i.e. Q 0.5 to
50, so the top of the knob is just short of self-oscillation. That is
deliberate: the filter drive in front of it is meant to be able to excite it.

## What is not verified

- **No DAW has loaded it.** The `.clap` has been loaded through the CLAP API
  by the self-test and by the GUI host, under Xvfb. Bitwig has not been tried.
- **Windows is not built.** The CMake blocks for Win32 and VST3 are written
  following SäureKiste's, and the window fork carries the shared window's
  win32 backend as it stood when it was copied -- including the per-module
  window class registration, which is the bug every fork of this window has
  had -- but no mingw cross-build has been run. `setup-winbuild.sh` and a
  Windows Cairo at `../winbuild/cairo-mingw` are what it needs
  (`SHAPEAU_WIN_CAIRO`).
- **The VST3 is not built.** `-DSHAPEAU_BUILD_VST3=ON` is wired to the
  clap-wrapper the same way; it has never been configured.
- **Nothing is tuned by ear.** The defaults, the nine presets and the drive
  and crush levels were chosen from the measurements above, not from listening.
  The self-test proves the stages do what they claim and that nothing is
  non-finite or unbounded; it cannot say whether the presets sound good. The
  three new presets have only been level-checked. The renderer's chord is all
  low band, so the mid and high bands in every preset are unexercised by the
  offline renders as well.
- **Only 48 kHz** is exercised by the self-test. The engine takes any sample
  rate (every coefficient is derived from it) and the renderer accepts
  `--rate`, but 44.1, 88.2 and 96 kHz have not been rendered and compared.
- **The window has not been checked on a HiDPI screen.** It is laid out at
  1x, 1400 px of content plus margins.
