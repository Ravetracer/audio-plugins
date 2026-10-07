# Substrike

A kick drum designer for Linux and Windows as a native CLAP instrument (plus
VST3 through clap-wrapper). Everything is synthesised; nothing is sampled.

The goal is one instrument for every kind of kick, from ambient through hip
hop, techno and trance to hardstyle and hardcore: up to eight layers, each with
its own effect chain in free order, layers feeding one another for rumble, a
master chain, and modulation. The design and the phases are in
[docs/PLAN.md](docs/PLAN.md).

## Status: 0.2.0, phase 2

What exists:

- **Eight lanes**, each playing one source, with level, pan, velocity
  sensitivity, a trigger delay of up to 100 ms (sample-exact), polarity
  invert, a note filter (any note, or one note, so Substrike can be played as
  a kit), transpose (-24 to +36 semitones) and Variation, which randomises
  pitch, level, decay and noise from hit to hit. At 0 % every hit repeats
  bit for bit.
- **Body**: sine, triangle, saw, square (band-limited) or 8-partial additive
  (tilt, even partials, stretch), with a pitch drop from Pitch Start to Pitch
  End along a curve, attack, hold and a curved decay, start phase, key
  tracking against a root note (MIDI 36, shown as C2 with C4 = 60), tanh
  shaping, FM from its own modulator (amount, ratio, decay), phase feedback
  and drift.
- **Click**: impulse, noise burst, blip or zap (a sweep through up to 8
  octaves), with a decay and its own filter, peak-normalised.
- **Noise**: white, pink, brown or crackle (with a density), stereo width,
  a filter with a decaying cutoff envelope, and attack, hold and a curved
  decay.
- **Resonator**: 2-8 damped modes (membrane, harmonic, odd or bar) struck
  by an impulse, a mallet or a noise burst, with tune, key tracking, decay,
  damping of the higher modes, brightness, hardness and a pitch drop.
- **Pitch Link**: a lane can follow another lane's Body pitch -- the whole
  drop for a Body, the end pitch for a Resonator or a Click blip -- so layers
  stay in tune.
- **Outputs**: a stereo main output plus one stereo output per lane. Each
  lane goes to Main, its own output, or both. The master level applies to
  the main output only.
- One-shot behaviour: note-off does nothing; a new hit fades the old one out
  over 3 ms, so retriggering does not click. Note choke is honoured.
- Plain-text state: parameters are saved in their own units (Hz, ms, dB), so
  a state stays valid when a range is widened later. A 0.1.0 state loads.
- No effect chain and no editor yet; the host shows its generic parameter
  view.

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
build/substrike-render --param "L1 Pitch Start=1.6 kHz" --param "L1 Body Decay=1.5 s"
build/substrike-render --param "L2 On=On" --param "L2 Output=Aux" --aux 2 --out click.wav
build/substrike-render --hits 4 --bpm 140 --seconds 2 --key 38
build/substrike-render --list-params
build/substrike-render --selftest
```

Parameters are addressed by their display name (spaces and case do not
matter); values are read by the plugin's own text parser, so "1.6k", "C2"
(for a frequency) and "1.5 s" all work. The output is a 32-bit float WAV of
the main output, or of lane N's output with `--aux N`.

The self-test covers parameter metadata and text round trips, state round
trips (and loading a 0.1.0 state), the default hit (pitch, silence, sleep),
bit-identical output across block sizes, MIDI and CLAP note dialects,
click-free retriggering, key tracking and other sample rates, and for the
lanes: every source variant finite and asleep after its tail, the note
filter, the trigger delay to the sample, polarity, output routing, pitch link
and transpose, resonator tuning, and Variation on and off.

## Reference material

`!references/` holds third-party kick samples used to measure what the engine
must cover. It is gitignored and is never committed or shipped.

## License

MIT, see the repository's LICENSE.
