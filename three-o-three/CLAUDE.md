# ThreeOhThree

A monophonic acid bass synthesiser: a model of the Roland TB-303's main board,
taken from the **February 1982 service notes**, as a native CLAP plugin.

Read the repository root `CLAUDE.md` first — it covers the shared library, the
build, the release tooling and the conventions every plugin here follows. This
file is only what is specific to this one.

Read `STATUS.md` and `TODO.md` before starting work. They are the current source
of truth for what works and what does not.

## What it is, in one paragraph

Linux-only, CLAP-only, 40 parameters, 26 presets. VCO → four-stage transistor
ladder → VCA, with the decay envelope, the accent circuit and the slide lag
around it, and an overdrive stage after all of it. It plays either from the host
over MIDI or from its own sixteen-step sequencer locked to the host transport.

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
- **It has a second window.** `src/gui/seqwindow.{h,cpp}` is the step sequencer's
  own window, built on the shared toolkit but not on the shared layout engine.
  Forking `shared/src/gui/window.cpp` is not allowed; adding a window beside it
  is.

## Layout, beyond the standard skeleton

```
three-o-three/
├── src/
│   ├── pattern.{h,cpp}      the sixteen-step pattern: steps, gates, slides,
│   │                        accents, and the seeded generator
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
  parameters for sixteen steps would wreck the host's parameter list. The
  consequence is that pattern edits are not automatable and the host's undo does
  not see them — a hardware sequencer offers the same deal. Do not "fix" this by
  moving them into the parameter table.
- **The sequencer is re-read from the host's beat timeline every block**, not
  advanced by a counter, so scrubbing, looping and tempo changes all land. Notes
  are sample-accurate: the audio block is split again at every step boundary.
- **A held MIDI note transposes the pattern rather than sounding**, which is
  what the machine's own keyboard did.
- **The version lives in two places** — `project(... VERSION)` in
  `CMakeLists.txt` and `kPluginVersion` in `src/threeohthree.h`. A
  `static_assert` fails the build when they disagree. Bump both.

## Build, and why it refuses to cross-build

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
./install.sh --no-selftest
```

`CMakeLists.txt` hard-fails on any platform but Linux. Nothing in the plugin is
Linux-specific beyond the window, so that is a statement about what has been
*tested*, not what is possible.

**Windows and VST3 builds are planned.** The repository already carries
everything they need — `setup-winbuild.sh`, the mingw toolchain file, the Cairo
cross-build, the clap-wrapper patch, and the complete win32 window in
`shared/src/gui/window.cpp`. Turning them on is a block of CMake copied from a
Verdalis plugin plus a test pass. When that happens, read the root `CLAUDE.md`
sections on the Cairo `-Db_ndebug=true` flag and the per-module window class
registration first; both are traps that have already cost someone a DAW session
once.

## Trademarks

Not affiliated with or endorsed by Roland Corporation. *TB-303* is their
trademark and is used only to name what was modelled. That notice belongs in the
README and the manual and must stay there.

The service-note PDFs in `!dev/` are Roland's and are **not ours to
redistribute**. They are gitignored. Never commit them, never ship them, and
never `git add -A` without checking.

## Still open

See `TODO.md` for the full list. The headline items:

- No VST3 and no Windows build yet (above).
- No `presets/demo-descriptions.txt`, so the website demo pipeline
  (`shared/tools/make-demos.sh`) has no musician-facing blurbs to use and would
  fall back to the presets' own technical descriptions.
- `demos/` holds plain WAV renders rather than the MP3 set the shared demo
  script produces.
