#!/usr/bin/env bash
#
# Renders Substrike's listening examples for the website into
# dist/demos/Substrike/: a hand-picked set of factory presets, each played
# by the plugin's own offline renderer as a short loop at its style's tempo.
#
#   substrike/tools/make-demos.sh
#
# Nothing is layered, edited or processed after the plugin. Each file gets one
# linear gain towards -16 LUFS, held back where that would push the true peak
# above -1 dBTP, and is encoded as MP3, 256 kbps joint stereo, 48 kHz.
#
# Not the shared make-demos.sh: that one renders sixteen seconds of a held
# note per preset at one tempo, which suits a pad and not a kick.
#
# Files are overwritten; nothing is deleted. Needs ffmpeg and a build with
# the tools (build/substrike-render).
set -euo pipefail
export LC_ALL=C   # decimal points for printf and the numbers ffmpeg prints

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "${here}/.." && pwd)"
root="$(cd "${src}/.." && pwd)"
render="${src}/build/substrike-render"
clap="${src}/build/Substrike.clap"
out="${root}/dist/demos/Substrike"
work="${src}/build/demo-work"
mkdir -p "$out" "$work"
command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }

# file name | preset load key | tempo (BPM) | hits (one per beat at that tempo)
demos=(
   "01-ConcreteThump-HardTechno|techno-concrete-thump|140|16"
   "02-BrickWall-HardTechno|techno-brick-wall|145|16"
   "03-HardRumble-Rumble|rumble-hard|140|16"
   "04-ClassicRumble-Rumble|rumble-classic|138|16"
   "05-RollingThunder-Rumble|rumble-rolling-thunder|140|16"
   "06-BerlinPressure-Techno|techno-berlin-pressure|132|16"
   "07-SchranzHammer-Schranz|techno-schranz-hammer|155|16"
   "08-DeepRound-House|house-deep-round|124|16"
   "09-BoomBapDust-HipHop|hiphop-boom-bap-dust|90|8"
   "10-SubBoomLong-Trap|trap-sub-boom-long|70|8"
   "11-UpliftingTail-Trance|trance-uplifting|138|16"
   "12-FullOnTok-Psytrance|psy-fullon-tok|145|16"
   "13-EuphoricHardstyle-Hardstyle|hardstyle-euphoric|150|16"
   "14-GabberClassic-Hardcore|hardcore-gabber-classic|180|16"
   "15-TightRoller-DrumAndBass|dnb-tight|87|8"
   "16-StudioKick-Acoustic|acoustic-studio|110|8"
   "17-DistantThunder-Ambient|ambient-distant-thunder|60|4"
   "18-ChaosEngine-Experimental|exp-chaos-engine|128|16"
)

for entry in "${demos[@]}"; do
   IFS='|' read -r file key bpm hits <<<"$entry"
   seconds="$(python3 -c "print(${hits} * 60.0 / ${bpm} + 2.5)")"
   wav="${work}/${file}.wav"
   "$render" --plugin "$clap" --preset "$key" --hits "$hits" --bpm "$bpm" --seconds "$seconds" \
      --rate 48000 --out "$wav" >/dev/null
   # One pass for the integrated loudness and the true peak.
   stats="$(ffmpeg -hide_banner -nostats -i "$wav" -filter_complex ebur128=peak=true -f null - 2>&1)"
   lufs="$(echo "$stats" | sed -n 's/^ *I: *\(-\?[0-9.]*\) LUFS.*/\1/p' | tail -1)"
   tp="$(echo "$stats" | sed -n 's/^ *Peak: *\(-\?[0-9.inf]*\) dBFS.*/\1/p' | tail -1)"
   gain="$(python3 -c "print(round(min(-16.0 - (${lufs}), -1.0 - (${tp})), 2))")"
   ffmpeg -hide_banner -loglevel error -y -i "$wav" -af "volume=${gain}dB" \
      -c:a libmp3lame -b:a 256k -joint_stereo 1 "${out}/${file}.mp3"
   printf '    %-38s %3s BPM  %6s LUFS  %6s dBTP  gain %+6.2f dB\n' "${file}.mp3" "$bpm" "$lufs" "$tp" "$gain"
done
