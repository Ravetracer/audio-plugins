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
build/sidmidi-keytest
prg="${SIDMIDI_TEST_PRG:-_dev_/cynthcart/bin/cynthcart.prg}"
roms="${SIDMIDI_TEST_ROMS:-_dev_/roms}"
if [ -f "$prg" ] && [ -d "$roms" ]; then
    build/sidmidi-selftest build/SidMidi.clap "$prg" 2 "$roms"
    # The true 1541: a disk loaded through the emulated drive, then a state round trip.
    d64="${SIDMIDI_TEST_D64:-_dev_/Station64/sta64.d64}"
    [ -f "$d64" ] && build/sidmidi-selftest build/SidMidi.clap "$d64" 14 "$roms" true1541
    # Media: a cartridge with a disk beside it, a disk only inserted, Start from disk.
    cart="${SIDMIDI_TEST_CART:-_dev_/cynthcart/bin/cynthcart_cartridge_ROM.bin}"
    disk="${SIDMIDI_TEST_DISK:-_dev_/SID-Wizard/SID-Wizard-1.93-C64-with-source/application/SID-Wizard-instruments.d64}"
    start="${SIDMIDI_TEST_D64:-_dev_/Station64/sta64.d64}"
    if [ -f "$cart" ] && [ -f "$disk" ] && [ -f "$start" ]; then
        build/sidmidi-selftest build/SidMidi.clap --media "$cart" "$disk" "$start" "$roms"
    fi
    # Freezer cartridges: both fast loaders on the true 1541, byte for byte.
    ar="${SIDMIDI_TEST_AR:-_dev_/cartridges/action_replay_6.crt}"
    fc3="${SIDMIDI_TEST_FC3:-_dev_/cartridges/final cartridge iii (1988-12)(riska h&p).crt}"
    if [ -f "$ar" ] && [ -f "$fc3" ] && [ -f "$start" ] && [ -f "${start%.d64}.prg" ]; then
        tools/cart-test.sh "$roms" "$ar" "$fc3" "$start" "${start%.d64}.prg"
    fi
    # Offline renders repeat exactly, across processes too.
    for i in 1 2; do
        build/sidmidi-run --roms "$roms" --prg "$prg" --seconds 6 --note 4:60:100:1 --no-video \
            --wav build/repeat-$i.wav >/dev/null
    done
    cmp -s build/repeat-1.wav build/repeat-2.wav && echo "ok    two renders are identical" \
        || { echo "FAIL  two renders differ"; exit 1; }
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
