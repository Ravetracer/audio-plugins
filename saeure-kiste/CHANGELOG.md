# SäureKiste -- changelog

Versions follow `X.Y.Z`: major / new features / bug fixes. Dates are the day
the version was committed.

## 0.15.2 -- 2026-09-25

- **The continuous controls glide.** Cutoff, Resonance, Env Mod, Volume, Drive,
  Tone, Tuning and every other continuous knob follow a new value over about
  20 ms instead of jumping to it, so fast sweeps and host automation no longer
  zipper. Switches and time controls still act at once.
- The delay's Mix, Feedback and Width glide too, and switching the delay off
  fades the repeats out instead of cutting them.
- Presets render exactly as before: values set on a silent instrument are not
  glided.

## 0.15.0 -- 2026-09-23

- **Per-pattern chains.** CHAIN and REPEAT are set per pattern and stored in
  the bank (state version 4; presets carry `seqN_chain` / `seqN_repeat`).
  LENGTH stays one setting for the whole bank.
- The old `chain_mode` / `chain_repeat` parameters are kept for their ids and
  hidden from the host. An older preset or state, or a host event on either
  parameter, applies the value to every pattern.
- The chain is walked forward from the start pattern; a backward seek walks
  again from the top. A pattern picked while a chain runs plays next.
- **TRANSP - / +** moves the shown pattern by a semitone, carrying notes across
  octaves. Notes clamped at the grid's edge are remembered, so transposing back
  restores the original shape.
- Bank rows reordered to CHAIN, REPEAT, LENGTH, TRIGGER.
- The grid's title row is grouped into sections (OCT, TRANSP, shifts, MIDI,
  MAP, seed and GEN). CLEAR is removed -- DEL in the bank does the same. PREV
  and NEXT moved into the bank under the pattern cells. The pane is 24 px
  taller.
- Long help-line tips wrap at the window's width.

## 0.14.0 -- 2026-09-23

