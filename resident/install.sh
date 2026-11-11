#!/usr/bin/env bash
# Builds ReSIDent in the build container, runs its tests, and installs the CLAP
# into ~/.clap/ReSIDent and the VST3 into ~/.vst3.
#
#   ./install.sh                 build, test, install
#   ./install.sh --no-install    build and test only
#
# The tests need a program to play and ROMs: RESIDENT_TEST_PRG and
# RESIDENT_TEST_ROMS (default: Cynthcart and the ROMs in _dev_, which is never
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
build/resident-keytest
build/resident-enginetest
prg="${RESIDENT_TEST_PRG:-_dev_/cynthcart/bin/cynthcart.prg}"
roms="${RESIDENT_TEST_ROMS:-_dev_/roms}"
if [ -f "$prg" ] && [ -d "$roms" ]; then
    build/resident-selftest build/ReSIDent.clap "$prg" 2 "$roms"
    # The same on the Light SID engine.
    build/resident-selftest build/ReSIDent.clap "$prg" 2 "$roms" lite
    # The true 1541: a disk loaded through the emulated drive, then a state round trip.
    d64="${RESIDENT_TEST_D64:-_dev_/Station64/sta64.d64}"
    [ -f "$d64" ] && build/resident-selftest build/ReSIDent.clap "$d64" 14 "$roms" true1541
    # Media: a cartridge with a disk beside it, a disk only inserted, Start from disk.
    cart="${RESIDENT_TEST_CART:-_dev_/cynthcart/bin/cynthcart_cartridge_ROM.bin}"
    disk="${RESIDENT_TEST_DISK:-_dev_/SID-Wizard/SID-Wizard-1.93-C64-with-source/application/SID-Wizard-instruments.d64}"
    start="${RESIDENT_TEST_D64:-_dev_/Station64/sta64.d64}"
    if [ -f "$cart" ] && [ -f "$disk" ] && [ -f "$start" ]; then
        build/resident-selftest build/ReSIDent.clap --media "$cart" "$disk" "$start" "$roms"
    fi
    # Freezer cartridges: both fast loaders on the true 1541, byte for byte.
    ar="${RESIDENT_TEST_AR:-_dev_/cartridges/action_replay_6.crt}"
    fc3="${RESIDENT_TEST_FC3:-_dev_/cartridges/final cartridge iii (1988-12)(riska h&p).crt}"
    if [ -f "$ar" ] && [ -f "$fc3" ] && [ -f "$start" ] && [ -f "${start%.d64}.prg" ]; then
        tools/cart-test.sh "$roms" "$ar" "$fc3" "$start" "${start%.d64}.prg"
    fi
    # Interface detection by the programs themselves, on every interface they know.
    if [ -f "$start" ] && [ -f "$cart" ]; then
        tools/interface-test.sh "$roms" "$start" "$cart"
    fi
    # Offline renders repeat exactly, across processes too.
    for i in 1 2; do
        build/resident-run --roms "$roms" --prg "$prg" --seconds 6 --note 4:60:100:1 --no-video \
            --wav build/repeat-$i.wav >/dev/null
    done
    cmp -s build/repeat-1.wav build/repeat-2.wav && echo "ok    two renders are identical" \
        || { echo "FAIL  two renders differ"; exit 1; }
    for i in 1 2; do
        build/resident-run --roms "$roms" --prg "$prg" --seconds 6 --note 4:60:100:1 --no-video --engine lite \
            --wav build/repeat-lite-$i.wav >/dev/null
    done
    cmp -s build/repeat-lite-1.wav build/repeat-lite-2.wav && echo "ok    two Light renders are identical" \
        || { echo "FAIL  two Light renders differ"; exit 1; }
    # Up to 0.8.1 the data folder was called SidMidi: its ROMs and CC maps are
    # copied into the new one on first use, and the old folder stays as it was.
    mig="$(mktemp -d build/migrate.XXXXXX)"
    mkdir -p "$mig/SidMidi/CC Maps"
    cp -r "$roms" "$mig/SidMidi/ROMs"
    echo "old readme" > "$mig/SidMidi/ROMs/README.txt"
    cp ccmaps/cynthcart.ccmap "$mig/SidMidi/CC Maps/mine.ccmap"
    env -u RESIDENT_ROM_DIR XDG_DATA_HOME="$PWD/$mig" build/resident-run --prg "$prg" --seconds 1 --no-video \
        --wav "$mig/boot.wav" >/dev/null \
        && [ -f "$mig/ReSIDent/CC Maps/mine.ccmap" ] && [ ! -e "$mig/ReSIDent/ROMs/README.txt" ] \
        && [ -f "$mig/SidMidi/ROMs/README.txt" ] && [ -f "$mig/SidMidi/CC Maps/mine.ccmap" ] \
        && echo "ok    the old SidMidi folder is taken over" \
        || { echo "FAIL  the old SidMidi folder was not taken over ($mig)"; exit 1; }
else
    echo "no test program ($prg) or ROMs ($roms): plugin self-test skipped"
fi
'

if [ "$install" = 1 ]; then
    mkdir -p "$HOME/.clap/ReSIDent" "$HOME/.vst3"
    cp build/ReSIDent.clap "$HOME/.clap/ReSIDent/ReSIDent.clap"
    cp -r build/vst3/ReSIDent.vst3 "$HOME/.vst3/"
    echo "installed: $HOME/.clap/ReSIDent/ReSIDent.clap and $HOME/.vst3/ReSIDent.vst3"
fi
