# Aurum Reverb

Algorithmic reverb for Linux and Windows as a native CLAP plugin (plus VST3
through clap-wrapper), with its own Cairo interface on X11 and win32.

## Features

- Three algorithms: Natural (32-line feedback delay network), Classic
  (allpass ring in the style of early digital units), Plate (Dattorro
  figure-eight tank).
- Room control moving through eleven room models from Ambience (0.2 s) to
  Cathedral (10 s), Length 25 to 400 %.
- Decay Contour: six bands (bell, shelves, notch) shaping the reverberation
  time per frequency. Realised with third-octave attenuation filters in every
  delay line; measured T60 follows the curve within a few percent.
- Tone EQ: six bands (bell, shelves, low/high cut up to 96 dB/oct) with
  stereo/left/right/mid/side placement and automatic gain compensation.
- Motion, Air, Depth, Density, Width (mono, cross-fed, multi-mono, side
  boost), Pre-Delay with tempo sync, Ducking, Gate with tempo sync, Freeze
  (toggle or hold), Mix with Lock Mix, input/output level and pan, soft
  bypass.
- Static editor without analyser: room ruler, decay time per third octave in
  seconds, tone curve, band inspector.
- Presets with folders, tags, favourites and search; undo/redo; A/B.
- Impulse response import (WAV/AIFF): derives Room, Decay Contour, Tone EQ,
  width, depth and pre-delay from a recorded space.
- Import of external `.ffp` text presets as a starting point.
- MIDI learn for every control.

## Build

Requirements: CMake 3.21+, a C++20 compiler, Ninja (optional), cairo and X11
development packages. The CLAP, clap-wrapper and VST3 SDKs are expected in
the repository's `CLAP/` folder (`../CLAP`, override with
`-DAURUM_SDK_DIR=...`); see the repository README for the checkout.

```sh
./install.sh                # configure, build, self-test, install to ~/.clap and ~/.vst3
```

Or manually:

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

The default target architecture is x86-64-v3 (AVX2/FMA), on Windows as well.
For older CPUs use `-DAURUM_ARCH_FLAGS=-march=x86-64-v2`.

### Windows

Cross-compiled from Linux with mingw-w64, against the Cairo that
`../setup-winbuild.sh` builds into `../winbuild/cairo-mingw` (override with
`-DAURUM_WIN_CAIRO=...`):

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=../shared/cmake/mingw-w64-x86_64.cmake
ninja -C build-win          # build-win/Aurum.clap, build-win/vst3/Aurum.vst3
```

The runtime and Cairo are linked in; the binaries need no DLLs beside them.
`../release.sh` builds both platforms and both formats.

## Files

- Presets: `$XDG_DATA_HOME/Aurum/Presets` (default `~/.local/share/Aurum/Presets`);
  on Windows `%APPDATA%\Aurum\Presets`.
- Settings (GUI size, lock mix, MIDI map, favourites):
  `$XDG_CONFIG_HOME/Aurum/settings.ini`; on Windows `%APPDATA%\Aurum\settings.ini`.
- File dialogs use `zenity` or `kdialog` on Linux, the system dialogs on Windows.

## Manual

`docs/manual.md` is the manual's text; `tools/make-manual.sh [outdir]` builds
it as HTML and PDF (default `../dist/manuals`), generating the parameter
appendix and the preset library from the plugin itself with `tools/docgen.cpp`.
`tools/make-screenshots.sh` regenerates `docs/images/` on a private Xvfb
display (`AURUM_BUILD=<build dir>`, default `build`); run it after any change to
the window and look at the result. Needs python3 with `markdown`, wkhtmltopdf,
Xvfb, xdotool and ImageMagick.

## Tests

- `build/tests/geq_test` - accuracy of the attenuation filter fit.
- `build/tests/engine_test [t60|stability|cpu|wav]` - decay times per
  octave against the model, random parameter stress test, CPU use.
- `build/import_test <tmpdir> [ffp-preset-dir]` - IR import round trip and
  preset conversion.
- `build/gui_snapshot out.png [scale]` - renders the editor offscreen (a PPM
  on Windows, whose Cairo has no PNG support).
- `build/clap_gui_host Aurum.clap [seconds]` - minimal CLAP host for GUI tests
  (e.g. under Xvfb with xdotool).
- `clap-validator validate build/Aurum.clap`

The Windows build of every test runs under wine. Use a scratch `WINEPREFIX`:
the plugin writes its presets and settings to `%APPDATA%` inside the prefix.
