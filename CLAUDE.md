# Audio Plugins

A collection of native **CLAP** audio plugins. Unlike a suite, these have no
shared theme: each one is whatever it needs to be. A TB-303 model, a utility, an
effect, an experiment — the only things they have in common are the foundation
they are built on and the way they are laid out.

This directory is the root. All work on any plugin starts here, and every new
plugin gets its own subfolder alongside the existing ones.

```
https://github.com/Ravetracer/audio-plugins
```

## What this is not

This is **not** the [Verdalis](https://github.com/Ravetracer/Verdalis) suite, and
the two are separate projects with separate repositories. Verdalis is nine
plugins modelling natural sound phenomena from first principles, bound by a rule
that nothing is sampled and everything is synthesised. That rule does not apply
here. A plugin in this repository may model a machine, wrap a table, or do
whatever the job needs.

**The shared library is the one thing the two projects have in common.** This
repository's `shared/` began as a copy of Verdalis's, renamed from the
`verdalis` namespace to `plugincore`. The two are now independent copies that
will drift apart. There is no submodule, no symlink and no build-time
dependency between the repositories — see *The shared library* below for what
that means when a bug is found in one of them.

## Layout

```
audio-plugins/
├── README.md          the GitHub front page
├── CLAUDE.md          this file
├── LICENSE            MIT, repository-wide
├── .gitignore
├── release.sh         builds every plugin and packs the archives
├── setup-winbuild.sh  one-time Windows cross-build setup
├── shared/            the PluginCore library every plugin is built on
├── CLAP/              CLAP SDK checkouts       — GITIGNORED, see below
├── winbuild/          meson venv + Windows Cairo — GITIGNORED
├── dist/              release archives         — GITIGNORED
├── Documents/         reference books and presets — GITIGNORED, see below
├── saeure-kiste/     SaeureKiste — a TB-303 model
├── rumpel-kiste/     RumpelKiste — a TR-909 model
├── aurum/            Aurum — an algorithmic reverb, NOT built on shared/
└── substrike/        Substrike — a kick drum designer, NOT built on shared/
```

## The plugins

| # | Plugin | Folder | Status | Platforms | Formats | What it is |
|---|--------|--------|--------|-----------|---------|------------|
| 1 | **SäureKiste** | `saeure-kiste/` | 0.15.2 | Linux, Windows | CLAP, VST3 | a Roland TB-303 model, from the 1982 service notes, plus Robin Whittle's Devil Fish modification |
| 2 | **RumpelKiste** | `rumpel-kiste/` | 0.2.1 | Linux, Windows | CLAP, VST3 | a Roland TR-909 model, from the 1984 service notes, with SäureKiste's sequencer and drive stage |
| 3 | **Aurum** | `aurum/` | 0.3.1 | Linux, Windows | CLAP, VST3 | a clean-room algorithmic reverb (FDN, allpass ring, plate) with a per-frequency decay contour; own DSP and GUI, see *Aurum* below |
| 4 | **Substrike** | `substrike/` | 1.1.0 | Linux, Windows | CLAP, VST3 | a layered kick drum designer for every style; standalone like Aurum, see *Substrike* below |

Naming follows the plugin, not a pattern: the CMake project, the installed
artifact and the display name are CamelCase (`SaeureKiste`), the folder is
lowercase (`saeure-kiste` — hyphens are fine, it is a folder name), and the
preset extension is the CamelCase name lowercased and joined (`.saeurekiste`).

Note that the folder and the preset extension are **not** required to match, and
for SäureKiste they do not. That is deliberate and it has one consequence
worth knowing: `shared/tools/install-plugin.sh` derives tool names and an
environment variable from the folder name, and `saeure-kiste` is not a usable
shell identifier. SäureKiste therefore carries its own `install.sh` rather
than the thin wrapper the Verdalis plugins use. A new plugin with a
shell-safe folder name can use the shared script.

## The shared library

`shared/` holds the code every plugin here is built on, compiled as the static
library `plugincore::shared` and reached through `<plugincore/...>` includes:

```
shared/
├── CMakeLists.txt              builds plugincore::shared
├── include/plugincore/
│   ├── params.h                ParamDesc, ParamKind, FilterKind, conversions
│   ├── param_macros.h          table-building shorthand (params.cpp only)
│   ├── preset.h                PresetContext, PresetData, the text format,
│   │                            user folders and the preset pack format
│   ├── preset_provider.h       PresetProviderSpec, the discovery factory
│   ├── dsp/{adsr,biquad,bubble,denormals,fastmath,filters,pocket,reverb,rng}.h
│   └── gui/
│       ├── gui.h               Gui + GuiDelegate, the plugin/window contract
│       ├── toolkit.h           Cairo drawing primitives, Rect, Align
│       └── window.h            Theme, PanelSpec, HeaderOrnament, MixerStrip,
│                                WindowSpec
├── src/{params,preset,preset_provider}.cpp
├── src/gui/window.cpp          the window: layout, widgets, browser, entry
├── cmake/                      embed_presets, mingw toolchain, Windows Cairo,
│                               clap_entry.version
├── patches/                    fixes for the gitignored CLAP/ checkouts
└── tools/                      install-plugin.sh, fithost.cpp, analysis/wavio.py,
                                make-demos.sh, and the manual toolchain:
                                make-manual.sh, docgen.cpp, manual.py, manual.css
```

A plugin pulls it in with

```cmake
add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)
target_link_libraries(<target> PRIVATE plugincore::shared)
```

and each plugin's `params.h` re-exports the shared names into its own namespace
with a scoped `using namespace plugincore;`, so plugin code calls `paramToReal`,
`Svf`, `parsePreset` and so on unqualified.

**How the shared code stays plugin-agnostic.** It never hardcodes a name. The
preset code takes a `PresetContext` (plugin name, file extension, parameter
table and its size); the discovery provider takes a `PresetProviderSpec`. The
preset **pack** format -- a folder of presets as one text file, added in
SäureKiste 0.5.0 -- is the same: `formatPresetPack`/`parsePresetPack` take a
context and carry each preset's *text* rather than a re-serialised copy, so a
preset with lines the shared format knows nothing about survives the round
trip. `GuiPreset::folder` and the folder and pack calls on `GuiDelegate` all
default to "this plugin does not do that", so a plugin that keeps a flat preset
directory gets the browser it always had; the browser UI that uses them is in
SäureKiste's forked window and has not been ported to the shared one.
`embed_presets.cmake` requires `NAMESPACE` and `PRESET_EXT` as arguments and
fails without them, because a default would silently generate the wrong
namespace and still compile in the plugin it was copied from.

Because `plugincore::shared` is a **static** library linked into each plugin
separately, file-scope state in shared code belongs to one plugin binary. That
is what makes the single preset-discovery provider safe.

### Keeping it in step with Verdalis

The two copies are independent and will diverge, but a bug found in one is a bug
in the other until proven otherwise. A fix in either repository's `shared/` is
worth checking against the other. The names differ by exactly two substitutions:

```
verdalis   -> plugincore        (namespace, include path, CMake target)
VERDALIS_  -> PLUGINCORE_       (CMake cache and environment variables)
```

so porting a change is a diff plus `sed`, not a rewrite. The one already-known
case is worth recording because it cost a user their DAW session:

> **The Windows window class must be registered per module.** A window class is
> keyed on `(HINSTANCE, name)`. Registering under `GetModuleHandle(nullptr)`
> keys it on the *host executable*, so every plugin binary in the process
> competes for one name; the loser gets `ERROR_CLASS_ALREADY_EXISTS` and then
> creates its windows against the winner's class — running another binary's
> `wndProc`, and therefore another binary's statically linked Cairo, over
> surfaces this one's Cairo allocated. Two copies of Cairo with separate global
> state corrupt each other. `shared/src/gui/window.cpp` uses the module's own
> `HINSTANCE` and a class name suffixed with it, and unregisters the class with
> the last window so an unloaded plugin cannot leave a dangling `wndProc`. X11
> has no class registry, so this can only ever bite on Windows.
> (Verdalis issue #1.)

The fourth case is the manual toolchain, changed here in 0.7.0 and **not yet
ported**. Four things, and the first two are bugs every Verdalis manual has:

- `manual.py` printed the plugin's name as a letter-spaced wordmark when no
  collection logo was found, *and* again as the cover title underneath it --
  so every manual in both repositories has the name on the cover twice. The
  fallback is gone; without a logo the cover starts at the name.
- `make-manual.sh` now reads `kPluginName` out of `src/<ns>.h` and passes it as
  `--display-name`, so the cover, the HTML title and the PDF metadata can say
  something the file system cannot. SaeureKiste is the project, the binary and
  the preset directory; SäureKiste is the instrument. Verdalis has no plugin
  whose two names differ, so there it is a no-op that costs nothing and will be
  there when one does.
- A heading alone at the foot of a page. The stylesheet has
  `page-break-after: avoid` on h1-h4 and wkhtmltopdf's WebKit ignores it; what
  that build *does* honour is `page-break-inside: avoid` on a block, so
  `manual.py` now wraps each heading and the block under it in one. A table
  deeper than ten rows is left out of the pairing, or the fix opens the hole it
  was closing.
- Markdown images are resolved against the manual source's folder and inlined
  as data URIs, and an image alone in a paragraph becomes a `<figure>` with its
  alt text as the caption. That is what lets a manual carry screenshots and
  still be one self-contained file. See `saeure-kiste/tools/make-screenshots.sh`
  for how the images themselves are made; nothing about it is plugin-specific
  except the coordinates.

The third case is a one-line formatting bug found here in SäureKiste 0.7.0 and
**not yet ported**: `paramValueToText` printed a `ParamKind::Stepped` value
with `%d` through an `int`. That is fine for every range Verdalis has and wrong
for any range past 2^31 -- SäureKiste's generator seed now spans a whole 32-bit
word so a Unix timestamp can be typed into it, and the cast overflowed, printing
"-2147483648" for a value near the top and failing to read back. It is now
`%lld` through a `long long`. Nothing in Verdalis has a stepped parameter that
wide, so this is a latent bug there rather than a live one; port it anyway,
because the next wide range will not announce itself.

The second case is the same shape and is **unfixed in Verdalis as of this
writing**: the window held its `WindowSpec` by reference while every plugin's
`createGui()` kept that spec in a function-local `static`. One spec per binary,
so opening a second instance's editor rewrote the pointers the first one's
window was still reading -- for SäureKiste that meant two tracks sharing one
pattern bank. `shared/src/gui/window.cpp` now holds the spec **by value**, and
`WindowSpec` grew an `ownsOrnament` flag so an ornament -- which carries the
animation state of one window -- can be allocated per window and deleted with
it. Porting it is the same two substitutions plus the same edit in each
plugin's `gui.cpp`. Nothing per-instance may live at file scope in a plugin
binary; `nm -C --defined-only <plugin>.clap | grep " [bB] "` is the cheap way to
ask.

### Rule for a shared change

Anything in `shared/` is used by every plugin here. A change there must be
verified against all of them — see the verification recipe under *Working notes*.

## Plugin anatomy

Every plugin follows the same skeleton. Reproduce it when starting a new one;
the consistency is what makes the shared components possible.

```
<plugin>/
├── CMakeLists.txt           C++17, CLAP module + offline tools
├── install.sh               thin wrapper over shared/tools/install-plugin.sh,
│                            or its own script if the folder name is not a
│                            usable shell identifier
├── README.md                user-facing documentation
├── STATUS.md                what works, what is measured
├── TODO.md                  planned work
├── docs/manual.md           the manual's prose; the parameter reference and
│                            the preset library are generated into it
├── presets/                 factory presets, one file per preset, .<plugin>
├── src/
│   ├── <plugin>.h           identity constants + the preset bindings
│   ├── entry.{h,cpp}        the CLAP entry point, split so the .clap and the
│   │                        .vst3 can be built from one implementation
│   ├── plugin.cpp           CLAP host glue and the audio callback
│   ├── params.{h,cpp}       this plugin's ParamId enum and ParamDesc table
│   ├── preset.cpp           binds the shared preset format to this plugin
│   ├── preset_provider.cpp  binds the shared discovery provider
│   ├── factories.h
│   ├── dsp/                 the synthesis engine (the DSP toolbox is shared)
│   └── gui/                 its theme, panel layout and header ornament;
│                            the window itself comes from shared/
├── tools/
│   ├── render.cpp           offline renderer + self-test
│   ├── guihost.cpp          standalone GUI host for window development
│   └── analysis/            Python analysis helpers
└── !dev/                    LOCAL ONLY — reference material, papers, renders.
                             Gitignored. Never commit, never ship: this
                             material is not ours to redistribute.
```

**Not every plugin needs every part of it.** SäureKiste adds `src/pattern.*`
and `src/gui/seqwindow.*` for its sequencer, which nothing else has, and that is
the point of this repository — the skeleton is a starting shape, not a
constraint.

## Aurum

`aurum/` is the one plugin here that is **not built on `shared/`**. It has its
own DSP, its own Cairo window and widgets (X11 and win32 backends), its own
preset manager and its own parameter model. That is deliberate and stays that way: do not port it onto
`plugincore`, do not move its code into `shared/`, and do not change `shared/`
for its sake. The *Rule for a shared change* covers SäureKiste and
RumpelKiste, not Aurum — and a change in `aurum/` never needs checking against
the other plugins.

It is a clean-room algorithmic reverb, written from public literature, with a
commercial reverb's user manual (in `Documents/`) used only as a feature
reference. It was developed in a separate repository and moved here without its
history.

```
aurum/
├── CMakeLists.txt        C++20, -march=x86-64-v3 (AURUM_ARCH_FLAGS to lower it)
├── cmake/plugin.cmake    the Aurum .clap, the Aurum-vst3 wrapper target, GUI deps per platform
├── install.sh            configure, build, self-test, install to ~/.clap, ~/.vst3
├── README.md
├── docs/PLAN.md          design, phases and status — the source of truth
├── docs/manual.md        the manual's prose, docs/images/ its screenshots
├── presets/              factory presets
├── src/
│   ├── aurum.h           identity constants (kPluginId, kPluginVersion, ...)
│   ├── entry.cpp         CLAP entry, compiled into the .clap and the .vst3
│   ├── dsp/              engine: FDN (Natural), allpass ring (Classic), plate,
│   │                     early reflections, decay designer, post EQ — static lib aurum-dsp
│   ├── plugin/           CLAP glue, params, preset session (undo/redo, A/B)
│   ├── state/            state I/O, settings, presets, IR import, .ffp import
│   ├── gui/              X11Window / Win32Window (NativeWindow.h picks one),
│   │                     Cairo widgets, editor, preset browser, FileDialog
│   └── util/             SpscQueue, Path.h (UTF-8 paths for std::filesystem)
├── tests/                geq_test, engine_test, import_test, longrun_test,
│                         gui_snapshot, clap_gui_host
└── tools/                docgen.cpp, make-manual.sh, make-screenshots.sh
```

What differs from the other plugins:

- **Identity** follows the repository convention (`de.ravetracer.aurum`,
  vendor `Ravetracer`), but lives in `src/aurum.h`. The version is in
  `project()` and `kPluginVersion`, and a `static_assert` (via
  `AURUM_CMAKE_VERSION`) fails the build when they disagree. The host display
  name is "Aurum Reverb"; everything on disk is "Aurum".
- **State on disk:** presets in `$XDG_DATA_HOME/Aurum/Presets`, settings (GUI
  size, MIDI map, favourites) in `$XDG_CONFIG_HOME/Aurum/settings.ini`; on
  Windows both under `%APPDATA%\Aurum`. Factory presets are compiled in and
  written there on first run, so the `.clap` is all that gets installed. Every
  path that reaches the file system goes through `util/Path.h`: a narrow
  `std::string` is read in the ANSI code page on Windows.
- **No `STATUS.md`/`TODO.md`.** `docs/PLAN.md` carries the phases and their
  status. All eight phases are done (DSP, CLAP+VST3, GUI, presets and browser,
  undo/A-B, MIDI learn, IR import, `.ffp` import, sound tuning). Phase 8 was
  closed by ear test on 2026-10-08: the presets are close, the remaining
  differences are accepted (the reference modulates, Aurum is not meant to be
  a clone). The VST3 has only been smoke-tested.
- **Windows since 0.2.0**, with the same setup as SäureKiste: the mingw
  toolchain file, `AURUM_WIN_CAIRO` (defaults to `../winbuild/cairo-mingw`),
  `CAIRO_WIN32_STATIC_BUILD`, the runtime linked in (`aurum_static_runtime()`
  in `CMakeLists.txt`, used for every binary including the tests), and the
  per-module window class in `Win32Window.cpp`. The CLAP is `build/Aurum.clap`,
  the VST3 target `Aurum-vst3` with its bundle in `build/vst3/`, and
  `install()` puts the `.clap` in `Aurum/` -- what `release.sh` expects.
  `release.sh` finds Aurum by `src/entry.cpp`. Unlike the PluginCore plugins,
  a Windows configure **fails** without Cairo instead of building window-less,
  so `release.sh --windows-no-gui` cannot build Aurum.
- **Windows quirks worth knowing.** The UI font is Inter on Linux and Segoe UI
  on Windows (`gui/Graphics.h`): Inter is not a Windows font, and Cairo's
  DirectWrite backend renders a missing family with clipped glyphs instead of
  substituting. Cairo also does no per-glyph fallback, so the UI uses no
  symbol characters -- the favourite star is a path (`icons::star`). The
  DirectWrite backend that the toy font API pulls in exports five
  `cairo_dwrite_*` functions from the DLL beside `clap_entry`; they carry an
  explicit `dllexport` from the Cairo build, so `--exclude-all-symbols` cannot
  hide them. Harmless, and only fixable in the Cairo cross-build.
- **Tested under wine only.** Tests, `gui_snapshot` and `clap_gui_host` (which
  has a win32 path) all cross-build and run under wine; no Windows DAW has
  loaded Aurum yet.
- **The sound is calibrated against reference renders** (0.3.0). The user
  rendered a single-sample impulse (0.5 at exactly 1.000 s, 48 kHz) through
  the reference reverb for one-control sweeps (Thickness, Distance,
  Brightness, Style, a long room) and for presets in every style; they live in
  `Documents/aurum-reference/` (gitignored, never commit) with a `.ffp` beside
  every render, so each case imports exactly, sorted into `natural/`,
  `plate/`, `vintage/` (one-control sweeps and long rooms per style) and
  `presets/`; `impulse-48k.wav` stays at the top. What came out of it, all in
  `dsp/ReverbEngine.cpp` and `dsp/DecayModel.h` with the numbers in comments:
  Natural's early reflections -12.5 - 10 x Distance dB; Brightness as two
  shelves plus a level (tone) and an absorption in 1/s for darker settings
  (decay); Thickness as a level curve with only subtle saturation; wet level
  +2 dB per doubling of room time; a per-style voicing (two shelves and a
  level); per-style decay calibration as a log2 multiplier and an absorption
  per octave; no level compensation for Decay Rate EQ bands. Sweeps match
  within about 1.5 dB per octave and 10 % in decay time. In 0.3.1 Brightness
  was swept on Plate and Vintage too (`brightness_plate_*`,
  `brightness_vintage_*`, and `long_plate_b-*`/`long_vintage_-*` at -75/-100,
  those four at Mix 22.5 %): the brightness tone table is now per style, with
  a lowpass (Q 0.4) for the dark side, and `DecayModel` scales the brightness
  decay effect per style. Plate does not change its decay with Brightness at
  all; it is a lowpass whose corner falls about 1.5 octaves per 25 %. Classic
  is mostly a decay change with little tone. Both now match within about 1 dB
  per octave up to 8 kHz. Plate was also swept for Thickness, Distance and
  Decay Rate (`plate/thickness_plate_*`, `distance_plate_*`, `decay_plate_*`;
  **rendered 6.02 dB hot** -- the Bitwig track was at 0 dB instead of its
  -6 dB default -- so halve them before comparing). From those: Plate's
  level falls 3.3 dB less over the Distance range, Thickness has a small
  per-style correction, the voicing level is +0.4 dB, and Decay Rate
  lengthens the plate as rate^0.905 above 100 % with a soft floor at 0.38 x
  the room's time below. Every Plate sweep matches within about 0.5 dB in
  total level, and Dark Grotto (`plate/dark_grotto_pro_r2.wav`, a real Plate
  render) within 0.1 dB. Known and accepted, not planned work: Classic at
  -100 % cuts the 16 kHz octave almost completely, which the lowpass does
  not follow; Plate and Classic lows still decay about 25 % short in long rooms; Additive and Cloud
  Chamber II (small rooms) are 2-4 dB too loud in every style, probably the
  room-size level, which has only been measured at 2.5 s and 10 s; and
  `presets/DarkGrotto_Plate.wav` and `presets/OlanchaFarewell_Plate.wav` were
  rendered in Vintage by mistake (their early responses correlate at 1.0 with
  the Vintage files). Thickness, Distance and Decay Rate have not been swept
  on Vintage. To redo a comparison,
  render the same `.ffp` through Aurum's engine with the impulse at the same
  place and compare octave levels and octave decay times (the analysis
  scripts were scratch; rewrite them). Blank the dry sample at 1.000 s when a
  render was made below 100 % mix.

Build and test:

```sh
cd aurum
./install.sh                                   # or: cmake -S . -B build -G Ninja && ninja -C build
build/tests/geq_test                           # attenuation filter fit accuracy
build/tests/engine_test [t60|stability|cpu|wav]
build/import_test <tmpdir> ../Documents/<ffp-folder>   # IR round trip + .ffp conversion
build/gui_snapshot out.png [scale]             # offscreen render + frame timing
build/clap_gui_host build/Aurum.clap [seconds] # minimal host, under Xvfb
clap-validator validate build/Aurum.clap

# Windows, cross-built; every program above runs under wine
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=../shared/cmake/mingw-w64-x86_64.cmake
ninja -C build-win
```

Rules specific to Aurum:

- **Never name the reference product or its vendor** anywhere in the
  repository — code, comments, docs, UI, commit messages. Aurum is meant for a
  free public release. The importer is "Import .ffp Preset Folder"
  (`state/FfpImport.*`); parameter names are Aurum's own (Room, Length,
  Pre-Delay, Motion, Air, Depth, Density, Width, Ducking, Gate, Freeze, Mix;
  algorithms Natural/Classic/Plate; Decay Contour and Tone EQ). Parameter IDs
  must not change — saved state depends on them.
- Never copy code, assets, presets or visual design from the reference product.
  The look is Aurum's own: graphite and brass, signal-flow panels.
- The third-party `.ffp` presets in `Documents/` are for testing the importer
  and for the user's own use. Never bundle them as factory presets and never
  commit them.
- **Function over visuals.** No analyser, meters or decorative animation; keep
  `gui_snapshot` frame cost well under ~4 ms at 1x.
- **GUI tests must point `XDG_CONFIG_HOME` and `XDG_DATA_HOME` at scratch
  directories** and run under Xvfb. A test once wrote a MIDI mapping into the
  user's real `~/.config/Aurum/settings.ini`. Under wine the same goes for
  `WINEPREFIX`: use a scratch prefix, never the one in `winetest.env`, or the
  plugin's `%APPDATA%\Aurum` lands in the user's real Bottles prefix.

## Substrike

`substrike/` is the second plugin **not built on `shared/`**, for the same
reasons as Aurum: lanes, a reorderable effect chain and a breakpoint curve
editor do not fit `WindowSpec`. The *Rule for a shared change* does not cover
it. Code is *copied* in where it helps (SäureKiste's drive models, Aurum's
window backends) and belongs to Substrike from then on; nothing is linked
across plugin folders.

`docs/PLAN.md` is the source of truth: the design (8 lanes x 6 effect slots
plus a master chain, per-slot band select, Bus lanes and a transient guard for
rumble, per-lane aux outputs, hit export with drag-and-drop, a violet theme)
and the eight phases. Phases 1-7 are done, and phase 8 (manual, screenshots,
demos, website text) as of 0.9.0, and 1.0.0 is the first release: the CLAP and VST3
build, eight lanes with the Body, Click, Noise, Resonator and Bus sources, per-lane
delay, polarity, note filter, transpose, Variation and pitch link, the eight
per-lane aux outputs, six effect slots per lane and on the master (sixteen
types including Reverb, Delay, Warp, Smear, Ring Mod, Stereo and Utility,
band select, oversampling), the transient guard (fade-in window and duck)
at the end of every lane, mono below and an output clip, modulation (four
LFOs, four curve envelopes, eight macros, velocity, note, random and lane
followers through a 32-route matrix, applied by `plugin/Player` on a
32-sample grid of the host's steady time), limiters (a slot type and the
master's Output Clip), 117 factory presets with a browser, CLAP preset
discovery and preset-load, the `substrike-metrics` tool, plain-text
state, the offline renderer and self-test, and the editor: a lane rack on
the left, the selected lane's strip, source, Body pitch/amp breakpoint curves
and chain on the right (accent `#8F7CF7`), a hit preview rendered on a worker
thread, Play, and the hit export by file and by drag and drop (an XDND
source in `X11Window`, OLE `DoDragDrop` in `Win32Window`; Aurum's backends
only accept drops), undo/redo (snapshots in the plugin), a live scope, and a
master Tune. A bus lane reads other
lanes in the same block: the engine orders the lanes by what they read
each process call, so no input is a block late.

What to know before working there:

- **Parameter ids are allocated in blocks** (`src/plugin/Params.h`): global
  1-999, lane L at `1000 * (L + 1)` with fixed offsets for the lane, each
  source and each effect slot, master chain 9000+, modulation 10000+. Never
  renumber; add inside the block.
- **State stores plain units** (Hz, ms, dB, enum labels), not knob positions,
  so ranges can be widened without breaking saved projects.
- **A slot's A-F are generic parameters that take their meaning from the
  slot's type** (shape table in `src/plugin/Params.cpp`): name, range, text,
  default and state key all go through `ParamTable::effective()`. Anything
  that converts a value must use it, not `def()`.
- **Breakpoint curves are state, macros are parameters.** Every curve is
  scaled by automatable macro parameters (start, end, time, curvature), so a
  drawn curve stays automatable.
- **Factory presets are sparse** (`presets/*.substrike` list only what
  differs from the defaults), so **a parameter's default must never
  change** without rendering every preset before and after
  (`substrike-render --all-presets --outdir ...`). `substrike-preset-check`
  must report no problems. Regenerated presets go through
  `presets-backup/` (gitignored), never a delete.
- **Release material** comes from Substrike's own scripts, which work in
  `build/` and never delete: `tools/make-manual.sh` (manual via
  `tools/docgen.cpp` and the shared manual.py; `release.sh` calls it),
  `tools/make-screenshots.sh` (docs/images, offscreen) and
  `tools/make-demos.sh` (dist/demos/Substrike). The website text and picture
  are in dist/website/.
- **The renderer is the test harness.** `build/substrike-render --selftest`
  loads the built `.clap` through the CLAP API; `install.sh` runs it. It also
  runs the Windows build under wine -- with a scratch `WINEPREFIX`, never the
  one in `winetest.env`.
- **GUI tests** follow Aurum's rules: `substrike-gui-snapshot` offscreen, and
  `substrike-gui-host` on a private Xvfb with scratch `XDG_CONFIG_HOME` and
  `XDG_DATA_HOME` (the drag writes into `$XDG_DATA_HOME/Substrike/Exports`).
  `SUBSTRIKE_TEST_DROP=1` gives it an XDND drop target that prints the URI it
  receives. The editor rebuilds its panels from the timer when the selection,
  a source or a slot type changes -- never from inside a widget's handler,
  which would destroy the widget under it.
- `substrike/!references/` holds third-party kick samples used to measure the
  ranges the engine must reach (the table in PLAN.md). Gitignored via
  `*/\!references/`; never commit, never ship, never name the packs.

## Build and install

From inside a plugin folder:

```sh
./install.sh                     # configure, build, self-test, install to ~/.clap
```

Or manually:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build            # defaults to ~/.clap/<PluginName>
```

A `build/` tree records the absolute source path it was configured against, so
moving or renaming a plugin folder makes its existing tree fail hard with
*"does not match the source used to generate cache"*. The fix is always to
delete the tree and let it reconfigure; nothing in it is worth keeping.

## CLAP SDK

`CLAP/` holds the CLAP SDK and the surrounding free-audio ecosystem. Each
subfolder is an independent upstream clone.

**It is gitignored and must stay that way.** It is third-party code, it is not
ours to redistribute, and everyone working here checks it out themselves.

Each plugin's `CMakeLists.txt` resolves the headers through
`${CMAKE_CURRENT_SOURCE_DIR}/../CLAP/clap/include`, so with `CLAP/` sitting
beside the plugins the repository is self-contained and builds need no extra
configuration. Only `clap/` is required to build; the rest are development and
validation tools.

To recreate the tree:

```sh
mkdir -p CLAP && cd CLAP
git clone https://github.com/free-audio/clap.git                  # required
git clone https://github.com/free-audio/clap-helpers.git
git clone https://github.com/free-audio/clap-validator.git
git clone https://github.com/free-audio/clap-info.git
git clone https://github.com/free-audio/clap-wrapper.git          # VST3 only

# Only for the VST3 build. vstgui4 is deliberately not checked out: the wrapper
# does not use it, and it is the one part of the SDK that is not MIT.
git clone --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk.git
cd vst3sdk && git submodule update --init base pluginterfaces public.sdk && cd ..
```

Reference versions in use: `clap` 1.2.10, `clap-wrapper` v0.16.0,
`clap-validator` 0.4.1, `vst3sdk` 3.8.1.

clap-validator 0.4.1 deadlocks in its `preset-discovery-crawl` and
`preset-discovery-load` tests on any plugin that lists presets with load keys
(presets compiled into the plugin, as Substrike's are): its `begin_preset()`
holds a lock that its `flush_preset()` takes again. It hangs at zero CPU,
forever. Exclude the two with `-x "preset-discovery-(crawl|load)"`.

`clap-wrapper` needs one patch to build against VST3 3.8; it is kept in
`shared/patches/` because `CLAP/` is gitignored and a fresh clone would lose it.

If the checkout lives anywhere else, point CMake at it explicitly with
`-DCLAP_INCLUDE_DIR=/path/to/clap/include`.

## Windows and VST3

**SäureKiste builds for Windows and as a VST3 as of 0.2.0**, so
`./release.sh <version>` needs no switches.

**The Windows build can be tested from here, and should be.** Cross-build the
offline tools as well (`-DSAEUREKISTE_BUILD_TOOLS=ON`) and both run under wine:
`saeurekiste-render.exe --selftest` puts the Windows plugin through the entire
suite, and `saeurekiste-guihost.exe` opens its real window so the win32 backend
can be driven and photographed exactly as the X11 one is. That found a genuine
bug in the forked window and would have found more. The tools need the same
static link options as the plugin or they die before `main()` with exit code 53
-- `ERROR_BAD_NETPATH`, Windows' unhelpful way of saying a dependent DLL is
missing.

wine is not a DAW and not Windows. No DAW has loaded any of this yet.

What it takes, and what a new plugin here has to repeat:

- `./setup-winbuild.sh` once, which cross-builds Cairo into
  `winbuild/cairo-mingw` using `shared/cmake/build-windows-cairo.sh`.
- `shared/cmake/mingw-w64-x86_64.cmake` as the toolchain file.
- `CLAP/clap-wrapper` and `CLAP/vst3sdk` checked out, with
  `shared/patches/clap-wrapper-vst3-sdk-3.8.patch` applied to the wrapper.
- The Windows and VST3 CMake blocks **copied from a Verdalis plugin**, not
  written fresh. `../Verdalis/rainyday/CMakeLists.txt` is the reference.
  Written from scratch they will build and still be wrong in ways nothing
  catches: `--exclude-all-symbols` missing so the DLL exports its whole
  interior, the trailing `-Bdynamic` missing after a `--whole-archive` group,
  a `FATAL_ERROR` where `release.sh --windows-no-gui` needs a warning, and
  `PREFIX ""` missing so a mingw VST3 bundle holds `libFoo.vst3`, which no host
  will load.

Three things to carry over, all easy to undo by accident:

- Cairo must be cross-built with `-Db_ndebug=true`. Meson does not define
  `NDEBUG` for `--buildtype=release`, so without it every `assert()` inside
  Cairo ships, and a failing one is an `abort()` behind a modal CRT dialog that
  takes the host down. `setup-winbuild.sh` checks for this and rebuilds a stale
  library that has them.
- The window class registration described under *Keeping it in step with
  Verdalis*. SäureKiste's forked window **had this bug** -- it was copied from
  `shared/src/gui/window.cpp` before the fix and carried
  `GetModuleHandle(nullptr)` and a fixed class name until the first Windows
  build was attempted. Nothing had noticed, because it cannot bite on Linux.
  That is the cost of a fork, and the reason to check one against its original
  before trusting it on a new platform.
- `CAIRO_WIN32_STATIC_BUILD` must be defined for the plugin. Cairo's headers
  declare every entry point `__declspec(dllimport)` otherwise, and the
  cross-built one is a static archive, so the link fails on a screenful of
  `__imp_cairo_*`.

## Releases

```sh
./setup-winbuild.sh        # once, and only when a plugin builds for Windows
./release.sh 0.2.0         # -> dist/audio-plugins-0.2.0.{tar.gz,zip}
```

`release.sh` discovers plugins by looking for subdirectories with a
`CMakeLists.txt` and a `src/plugin.cpp` (or `src/entry.cpp`, which is how it
finds Aurum), **and skips any that git does not
track**, so an unrelated checkout or a branch parked beside the repository does
not end up in an archive by accident. A new plugin joins a release simply by
existing and being committed — there is no list to update. A committed plugin
that is not ready to ship carries a `NO-RELEASE` file saying why, and
`release.sh` skips it; Substrike carried one from 0.1.0 until its first
release at 1.0.0.

It reads each plugin's display name and version from its `project()` line,
builds Release, installs into a staging tree, and writes `BUILD-INFO.txt`
recording what went in. It produces one archive per plugin plus one for the
whole collection.

The per-plugin archives carry that plugin's **own** version from its `project()`
line, not the collection's, because they are downloaded and updated separately.

**This script came from Verdalis unchanged**, and as of 0.2.0 this repository
delivers everything it assumes: it builds Windows and VST3 for every plugin, and
SaeureKiste does both. `--linux-only` and `--no-vst3` are still there for a
machine without the mingw toolchain or the VST3 checkouts.

Other options: `--tarball` adds `.tar.gz` beside every `.zip`; `--no-manuals`
skips the PDF manuals. It deletes nothing: an old staging tree, build tree, manual folder or
archive it replaces is moved into `dist/.retired/<time>/` (the user asked for
no deletes; clearing that folder is theirs to do). Offline tools are switched off for release builds
(`-D<PLUGIN>_BUILD_TOOLS=OFF`).

## Conventions for a new plugin

1. New folder directly under `audio-plugins/`, lowercase.
2. Copy the skeleton above from the closest existing plugin.
3. Naming: folder and preset extension lowercase; CMake project, installed
   artifact and display name CamelCase. Prefer a shell-safe folder name so the
   plugin can use `shared/tools/install-plugin.sh` rather than its own script.
4. No new repository and no new remote — it is a folder in this one. It joins
   the next release automatically once committed; add a row to the README's
   plugin table and to the one in this file.
5. Identity constants live in `src/<plugin>.h`:
   - `kPluginId` = `de.ravetracer.<plugin>` (lowercase, no hyphens)
   - `kPluginVendor` = `Ravetracer`
   - `kPluginUrl` = `https://github.com/Ravetracer/audio-plugins`
6. The version appears twice — `project(... VERSION x.y.z)` in `CMakeLists.txt`
   and `kPluginVersion` in `src/<plugin>.h` — and a `static_assert` fails the
   build when they disagree. Bump both. See *Versioning* below for which part.

## Versioning

Every plugin is versioned `X.Y.Z`, the same scheme Verdalis uses:

| | |
|---|---|
| **X** | major — big changes, a complete new DSP engine, anything that makes it a different instrument |
| **Y** | new features |
| **Z** | bug fixes |

Each plugin carries its own version; the collection archive carries its own,
independently, as the argument to `release.sh`.

**Bump it as part of the work that earns it**, not afterwards. The failure mode
is quiet and it has already happened here: a pattern bank, a collapsible window,
the Devil Fish controls and the Windows and VST3 builds all landed on SäureKiste
without the middle number moving, because nothing in the build or the tests has
an opinion about it. The version has to be bumped by whoever adds the feature.

Remember both sites — `project()` and `kPluginVersion` — plus the `Version x.y.z`
line at the top of `STATUS.md` and on the manual's cover, which the
`static_assert` does *not* cover.
7. Keep the visual language: the window comes from `shared/`, so the layout
   engine and geometry are automatic. What a plugin writes is its own `Theme`
   (a new accent, with the greys tinted towards it), its panel table, and a
   `HeaderOrnament` if it has something to animate.

## The window, and its theme

Every plugin here has **the same window**: one layout engine, one set of
widgets, one set of interactions, in `shared/src/gui/window.cpp`. Knobs, chips,
dropdowns, the preset browser, the save field, the typed value entry and the
meters are written once. A plugin describes itself with a `WindowSpec` and calls
`createWindow()`.

**Every plugin has its own colour theme, and that is the main thing that tells
the windows apart.** The `Theme` in the spec carries the whole palette, not just
the accent: the backgrounds, the panel fill and edge, the knob face, the track
and the three text greys. A plugin tints its greys towards its own accent so the
window reads as one instrument rather than a grey chassis with a coloured knob.

| Plugin | Accent | Character |
|--------|--------|-----------|
| SäureKiste | `#9BE31D` | acid green at hue 82, over a cool, almost neutral near-black graphite chassis rather than a tinted one -- the machine it models was a silver box with dark legends, and a warm chassis would be pretending otherwise |
| RumpelKiste | `#FF6E1A` | signal orange at hue 22, the colour of the machine's step keys, over a graphite leaned a few degrees warm so it cannot be mistaken for SäureKiste's across a room |

A new plugin picks its own accent and derives its greys from it. Do not reuse
another plugin's theme.

`Theme::highlight` is a second, brighter colour for an ornament that needs one.
Set it to the text colour when there is nothing like that.

**The delegate** (`plugincore/gui/gui.h`) is deliberately generic:
`guiVoiceCount()` and `guiVoiceLimit()` are whatever the activity meter counts,
and `guiEventCounter()` is an optional monotonic count of discrete events,
defaulting to zero. The meter's wording comes from the spec's `voiceNoun` and
`eventNoun`.

**SäureKiste is the exception, and it is a fork** -- and RumpelKiste forks
SäureKiste's fork, replacing the piano roll with a drum grid and keeping the
bank, the pattern map and the chain controls. `src/gui/seqwindow.*` is
`shared/src/gui/window.cpp` copied, with the namespace changed and the step grid,
the pattern bank and the collapsible panel section added — it is not a second
window beside the shared one, it replaces it for that plugin. A step grid cannot
be expressed as panels of knobs, and the plugin did not get to change what every
other window looks like in order to have one. Diff the two files to see exactly
what was added. **This is not a precedent.** A new plugin describes itself with a
`WindowSpec` and calls `createWindow()`; forking is the last resort, and the cost
of it is that every fix to the shared window has to be ported by hand.

## The manuals

Every plugin ships a PDF manual in its release archives, built by
`shared/tools/make-manual.sh <plugin>` into `dist/manuals/` as
`<Plugin>-<version>-Manual.pdf`, with the HTML it was rendered from beside it —
that HTML is self-contained (inlined stylesheet, logo as a data URI) and is what
a website would publish.

**Three of the substitutions are generated, not written.** `shared/tools/docgen.cpp`
reads the plugin's own `paramTable()` and its preset files and emits the
parameter reference and the preset library as Markdown, which the script
substitutes into `<plugin>/docs/manual.md` at `{{PARAMETER_REFERENCE}}` and
`{{PRESET_LIBRARY}}` (`{{PLUGIN}}` and `{{VERSION}}` are substituted too, and an
unsubstituted `{{...}}` is an error rather than silently shipped). So the
parameter tables cannot drift from the build.

`{{PARAMETER_SUMMARY}}` is the third, added in SäureKiste 0.8.0 and **not yet
ported to Verdalis**: the same table as `{{PARAMETER_REFERENCE}}` with the
"What it does" column left out (`docgen --params-brief`). It exists because the
explanations come from each parameter's `tip`, which is written for the plugin's
help line and names the hardware outright -- so a manual that may not print
those names cannot take the full reference, but can still carry the part that
goes stale, which is the ranges and the defaults. SäureKiste uses it as an
appendix behind its hand-written control chapter.

**A manual is not obliged to use any of them, and that is a trap.** SäureKiste's
carried none until 0.8.0: `make-manual.sh` built `docgen`, generated both
chapters and substituted them into a document with no placeholders, silently, for
every release. An *unsubstituted* `{{...}}` is a hard error; an *unused*
generated section was not, and the hand-written control tables drifted from
`params.cpp` for three releases behind it. If a manual here has no
`{{PARAMETER_REFERENCE}}` and no `{{PARAMETER_SUMMARY}}`, that is a finding, not
a style choice.

`docgen` is compiled directly with `g++` by the script rather than through the
plugin's CMake project, because release builds switch the offline tools off and
the manual still has to build. It needs only the plugin's `params.cpp` and the
shared parameter code.

**Aurum has its own.** The shared docgen reads a PluginCore `ParamDesc` table,
which Aurum does not have, so `aurum/tools/docgen.cpp` reads Aurum's
`ParamTable` and its compiled-in `factoryPresets()` instead, and
`aurum/tools/make-manual.sh` renders through the shared `manual.py` and
`manual.css` unchanged. `release.sh` runs a plugin's own
`tools/make-manual.sh <outdir>` when one exists. Aurum's parameters carry no
help text, so its manual uses `{{PARAMETER_SUMMARY}}` only. Its screenshots
come from `aurum/tools/make-screenshots.sh`, which starts a private Xvfb with
scratch XDG dirs and drives `clap_gui_host` -- never the user's desktop.

**There is no collection logo yet.** `make-manual.sh` looks for
`_designs/plugincore-logo-horizontal-4000.png` and, not finding it, passes no
logo; `manual.py` then puts the plugin's name on the cover as a wordmark
instead. Drop a logo at that path and it is picked up with no other change.

The toolchain is `python3` with the `markdown` module, plus `wkhtmltopdf` for
the PDF. `release.sh` treats it as optional: if it is missing the release says
so and goes out without manuals rather than failing.

Two constraints come from `wkhtmltopdf`, and both are in `manual.css`: its
WebKit is old (no grid, no custom properties, no `@page` margin boxes — page
geometry is set on the command line), and the build on this machine is the
unpatched-Qt one, which cannot write page footers or a PDF outline. The manuals
therefore have **no page numbers**, and the contents page is a list of links.

## Demo tracks

`shared/tools/make-demos.sh <plugin-folder>` renders one MP3 per factory preset
into `dist/demos/<Plugin>/`, applies a single linear gain towards -16 LUFS
backed off to keep the true peak under -1 dBTP, and encodes at 256 kbps. There
is no compression and no limiting, so the relative loudness the presets were
fitted to survives and a sparse preset stays quiet. It then updates
`dist/demos/demos.json` and `dist/demos/README.md` in place, touching only that
plugin's section. `--text-only` refreshes that metadata without re-rendering.

**A demo's blurb and a preset's description are two different texts.** The
preset's `description` is documentation and is what the manual's preset library
prints. A website wants a sentence a musician can read. So a plugin carries
`presets/demo-descriptions.txt` — one `Preset Name = text` line per preset — and
the script prefers it, falling back to the preset's own description and warning
about every preset it had to fall back on.

`--render-arg` passes one argument straight through to the plugin's renderer and
may be repeated. Nothing in the script knows what the argument means, which is
how a plugin asks for something only its own renderer has:

```sh
shared/tools/make-demos.sh saeure-kiste --render-arg --demo-moves
```

SaeureKiste's `--demo-moves` sweeps the cutoff and rides the resonance and the
drive across the take, relative to whatever each preset sets, so a demo is a
recording of somebody playing the preset rather than a photograph of it. Without
it every knob holds still for sixteen seconds.

SaeureKiste instead keeps plain WAV renders in `demos/`, which are gitignored
and are made with that flag. A `demo-descriptions.txt` and a proper MP3 run are
open work.

## Working notes

- `!dev/` is large and gitignored. **Never `git add -A` without checking.**
  SäureKiste's `!dev/` holds the Roland TB-303 service-note PDFs and Robin
  Whittle's Devil Fish manuals. Neither set is ours to redistribute.
- The self-test runs as part of `install.sh` — do not skip it when changing DSP.
- `STATUS.md` and `TODO.md` in each plugin are the current source of truth for
  that plugin's state; read them before starting work there.
- Trademarks: SäureKiste models a Roland product and says so. It is not
  affiliated with or endorsed by Roland, and *TB-303* is used only to name what
  was modelled. Keep that notice in the README and the manual.

### Verifying a change to shared/

Where a plugin's synthesis is stochastic, comparing output only works with the
seed pinned; `render --param` matches on the *display* name, so the override is
`randomseed=N`. SäureKiste has no random state at all and renders
bit-identically by construction, which makes it a good canary.

The recipe that proves a refactor changed nothing:

```sh
# 1. a reference build from before the change
git worktree add /tmp/ref <commit>
ln -s "$PWD/CLAP" /tmp/ref/CLAP
cmake -S /tmp/ref/saeure-kiste -B /tmp/ref-build -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/ref-build

# 2. render every preset from both
for side in ref new; do
   ./saeurekiste-render --plugin ./SaeureKiste.clap --all --outdir /tmp/wav-$side \
      --seconds 3 --tail 2 --rate 48000
done

# 3. compare
for f in /tmp/wav-ref/*.wav; do cmp "$f" "/tmp/wav-new/$(basename "$f")"; done
```

Vary `--rate`. Also run `render --selftest` (it round-trips a saved preset) and
`render --list` (it exercises preset discovery, and its output should be
byte-identical).

For a GUI change, `<plugin>-guihost <plugin>.clap "" 8` opens the real window for
eight seconds; capture it with `import -window $(xdotool search --name
SaeureKiste | head -1)` and compare with `compare -metric AE`. Zero differing
pixels is the bar. Match the window by its expected width when picking it out of
`xdotool search` — the search also matches other windows with the plugin's name
in the title, and grabbing the wrong one silently "passes".

Click a *knob*, not an enum chip, when driving interactions with `xdotool`: a
chip opens its dropdown instead, which is correct behaviour and looks like a
failed test.
