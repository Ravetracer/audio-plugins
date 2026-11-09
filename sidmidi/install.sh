#!/usr/bin/env bash
# Builds SidMidi in the build container, runs its tests, and installs the CLAP
# into ~/.clap/SidMidi and the VST3 into ~/.vst3.
#
#   ./install.sh                 build, test, install
#   ./install.sh --no-install    build and test only
#
# The tests need a program to play and ROMs: SIDMIDI_TEST_PRG and
# SIDMIDI_TEST_ROMS (default: Cynthcart and the ROMs in _dev_, which is never
# committed). Without them the plugin self-test is skipped. The installed
# plugin finds its ROMs in its own ROM folder, never in _dev_.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"
install=1
[ "${1:-}" = "--no-install" ] && install=0

../devbox.sh bash -c '
set -e
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C build
tools/fetch-cpu-tests.sh build
prg="${SIDMIDI_TEST_PRG:-_dev_/cynthcart/bin/cynthcart.prg}"
roms="${SIDMIDI_TEST_ROMS:-_dev_/roms}"
if [ -f "$prg" ] && [ -d "$roms" ]; then
    build/sidmidi-selftest build/SidMidi.clap "$prg" 2 "$roms"
    # The true 1541: a disk loaded through the emulated drive, then a state round trip.
    d64="${SIDMIDI_TEST_D64:-_dev_/Station64/sta64.d64}"
    [ -f "$d64" ] && build/sidmidi-selftest build/SidMidi.clap "$d64" 14 "$roms" true1541
else
    echo "no test program ($prg) or ROMs ($roms): plugin self-test skipped"
fi
'

if [ "$install" = 1 ]; then
    mkdir -p "$HOME/.clap/SidMidi" "$HOME/.vst3"
    cp build/SidMidi.clap "$HOME/.clap/SidMidi/SidMidi.clap"
    cp -r build/vst3/SidMidi.vst3 "$HOME/.vst3/"
    echo "installed: $HOME/.clap/SidMidi/SidMidi.clap and $HOME/.vst3/SidMidi.vst3"
fi
