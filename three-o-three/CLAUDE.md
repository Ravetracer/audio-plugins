# ThreeOhThree

A monophonic acid bass synthesiser: a model of the Roland TB-303's main board,
taken from the **February 1982 service notes**, as a native CLAP plugin.

Read the repository root `CLAUDE.md` first — it covers the shared library, the
build, the release tooling and the conventions every plugin here follows. This
file is only what is specific to this one.

Read `STATUS.md` and `TODO.md` before starting work. They are the current source
of truth for what works and what does not.

## What it is, in one paragraph

Linux and Windows, CLAP and VST3, 51 parameters, 27 presets. VCO → four-stage transistor
ladder → VCA, with the decay envelope, the accent circuit and the slide lag
around it, and an overdrive stage after all of it. It plays either from the host
over MIDI or from its own sixteen-step sequencer locked to the host transport,
which draws on a bank of sixty-four patterns.

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
three-o-three/
├── src/
│   ├── pattern.{h,cpp}      the bank of sixty-four sixteen-step patterns:
│   │                        steps, gates, slides, accents, the chain and the
│   │                        seeded generator
│   ├── gui/seqwindow.{h,cpp} the sequencer's piano-roll window
│   └── dsp/acid_engine.{h,cpp}  the circuit
├── demos/                   plain WAV renders — GITIGNORED
└── !dev/                    the Roland service-note PDFs — GITIGNORED
```

`install.sh` is this plugin's own, not the thin wrapper over
`shared/tools/install-plugin.sh` that a suite plugin uses. That script derives
tool names and an environment variable from the folder name, and
`three-o-three` is not a usable shell identifier.

## Things that are easy to get wrong

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
  what the machine's own keyboard did.
- **Widening a logarithmic parameter's range breaks every saved project unless
  it is migrated.** A state blob stores the *raw* value, which for a Log
  parameter is a position on its own curve -- so changing dispMin/dispMax
  silently moves it. `migrateRanges()` in `plugin.cpp` converts Cutoff, Decay
  and Slide Time out of the pre-Devil-Fish ranges, state version 4 is what
  triggers it, and the self-test builds a version 3 blob by hand to check it.
  Tracking is not in that list and must not be: its maximum grew but its kind
  did not, so its stored value still means the same thing. Preset files are
  safe either way -- they are written in real units.
- **The version lives in two places** — `project(... VERSION)` in
  `CMakeLists.txt` and `kPluginVersion` in `src/threeohthree.h`. A
  `static_assert` fails the build when they disagree. Bump both.

## Build

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
./install.sh --no-selftest
```

Four binaries: CLAP and VST3, Linux and Windows. `./release.sh 0.1.0` at the
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
