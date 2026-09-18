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
| **[SäureKiste](saeure-kiste/)** | 0.3.1 | Linux, Windows | CLAP, VST3 | a monophonic acid bass synthesiser modelled on the Roland TB-303's main board, from the February 1982 service notes, with Robin Whittle's Devil Fish modification |

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

## Layout

```
audio-plugins/
├── shared/            the PluginCore library every plugin is built on
├── saeure-kiste/     SaeureKiste
├── release.sh         builds every plugin and packs the archives
├── setup-winbuild.sh  one-time Windows cross-build setup
└── CLAP/              CLAP SDK checkouts — gitignored, fetched locally
```

## Windows and VST3

Not yet. Everything needed is in the repository — the Cairo cross-build, the
mingw toolchain file, the clap-wrapper patch, and a complete win32 window in
`shared/src/gui/window.cpp` — but no plugin here has been built or tested on
Windows, and a plugin nobody has run is worse than one that refuses to
configure. It is planned.

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
is their trademark and is used only to name what was modelled.
