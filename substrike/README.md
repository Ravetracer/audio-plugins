# Substrike

A kick drum designer for Linux and Windows as a native CLAP instrument (plus
VST3 through clap-wrapper). Everything is synthesised; nothing is sampled.

The goal is one instrument for every kind of kick, from ambient through hip
hop, techno and trance to hardstyle and hardcore: up to eight layers, each with
its own effect chain in free order, layers feeding one another for rumble, a
master chain, and modulation. The design and the phases are in
[docs/PLAN.md](docs/PLAN.md).

## Status: 0.1.0, phase 1

What exists:

- One lane with the **Body** source: a sine whose pitch falls from Pitch
  Start to Pitch End along a curve (Sweep Time, Sweep Curve) and whose level
  runs Attack, Hold and a curved Decay. Start phase, key tracking against a
  root note (C1 by default), velocity sensitivity, level and pan.
- One-shot behaviour: note-off does nothing; a new hit fades the old one out
  over 3 ms, so retriggering does not click. Note choke is honoured.
- Plain-text state: parameters are saved in their own units (Hz, ms, dB), so
  a state stays valid when a range is widened later.
- No editor yet; the host shows its generic parameter view.

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
whole self-test against the Windows binary under wine. No Windows DAW has
loaded Substrike yet.

## Tools

`substrike-render` loads the built `.clap` like a host does:

```sh
build/substrike-render --plugin build/Substrike.clap --out kick.wav
build/substrike-render --param "L1 Pitch Start=1.6 kHz" --param "L1 Decay=1.5 s"
build/substrike-render --hits 4 --bpm 140 --seconds 2 --key 38
build/substrike-render --list-params
build/substrike-render --selftest
```

Parameters are addressed by their display name (spaces and case do not
matter); values are read by the plugin's own text parser, so "1.6k", "C2"
(for a frequency) and "1.5 s" all work. The output is a 32-bit float WAV.

The self-test covers parameter metadata and text round trips, state round
trips, the default hit (pitch, silence, sleep), bit-identical output across
block sizes, MIDI and CLAP note dialects, click-free retriggering, key
tracking and other sample rates.

## Reference material

`!references/` holds third-party kick samples used to measure what the engine
must cover. It is gitignored and is never committed or shipped.

## License

MIT, see the repository's LICENSE.
