#!/usr/bin/env bash
#
# Renders ReSIDent's demo audio into dist/demos/ReSIDent/.
#
#   resident/tools/make-demos.sh [--engine lite]
#
# With --engine lite the same takes are played on the Light SID engine and
# written to dist/demos/ReSIDent-Light/ instead, for comparing the engines by
# ear; the release demos are the reSIDfp ones.
#
# ReSIDent has no presets of its own -- its sounds are the C64 programs' -- so
# the shared make-demos.sh, which renders factory presets, does not fit. This
# plays a short phrase into each program the developer has in _dev_ through
# resident-run (offline, so a re-run reproduces the files), trims the start-up
# away, and treats the result like the shared script does: one linear gain
# towards -16 LUFS, backed off to keep the true peak under -1 dBTP, no
# compression, MP3 at 256 kbps joint stereo. The gain is applied by ffmpeg,
# not sox, which clips a float input above full scale.
#
# The programs and ROMs are never shipped; the demos are recordings of them
# playing. Files are overwritten, nothing is deleted. Needs a build with the
# tools (build/resident-run), ffmpeg, lame and python3.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "${here}/.." && pwd)"
root="$(cd "${src}/.." && pwd)"
run="${src}/build/resident-run"
roms="${RESIDENT_DEMO_ROMS:-${src}/_dev_/roms}"
out="${root}/dist/demos/ReSIDent"
work="${src}/build/demo-work"
engine=()
if [ "${1:-}" = "--engine" ] && [ "${2:-}" = "lite" ]; then
   engine=(--engine lite)
   out="${root}/dist/demos/ReSIDent-Light"
   work="${src}/build/demo-work-light"
elif [ $# -gt 0 ]; then
   echo "usage: $0 [--engine lite]" >&2
   exit 2
fi
mkdir -p "$out" "$work"
[ -x "$run" ] || { echo "no ${run}: build with the tools" >&2; exit 1; }
for t in ffmpeg lame python3; do
   command -v "$t" >/dev/null || { echo "${t} not found" >&2; exit 1; }
done

# notes <start> <step> <len> <chord>... : one --note per key, chord after chord.
notes() {
   local t="$1" step="$2" len="$3"; shift 3
   local args=()
   for chord in "$@"; do
      for key in ${chord//,/ }; do
         args+=(--note "${t}:${key}:100:${len}")
      done
      t="$(python3 -c "print(round(${t} + ${step}, 3))")"
   done
   echo "${args[@]}"
}

# demo <name> <file> <option> <start> <seconds> <notes...>
demo() {
   local name="$1" file="$2" opt="$3" start="$4" seconds="$5"; shift 5
   [ -f "$file" ] || { echo "    skipping ${name}: no ${file}"; return; }
   echo "--> ${name}"
   # shellcheck disable=SC2086
   "$run" --roms "$roms" "$opt" "$file" --no-video --seconds "$(python3 -c "print(${start} + ${seconds})")" \
      "${engine[@]}" "$@" --wav "${work}/${name}-full.wav" >/dev/null
   ffmpeg -nostats -hide_banner -loglevel error -y -ss "$start" -t "$seconds" -i "${work}/${name}-full.wav" \
      -c:a pcm_f32le "${work}/${name}.wav"
   read -r loudness truepeak <<<"$(ffmpeg -nostats -hide_banner -i "${work}/${name}.wav" \
      -filter_complex ebur128=peak=true -f null - 2>&1 |
      awk '/Integrated loudness:/{s=1} s&&/^ *I: /{i=$2} /True peak:/{p=1} p&&/^ *Peak: /{t=$2} END{print i, t}')"
   gain="$(python3 -c "
i, t = float('$loudness'), float('$truepeak')
g = -16.0 - i
if t + g > -1.0:
    g = -1.0 - t
print(f'{g:.2f}')")"
   ffmpeg -nostats -hide_banner -loglevel error -y -i "${work}/${name}.wav" -af "volume=${gain}dB" \
      -c:a pcm_s24le "${work}/${name}-final.wav"
   lame --quiet -b 256 -m j -h "${work}/${name}-final.wav" "${out}/${name}.mp3"
   printf '    %-28s %+7s dB\n' "${name}.mp3" "$gain"
}

dev="${src}/_dev_"

# Cynthcart: its start-up preset, chords of an A minor turn-around. Ready
# after about three seconds.
# shellcheck disable=SC2046
demo 01-Cynthcart "${dev}/cynthcart/bin/cynthcart.prg" --prg 4 16 \
   $(notes 4 2 1.9 69,72,76 65,69,72 60,64,67 67,71,74 69,72,76 65,69,72 67,71,74 69,72,76)

# SID Synth: its start-up instrument (an arpeggio), MIDI switched on in its
# menu first (RUN/STOP, item 01 to DATEL, RUN/STOP), then held chords.
# shellcheck disable=SC2046
demo 02-SIDSynth "${dev}/SID_Synth/sidsynth.d64" --d64 14 16 \
   --key 9:RUNSTOP:0.2 --key 11:RETURN:0.2 --key 12:RUNSTOP:0.2 \
   $(notes 14 4 3.8 57,60,64 53,57,60 48,52,55 55,59,62)

# Station64: its start-up sound, a bass line; it unpacks for about twelve
# seconds.
# shellcheck disable=SC2046
demo 03-Station64 "${dev}/Station64/sta64.d64" --d64 15 16 \
   $(notes 15 0.5 0.4 45 45 57 45 43 43 55 43 41 41 53 41 43 43 55 47 45 45 57 45 43 43 55 43 41 41 53 41 40 40 52 44)

ls -l "$out"