- **TRIGGER** sets when a pattern selected during playback takes over:
  At End (default, the 0.13.0 behaviour), Instant (next step, keeping the
  place in the bar) or Restart (next step, from the new pattern's first step).
- **REPEAT** (1 to 256) sets how often each pattern in a chain plays before
  the chain moves on.
- While a chain runs, the grid, the bank cursor and the playhead follow the
  playing pattern; edits, GEN, PREV/NEXT and the MIDI drag act on it.
- The bank's controls are four rows: CHAIN, LENGTH, REPEAT, TRIGGER.

## 0.13.0 -- 2026-09-23

- Selecting a pattern while the sequencer runs no longer switches on the spot:
  the current pattern plays to its last step, then the new one starts. Applies
  to bank clicks, PREV/NEXT, Live-mode pads and automation. The queued pattern
  flashes until it takes over. With the sequencer stopped a selection applies
  at once.

## 0.12.0 -- 2026-09-21

- **Front panel in the machine's order.** VCF holds Cutoff, Resonance, Env Mod,
  Decay and Accent. Tracking, Overdrive and Filter FM moved to a new VCF MOD
  panel in the collapsible half.
- Accent At, Sweep Time, Sweep Speed and Accent Hold joined the MODS panel; the
  ACCENT panel is gone. The collapsible half is VCF MOD, SLIDE, VIBRATO and AMP
  on one row with MODS below.
- **RESET** on the preset bar returns every parameter to its default (the stock
  circuit). Arms on the first click, acts on the second. Pattern, Chain Mode,
  Chain Length, Mode and Rate are kept; pattern contents are never touched.
- Rows in the window no longer stretch their last panel to the right margin.

## 0.11.0 -- 2026-09-21

- Three new factory presets built around the pedal-derived drive models:
  **Stompbox** (Germanium), **Dig In** (Crunch) and **Halo** (Lead). The
  library grows from 27 to 30 presets.

## 0.10.0 -- 2026-09-21

- **Two new drive models, Crunch and Lead**: the two channels of the BOSS SD-2
  Dual OverDrive, modelled from its service notes. Crunch is one LED-clipped
  stage that stays clean until hit; Lead is three gain stages with two
  clippers, the most frequency-selective model in the set.
- **Standard MIDI File import** (format 0 or 1), one file or a whole folder,
  from the browser's IMPORT menu ("Pattern file..." / "Pattern folder...").
  Quantised to sixteenths, one note per step; accents, slides and vibrato are
  read back with the same conventions the MIDI export writes. A file that
  repeats exactly is reduced to its shortest period.
- Single-file import for `.pat` files as well; a file lands on the shelf its
  folder would have made.
- Fixed: a division by zero in the drive stage at the bottom of Drive (Lead).
- Fixed: asymmetric diode pairs no longer leave the clipper's node off zero at
  rest.

## 0.9.0 -- 2026-09-21

- **New drive model, Germanium**, modelled component by component from the MXR
  Distortion+ schematic. Its gain rises with frequency (a shelf plus the op-amp's
  bandwidth limit, giving a mid hump near 1.5 kHz), so it stays tight on low
  notes. The diode clipper is solved per sample rather than applied as a curve.
  Bias adds a second diode on one side.

## 0.8.0 -- 2026-09-21

- **Import patterns from AudioRealism Bassline**: IMPORT > "Pattern folder..."
  turns every pattern file under a directory into a preset, one shelf per
  source folder. Reads ABL2 text, ABL3 text and Reason JukeboxPatch XML,
  detected per file from content. A `.param` file beside a `.pat` supplies the
  full front panel. Multi-pattern files fill several bank slots; out-of-range
  lines are moved with Pattern Oct.
- Manual: the control tables in chapter 3 were corrected against the parameter
  table (Steps, Mode, Cutoff, Decay, Tracking, Slide Time, Res Range), the
  factory library lists all presets, and a generated appendix listing every
  parameter's range and default was added.

## 0.7.1 -- 2026-09-20

- Fixed: pressing GEN crashed the host (division by zero) since the seed range
  was widened in 0.7.0.

## 0.7.0 -- 2026-09-20

- **Long notes.** Dragging along a piano-roll row writes a tied run, drawn as
  one bar. Removing a slide in the middle splits it; restoring it joins it.
  No format change.
- **Delay**, last in the chain: free or tempo-synced, mono, stereo or
  ping-pong, mid/side width, feedback up to 130 % (saturates rather than
  overflowing). Off by default.
- **Live mode**, a third value of Mode: keys select or step patterns instead of
  transposing. Pattern Oct shifts the running pattern by octaves. The pad map
  is learned from the window and stored in the state, not in presets (state
  version 3).
- The generator seed spans a full 32-bit word (a Unix timestamp works) and can
  be typed.
- Manual rewritten for end users, with a contents page and 24 screenshots.
- Fixed: stepped parameter values above 2^31 were displayed wrongly.

## 0.6.0 -- 2026-09-20

- **Drive stage rebuilt from the literature**: seven models -- Soft Clip,
  Overdrive, Tube, Valve Stack, Fuzz, Rectifier, Crush -- each an equation from
  Zoelzer's *DAFX* or Pirkle's *Designing Audio Effect Plugins in C++*. They
  replace the fourteen shapes of 0.5.0, most of which sounded the same. Each has
  its own gain staging, runs at 2x oversampling and is loudness-matched on a
  real line.
- New **Bias** control shifts each model's operating point for even harmonics.
- Soft Clip is unchanged, so every existing preset renders as before.

## 0.5.0 -- 2026-09-20

Not released on its own; shipped together with 0.6.0.

- **Patterns up to 128 steps** (was 16). The generator fills the set length and
  extends a seed's line rather than replacing it. The grid shows 16 or 32
  columns and scrolls. State version 2 reads older banks intact.
- **Preset folders and packs.** The browser groups presets by folder; saving as
  "Folder/Name" creates one. A folder exports as a single pack file and imports
  back as a new folder without overwriting anything.
- Drive stage with fourteen shapes and a Dist Mix control (replaced in 0.6.0;
  Dist Mix stays).
- **DEL** in the pattern bank empties the selected slot.

## 0.4.0 -- 2026-09-19

- Fixed: two instances shared one pattern bank and pattern selection once both
  editors had been opened.
- Fixed: GEN regenerated the same pattern on every press; it now picks a new
  seed each time. The seed is still a parameter, so a pattern stays
  reproducible.
- Fixed: pressing a key with the host transport stopped did not always restart
  the sequencer from step one.

## 0.3.1 -- 2026-09-18

- The octave lane draws up in green and down in amber, so the two directions
  are distinguishable at two octaves.

## 0.3.0 -- 2026-09-18

- **Note output port**: everything the sequencer plays, transposition included,
  goes out as MIDI for recording on another track.
- **MIDI drag**: the MIDI button drags the selected pattern into the host as a
  Standard MIDI File (X11 and Windows).
- **COPY / PASTE** between pattern slots.
- A step reaches **two octaves** up or down (was one). Older states still load.
- A key press restarts the free-running pattern from step one.
- Fixed: changing Mode (e.g. by browsing presets) while a key was held left a
  stuck note that kept the pattern running.
- Fixed: reset did not clear held transpose keys; taking the keyboard for the
  save field now drops them too.
- Fixed: the tempo is read from a stopped transport as well, not only while
  playing.

## 0.2.0 -- 2026-09-18

- **Windows and VST3 builds**: CLAP and VST3 for Linux and Windows.
- **Pattern bank**: 64 patterns in an 8x8 grid, with Pattern, Chain (Stay,
  Next, First, Random) and Chain Length as automatable parameters. The playing
  pattern follows the host's beat position. The bank is saved in the state and
  in presets.
- **Devil Fish controls**, after Robin Whittle's modification: Overdrive, Filter
  FM, Muffler, Soft Attack, Amp Decay, Amp Sustain, accent Sweep Speeds and
  Accent Hold; wider ranges for Cutoff, Decay, Slide Time, Res Range and
  Tracking. All default to the stock circuit.
- **Collapsible ADVANCED section** holding the mods and the accent, slide and
  vibrato panels. The activity and peak meters are removed.
- New **Blank Slate** preset. Loading a preset now resets parameters it does not
  mention to their defaults.
- Vib Depth maximum doubled to 200 cents.
- Renamed from ThreeOhThree to SäureKiste.
- Fixed: Windows window-class registration, which could let two plugins in one
  process corrupt each other.

## 0.1.0 -- 2026-09-18

Initial version, as ThreeOhThree. Linux, CLAP only.

- TB-303 model from the 1982 service notes: VCO, four-stage transistor ladder,
  VCA, decay envelope, accent circuit and slide, plus a drive stage.
- MIDI mode (velocity accents, overlap slides, CC1 vibrato) and a 16-step
  sequencer locked to the host's beat timeline, transposed by held keys.
- Ten mods exposing the engine's unspecified constants as controls.
- Pattern generator: seed, scale, root and five densities.
- 40 parameters, 26 factory presets, deterministic rendering.
