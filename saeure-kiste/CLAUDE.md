# SäureKiste

A monophonic acid bass synthesiser: a model of the Roland TB-303's main board,
taken from the **February 1982 service notes**, as a native CLAP plugin.

Read the repository root `CLAUDE.md` first — it covers the shared library, the
build, the release tooling and the conventions every plugin here follows. This
file is only what is specific to this one.

Read `STATUS.md` and `TODO.md` before starting work. They are the current source
of truth for what works and what does not.

## What it is, in one paragraph

Linux and Windows, CLAP and VST3, 54 parameters, 27 presets. VCO → four-stage transistor
ladder → VCA, with the decay envelope, the accent circuit and the slide lag
around it, and a drive stage of seven models after all of it. It plays either
from the host over MIDI or from its own sequencer locked to the host transport,
which runs 1 to 128 steps and draws on a bank of sixty-four patterns.

## The third source

Everything in front of the drive stage comes from the service notes. The stage
itself has no schematic, so it comes from the literature instead: `dsp/drive.h`
and `dsp/drive.cpp` implement equations from Zölzer's *DAFX* chapter 4 and
Pirkle's *Designing Audio Effect Plugins in C++* chapter 19, with the citation
written beside every one of them.

**The books live in `Documents/` and are gitignored**, exactly like the service
notes in `!dev/`: they are copyrighted and are not ours to redistribute. The
equations, implemented and attributed, are ours to ship; the PDFs are not.

## The second machine it models

The plugin models **Robin Whittle's Devil Fish** modification as well as the
stock board. That is a different kind of source: Whittle publishes a manual
describing what each addition does, in numbers, rather than a schematic. It is
still a source, and the rule below still applies -- where he gives a number it
is fixed, and where he describes a behaviour without one (the Fast and Slow
sweep speeds, how far Filter FM moves the cutoff) the shape is fitted to the
description and the knobs around it stay controls.

**Every Devil Fish control defaults to the stock circuit**, from Whittle's own
"Limiting the Devil Fish to TB-303 sounds" table. That is what lets the whole
preset library be diffed byte for byte against the build from before any of it
existed, which is the only real proof the modification broke nothing. Do not
change one of those defaults.

His manuals are in `!dev/` and are **his, not ours to redistribute** -- exactly
the same rule as the Roland service notes. Gitignored, never committed.

Trademarks: not affiliated with or endorsed by Robin Whittle, and *Devil Fish*
is used only to name what was modelled. The controls are named for what they do,
which happens to be Whittle's own words for them; there is no Devil Fish
branding on the panel and there should not be.

## The one rule that governs this plugin

**Everything that can be read off the schematic is fixed; everything that cannot
is a control, not a hidden guess.**

That is the whole design. The service notes give component values, so the
ladder's unequal input capacitor, the accent circuit's 68 ms time constant and
the envelope-modulation bias are all fixed by the circuit. The constants the
schematic does *not* give — the two Env Mod magnitudes, the three accent
numbers, the accent decay the hardware forces, the square wave's droop and the
rest — became the **ten mods**, exposed as parameters.

This was the author's explicit instruction and it overrides an instinct to chase
exact hardware accuracy: a tweakable knob beats a guessed constant baked into
the engine. When a new unknown appears, expose it.

## What makes it different from a Verdalis plugin

Both are built on the same window and parameter model, so it looks like one.
Three things are not the same, and all three matter when changing it:

- **It is deterministic.** The engine holds no random state at all, so the same
  preset at the same sample rate renders bit-identical samples every time, and
  the self-test checks it. The nature instruments the shared code came from are
  deliberately stochastic and need `randomseed=N` pinned to compare renders.
  This one does not, which makes it a good canary for a `shared/` refactor.
- **It models a machine, not a phenomenon.** There is no "measured, not guessed"
  fitting against reference recordings here; the reference is a schematic.
- **Its window is a fork, not a second window.** `src/gui/seqwindow.{h,cpp}` is
  `shared/src/gui/window.cpp` copied, with the namespace changed and three
  things added: the step grid, the pattern bank and the collapsible panel
  section. It replaces the shared window here rather than sitting beside it. A
  step grid is not expressible as panels of knobs, and this plugin does not get
  to change what every other window in the repository looks like in order to
  have one. Diff the two files to see exactly what was added — and port a fix to
  the shared window by hand, because nothing does it automatically.

## Layout, beyond the standard skeleton

```
saeure-kiste/
├── src/
│   ├── pattern.{h,cpp}      the bank of sixty-four sixteen-step patterns:
│   │                        steps, gates, slides, accents, the chain and the
│   │                        seeded generator
│   ├── midifile.{h,cpp}     a pattern as a standard MIDI file, for the drag
│   ├── gui/dragfile.{h,cpp} the desktop drag itself: XDND on X11, OLE on win32
│   ├── gui/seqwindow.{h,cpp} the sequencer's piano-roll window
│   └── dsp/acid_engine.{h,cpp}  the circuit
├── demos/                   plain WAV renders — GITIGNORED
└── !dev/                    the Roland service-note PDFs — GITIGNORED
```

