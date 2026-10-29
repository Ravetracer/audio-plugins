# Audio Plugins

A collection of native **CLAP** audio plugins for Linux.

Unlike a suite, these share no theme. Each plugin is whatever it needs to be —
a machine model, a utility, an effect, an experiment. What they have in common
is the foundation they are built on: one parameter model, one preset format and
one window, in `shared/`.

Vibe coded with Claude.

## The plugins

| Plugin | Version | Platforms | Formats | What it is |
|--------|---------|-----------|---------|------------|
| **[SäureKiste](saeure-kiste/)** | 0.17.0 | Linux, Windows | CLAP, VST3 | a monophonic acid bass synthesiser modelled on the Roland TB-303's main board, from the February 1982 service notes, with Robin Whittle's Devil Fish modification |
| **[RumpelKiste](rumpel-kiste/)** | 0.2.1 | Linux, Windows | CLAP, VST3 | a rhythm composer modelled on the Roland TR-909's voicing board, from the June 1984 service notes, with a 64-pattern step sequencer and a drive bus |
| **[Aurum](aurum/)** | 0.3.1 | Linux, Windows | CLAP, VST3 | an algorithmic reverb with three engines (feedback delay network, allpass ring, plate), a per-frequency decay contour and a tone EQ; built on its own DSP and window rather than `shared/` |
| **[Substrike](substrike/)** | 1.1.0 | Linux, Windows | CLAP, VST3 | a layered kick drum designer for every style, from ambient to hardcore: eight synthesised layers (body, click, noise, resonator, and bus lanes for rumble) with per-lane outputs, effect chains with reverb, delays and drive, a transient guard, a modulation matrix with LFOs, envelopes and macros, 117 factory presets with a browser, breakpoint curves and an editor with hit export by drag and drop; built on its own code rather than `shared/` |
| **[Shapeau](shapeau/)** | 0.4.0 | Linux, Windows | CLAP, VST3 | a multiband drawing rack: a chain of effects (volume, filter, drive, crush, time, width, pan) in any order, each driven by a shape drawn per frequency band, locked to the beat or restarted by a MIDI note or a hit in the sidechain, with the audio and the sidechain drawn behind the shape as it plays |

## Build

Each plugin builds and installs from its own folder:

```sh
cd saeure-kiste
./install.sh          # configure, build, self-test, install to ~/.clap
```

The build needs a CLAP SDK checkout beside the plugins. It is third-party code
and is gitignored, so fetch it once:

```sh
mkdir -p CLAP && git -C CLAP clone https://github.com/free-audio/clap.git
```

The plugin window needs X11 and Cairo. On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build libx11-dev libcairo2-dev
```

### Or in a container

`devbox.sh` runs any command inside a Docker image that holds the whole
toolchain -- the Linux and Windows compilers, wine for the Windows self-tests,
Xvfb for the GUI tools, wkhtmltopdf for the manuals -- so a machine needs
nothing but Docker:

```sh
./devbox.sh                          # a shell in the container
./devbox.sh ./release.sh 0.19.0      # any command, from the current folder
```

The repository is mounted at its own path and the command runs as you, so
build trees work from both sides. The container's home is thrown away on exit,
which keeps wine prefixes and plugin settings out of your real one.

## Layout

```
audio-plugins/
├── shared/            the PluginCore library every plugin is built on
├── saeure-kiste/     SaeureKiste
├── rumpel-kiste/     RumpelKiste
├── aurum/            Aurum
├── substrike/        Substrike
├── shapeau/          Shapeau
├── release.sh         builds every plugin and packs the archives
├── setup-winbuild.sh  one-time Windows cross-build setup
├── devbox.sh          the build container; devbox/Dockerfile is its image
└── CLAP/              CLAP SDK checkouts — gitignored, fetched locally
```

## Windows and VST3

Every plugin builds for Linux and Windows, as CLAP and as VST3, and
`./release.sh <version>` packs all four. Windows is cross-compiled with
mingw-w64 against a Cairo built once by `./setup-winbuild.sh`, and the Windows
self-tests run under wine -- both work inside `devbox.sh`. The Windows builds
have been tested under wine only.

## Relationship to Verdalis

This is a separate project from the
[Verdalis](https://github.com/Ravetracer/Verdalis) suite, with its own
repository and its own rules. Verdalis models natural sound phenomena from first
principles and never ships a sample; nothing here is bound by that.

The two share ancestry in one place: this repository's `shared/` began as a copy
of Verdalis's, renamed from the `verdalis` namespace to `plugincore`. They are
independent copies now and will drift apart.

## Licence

MIT — see [LICENSE](LICENSE).

SäureKiste is not affiliated with or endorsed by Roland Corporation. *TB-303*
is their trademark and is used only to name what was modelled. The same goes for
RumpelKiste and *TR-909*.
