#!/usr/bin/env bash
#
# Freezer cartridges and their fast loaders, through resident-run:
#
#   tools/cart-test.sh ROM-FOLDER ACTION-REPLAY.crt FINAL-CARTRIDGE-III.crt DISK.d64 PROGRAM.prg
#
# Each cartridge loads PROGRAM (the first file on DISK) with its own fast
# loader on the true 1541, typed on the C64's keyboard, and the C64's memory
# must then hold the program byte for byte. The fast loaders time their bits
# to the cycle between C64 and drive, so this is what proves that timing.
# The Action Replay without its fast loader must load from the virtual drive.
# Writes into build/cart-test; deletes nothing. A run that hangs is stopped
# after three minutes and fails its check.
set -euo pipefail
roms="$1" ar="$2" fc3="$3" disk="$4" prg="$5"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run="${here}/build/resident-run"
work="${here}/build/cart-test"
mkdir -p "$work"
fails=0

# Key presses that type TEXT from second START on (SHIFT for the quote).
keys() {
    python3 - "$1" "$2" <<'PY'
import sys
t = float(sys.argv[1]); out = []
names = {' ': 'SPACE', '*': 'ASTERISK', ',': 'COMMA', '\n': 'RETURN'}
for ch in sys.argv[2]:
    if ch == '"':
        out += ['--key', f'{t:.2f}:LSHIFT:0.25', '--key', f'{t + 0.05:.2f}:2:0.1']; t += 0.35
    else:
        out += ['--key', f'{t:.2f}:{names.get(ch, ch.upper())}:0.08']; t += 0.15
print(' '.join(out))
PY
}

# Each dump is emptied (not deleted) before its run, so a run that never
# writes one cannot pass on the last run's file.
fresh() { : > "${work}/$1"; }

check() { # check NAME DUMP
    if python3 - "$2" "$prg" <<'PY'
import sys
ram = open(sys.argv[1], 'rb').read(); p = open(sys.argv[2], 'rb').read()
at = p[0] | p[1] << 8; body = p[2:]
sys.exit(0 if ram[at:at + len(body)] == body else 1)
PY
    then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}

load='LOAD"*",8,1
'
# Action Replay: F7 installs the fast loader.
fresh ar.bin
# shellcheck disable=SC2046
timeout 180 "$run" --roms "$roms" --crt "$ar" --attach "$disk" --drive true --seconds 16 --no-video \
    --key 3:F7:0.2 $(keys 5 "$load") --dump "15.9:${work}/ar.bin" >/dev/null || true # a hang (timeout) shows as a failed check
check "Action Replay: its fast loader loads the program on the true 1541" "${work}/ar.bin"

# Final Cartridge III: RUN/STOP at power-on skips its desktop; real time, so
# the key is seen while it starts.
fresh fc3.bin
# shellcheck disable=SC2046
timeout 180 "$run" --roms "$roms" --crt "$fc3" --attach "$disk" --drive true --no-disk-warp --seconds 34 --no-video \
    --key 0:RUNSTOP:3 $(keys 5 "$load") --dump "33.9:${work}/fc3.bin" >/dev/null || true # a hang (timeout) shows as a failed check
check "Final Cartridge III: its fast loader loads the program on the true 1541" "${work}/fc3.bin"

# Action Replay, F3 (no fast loader): the KERNAL loads from the virtual drive.
fresh ar-virtual.bin
# shellcheck disable=SC2046
timeout 180 "$run" --roms "$roms" --crt "$ar" --attach "$disk" --seconds 16 --no-video \
    --key 3:F3:0.2 $(keys 5 "$load") --dump "15.9:${work}/ar-virtual.bin" >/dev/null || true # a hang (timeout) shows as a failed check
check "Action Replay without its fast loader loads from the virtual drive" "${work}/ar-virtual.bin"

# Action Replay beside a MIDI interface: a program that resets the 6850 at
# every interface's address ($DE00, $DE04, $DE08, as Station64 does when it
# looks for one) must not reach the cartridge's register, and the
# cartridge's own writes to $DE00 must not reach a Sequential interface
# there. Either way the machine would end in the freezer, and the POKE
# after them would never run.
checkbyte() { # checkbyte NAME DUMP ADDR VALUE
    if python3 - "$2" "$3" "$4" <<'PY'
import sys
ram = open(sys.argv[1], 'rb').read()
sys.exit(0 if len(ram) == 65536 and ram[int(sys.argv[2])] == int(sys.argv[3]) else 1)
PY
    then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}
probe='POKE56832,3
POKE56836,3
POKE56840,3
POKE49152,66
'
for iface in datel sequential; do
    fresh "ar-${iface}.bin"
    # shellcheck disable=SC2046
    timeout 180 "$run" --roms "$roms" --crt "$ar" --midi "$iface" --seconds 16 --no-video \
        --key 3:F7:0.2 $(keys 5 "$probe") --dump "15.9:${work}/ar-${iface}.bin" >/dev/null || true
    checkbyte "Action Replay with a ${iface} interface: a MIDI program's probe leaves it alone" \
        "${work}/ar-${iface}.bin" 49152 66
done

[ "$fails" = 0 ] && echo "PASSED (0 failures)" || { echo "FAILED ($fails failures)"; exit 1; }