`install.sh` is this plugin's own, not the thin wrapper over
`shared/tools/install-plugin.sh` that a suite plugin uses. That script derives
tool names and an environment variable from the folder name, and
`saeure-kiste` is not a usable shell identifier.

## Things that are easy to get wrong

- **The octave is split across two bit fields, on purpose.** A step packs into
  sixteen bits; the octave's *direction* is in bits 4-5 where it always was and
  its *distance* is in bit 9. Widening the old field in place would have made
  every state blob written before 0.3.0 unreadable; split, a 0.2.x build reads a
  new blob as the same step one octave less extreme. Do not tidy this up.
- **A slid note must not retrigger the envelope.** On the machine the gate never
  goes low across a slide, so the envelope keeps running. This is true for both
  play modes — an overlapping MIDI note and a slid sequencer step. It is the
  single most audible behaviour of the instrument and the easiest to break.
- **Pattern edits are not parameters, and that is deliberate.** Eighty
  parameters for sixteen steps would wreck the host's parameter list, and the
  bank makes it five thousand. The consequence is that pattern edits are not
  automatable and the host's undo does not see them — a hardware sequencer offers
  the same deal. Do not "fix" this by moving them into the parameter table.
  *Which* pattern is selected, the chain mode and the chain length are
  parameters, because those are the things an arrangement automates.
- **The chain is derived from the beat position, never counted.**
  `chainPatternAt(mode, selected, length, cycle)` in `params.cpp` takes the
  cycle — how many times round the pattern the host's timeline is — and returns
  the pattern index, Random included. This is the same rule as the steps, and for
  the same reason: a counter bumped at the end of each pattern comes apart the
  first time somebody loops a bar.
- **A preset load starts from the defaults.** `applyPreset()` resets every
  parameter before applying what the file says, so a preset written before a
  parameter existed does not leave that parameter holding a stale value. The
  factory presets written before the bank say nothing about it; without this,
  loading one while pattern 12 was selected left the sequencer on a pattern the
  preset had just emptied.
- **The window has two heights.** The ADVANCED button on the preset bar opens the
  collapsible section; the window lays itself out, resizes its own X11 window,
  and then asks the host through `clap_host_gui::request_resize`. A host that
  refuses leaves the editor clipped, which is visible and therefore reportable —
  a window that never relaid itself would be wrong and look fine. Whether it is
  open lives in the plugin (`mAdvancedOpen`, in the state blob at version 3), not
  in the window, so it survives closing the editor; `syncAdvanced()` in the
  window picks up a change the host made under it.
- **The sequencer is re-read from the host's beat timeline every block**, not
  advanced by a counter, so scrubbing, looping and tempo changes all land. Notes
  are sample-accurate: the audio block is split again at every step boundary.
- **A held MIDI note transposes the pattern rather than sounding**, which is
  what the machine's own keyboard did. With no transport running it also starts
  the pattern again from step one; with one, it does not, because there the
  position belongs to the song.
- **The sequencer's notes go out of a note port as well as into the engine**,
  and `src/midifile.cpp` writes the same notes to a file for the window to drag
  out. Three conventions are shared by both and by the plugin's own MIDI input:
  an accent is a velocity above the accent threshold, a slide is an overlap, a
  vibrato is CC1. Change one of them and change all three.
- **A drag runs a nested event loop and the window stops painting until it ends.**
  That is what both platforms offer -- `DoDragDrop` is modal, and the X11 source
  has to answer the target while the button is down -- and it is what every
  other application does.
- **The parameter table is indexed by id, so a new row goes at the *end* of it.**
  `paramTable()[kParamRandSeed]` and `mSpec.params[id]` are both positional
  lookups: the table's order has to match the `ParamId` enum exactly. A new
  parameter is appended to the enum, because ids are persisted in presets and
  state -- so its row goes at the bottom of the table too, whatever module it
  belongs to. Putting the two drive-stage rows next to Drive, where they read
  better, made every parameter past them resolve to the wrong entry and
  segfaulted the self-test on the first run. The `mod` field is what groups a
  parameter in the preset file and the manual; the position in the table is
  not.
- **A set of waveshapers is not a set of distortions.** 0.5.0 shipped fourteen
  and most of them were inaudible, for three reasons worth remembering: they
  shared one pre-gain mapping, so they all saturated at the same knob position
  and any saturated clipper is the same square wave; the level matching divided
  out how much each curve compressed, which is the difference; and none of them
  had filtering of its own. What separates the seven models in `dsp/drive.h` is
  *structure* -- a linear region that survives, an operating point off centre,
  four stages in series with a tone stack between them, a rectifier, a
  quantiser -- and each one's own gain staging over the range its own source
  gives it.
