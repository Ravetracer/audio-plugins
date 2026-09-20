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
└── saeure-kiste/     SaeureKiste — a TB-303 model
```

## The plugins

| # | Plugin | Folder | Status | Platforms | Formats | What it is |
|---|--------|--------|--------|-----------|---------|------------|
| 1 | **SäureKiste** | `saeure-kiste/` | 0.7.0 | Linux, Windows | CLAP, VST3 | a Roland TB-303 model, from the 1982 service notes, plus Robin Whittle's Devil Fish modification |

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
`CMakeLists.txt` and a `src/plugin.cpp`, **and skips any that git does not
track**, so an unrelated checkout or a branch parked beside the repository does
not end up in an archive by accident. A new plugin joins a release simply by
existing and being committed — there is no list to update.

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
skips the PDF manuals. Offline tools are switched off for release builds
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

A new plugin picks its own accent and derives its greys from it. Do not reuse
another plugin's theme.

`Theme::highlight` is a second, brighter colour for an ornament that needs one.
Set it to the text colour when there is nothing like that.

**The delegate** (`plugincore/gui/gui.h`) is deliberately generic:
`guiVoiceCount()` and `guiVoiceLimit()` are whatever the activity meter counts,
and `guiEventCounter()` is an optional monotonic count of discrete events,
defaulting to zero. The meter's wording comes from the spec's `voiceNoun` and
`eventNoun`.

**SäureKiste is the exception, and it is a fork.** `src/gui/seqwindow.*` is
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

**Two of the chapters are generated, not written.** `shared/tools/docgen.cpp`
reads the plugin's own `paramTable()` and its preset files and emits the
parameter reference and the preset library as Markdown, which the script
substitutes into `<plugin>/docs/manual.md` at `{{PARAMETER_REFERENCE}}` and
`{{PRESET_LIBRARY}}` (`{{PLUGIN}}` and `{{VERSION}}` are substituted too, and an
unsubstituted `{{...}}` is an error rather than silently shipped). So the
parameter tables cannot drift from the build.

`docgen` is compiled directly with `g++` by the script rather than through the
plugin's CMake project, because release builds switch the offline tools off and
the manual still has to build. It needs only the plugin's `params.cpp` and the
shared parameter code.

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
