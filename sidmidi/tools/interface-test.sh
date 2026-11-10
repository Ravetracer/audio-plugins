#!/usr/bin/env bash
#
# The MIDI programs find their interface by themselves, through sidmidi-run:
#
#   tools/interface-test.sh ROM-FOLDER STATION64.d64 CYNTHCART-ROM.bin
#
# Each program starts with one interface selected and is sent a note; it must
# sound. Station64 detects Datel, Passport, Sequential and Namesoft (the last
# two through a test that waits for TDRE after resetting a running ACIA);
# Cynthcart detects Datel, Passport and Sequential (and needs a freshly reset
# ACIA to read 0). Writes into build/interface-test; deletes nothing.
set -euo pipefail
roms="$1" sta64="$2" cynth="$3"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run="${here}/build/sidmidi-run"
work="${here}/build/interface-test"
mkdir -p "$work"
fails=0

# check NAME WAV FROM TO: the RMS level between FROM and TO seconds must be
# above -40 dBFS (a held note is about -30, the program idle below -50).
check() {
    if python3 - "$2" "$3" "$4" <<'PY'
import math, struct, sys
d = open(sys.argv[1], 'rb').read()
i, ch, sr, data = 12, 2, 48000, b''
while i + 8 <= len(d):
    cid, n = d[i:i + 4], struct.unpack('<I', d[i + 4:i + 8])[0]
    if cid == b'fmt ':
        ch, sr = struct.unpack('<HI', d[i + 10:i + 16])
    elif cid == b'data':
        data = d[i + 8:i + 8 + n]
        break
    i += 8 + n + (n & 1)
s = struct.unpack('<%df' % (len(data) // 4), data)
seg = s[int(float(sys.argv[2]) * sr) * ch:int(float(sys.argv[3]) * sr) * ch]
level = 20 * math.log10(math.sqrt(sum(x * x for x in seg) / max(1, len(seg))) + 1e-12)
sys.exit(0 if level > -40 else 1)
PY
    then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}

for iface in datel passport sequential namesoft; do
    wav="${work}/station64-${iface}.wav"
    : > "$wav" # emptied, not deleted, so a run that writes nothing cannot pass
    timeout 180 "$run" --roms "$roms" --d64 "$sta64" --midi "$iface" --seconds 38 --no-video \
        --note 30:60:100:6 --wav "$wav" >/dev/null || true
    check "Station64 finds a ${iface} interface and plays" "$wav" 31 36
done

for iface in datel passport sequential; do
    wav="${work}/cynthcart-${iface}.wav"
    : > "$wav"
    timeout 120 "$run" --roms "$roms" --crt "$cynth" --midi "$iface" --seconds 10 --no-video \
        --note 6:60:100:3 --wav "$wav" >/dev/null || true
    check "Cynthcart finds a ${iface} interface and plays" "$wav" 6.5 8.5
done

[ "$fails" = 0 ] && echo "PASSED (0 failures)" || { echo "FAILED ($fails failures)"; exit 1; }