- **And the test for it has to measure sound, not samples.** The check that let
  the fourteen through was "no two types render the same audio", comparing
  sample buffers: two signals can differ in every sample and be
  indistinguishable. `render --selftest` now searches each model's Drive for the
  setting that gives 25 % THD and compares harmonic distributions *there*,
  because any two clippers meet at the top of the knob. A new model that
  duplicates an existing one fails the build.
- **The drive stage's level matching measures the shape, so anything applied
  inside the shape is measured away.** `setParams()` walks each shape from
  silence up to 0.8 and divides by what it finds. A per-model trim put inside
  the shape is therefore measured and scaled straight back out -- the first
  attempt at giving Metal headroom did not move the output by a single sample.
  The trims are applied in `DriveStage::process()`, after the matching, for
  exactly that reason, and the Valve Stack's goes *inside* its own output stage
  on purpose so that stage can bound it.
- **Loudness and peak are different jobs.** Matching peaks keeps the plugin in
  bounds; it does not make two models the same loudness, because a model that
  squashes one half of the wave or thins the bottom of it has the same peak and
  much less signal under it. Measured on a bass line the models sat 14 dB apart
  that way. They are matched on RMS now, with the peak as a ceiling, plus one
  measured trim each -- see the note in `drive.cpp`.
- **Widening a logarithmic parameter's range breaks every saved project unless
  it is migrated.** A state blob stores the *raw* value, which for a Log
  parameter is a position on its own curve -- so changing dispMin/dispMax
  silently moves it. There is no migration path in the code now: the rename
  changed the state magic, so nothing older can be read at all and the format
  restarted at version 1. The next range change will need one written, and a
  self-test that builds an old blob by hand to prove it works.
- **Nothing per-instance may live at file scope.** A DAW loads the binary once
  and instantiates it per track, so a global, a namespace-scope variable or a
  function-local `static` is shared by every instance in the project. This has
  already cost one bug: `createGui()` kept its `WindowSpec` in a `static` and
  the window held a *reference* to it, so a second instance's editor redirected
  the first one's at the second plugin's pattern bank -- two tracks sharing one
  bank and one pattern selection. The window now copies the spec, the spec is a
  local, and the header ornament is allocated per window. The check is
  mechanical: `nm -C --defined-only build/SaeureKiste.clap | grep " [bB] "`
  should turn up nothing but lookup tables and the module-level preset
  discovery, which is the same for every instance by definition.
  `tools/check-instances.sh` proves it from the outside, with two editors open.

- **The version lives in two places** — `project(... VERSION)` in
  `CMakeLists.txt` and `kPluginVersion` in `src/saeurekiste.h`. A
  `static_assert` fails the build when they disagree. Bump both.

## Build

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
./install.sh --no-selftest
```

Four binaries: CLAP and VST3, Linux and Windows. `./release.sh 0.2.0` at the
repository root builds all of them and packs the archives with the manual.

Windows is cross-compiled with mingw-w64 against a Cairo built by
`./setup-winbuild.sh` (once). The VST3 is the same plugin behind free-audio's
clap-wrapper, which needs `CLAP/clap-wrapper` and `CLAP/vst3sdk` checked out and
`shared/patches/clap-wrapper-vst3-sdk-3.8.patch` applied.

**The Windows CMake blocks are the Verdalis ones, adapted.** That is what the
root `CLAUDE.md` says to do and it is worth doing: writing them from scratch
gets you something that builds while quietly missing `--exclude-all-symbols`,
the trailing `-Bdynamic` after a `--whole-archive` group, and the graceful
degrade that `release.sh --windows-no-gui` depends on. Compare against
`../../Verdalis/<plugin>/CMakeLists.txt` before changing them.

**Nobody has loaded the Windows build in a DAW.** It compiles and it imports
only system DLLs. That is not the same as working.

## Trademarks

Not affiliated with or endorsed by Roland Corporation. *TB-303* is their
trademark and is used only to name what was modelled. That notice belongs in the
README and the manual and must stay there.

The service-note PDFs in `!dev/` are Roland's and are **not ours to
redistribute**. They are gitignored. Never commit them, never ship them, and
never `git add -A` without checking.

## Still open

See `TODO.md` for the full list. The headline items:

- The Windows binaries have never been run in a DAW (above).
- No `presets/demo-descriptions.txt`, so the website demo pipeline
  (`shared/tools/make-demos.sh`) has no musician-facing blurbs to use and would
  fall back to the presets' own technical descriptions.
- `demos/` holds plain WAV renders rather than the MP3 set the shared demo
  script produces.
