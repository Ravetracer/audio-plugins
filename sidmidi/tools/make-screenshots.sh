#!/usr/bin/env bash
#
# Regenerates the manual's screenshots into docs/images/.
#
#   sidmidi/tools/make-screenshots.sh [outdir]
#
# The window is rendered offscreen by sidmidi-gui-snapshot -- no display, no
# window on anyone's desktop -- with a program loaded and run offline, so the
# same build gives the same pictures. The manual embeds them as data URIs,
# so they are committed; run this by hand when the interface changes, and
# look at what comes out.
#
# The programs and ROMs are the developer's own (SIDMIDI_SHOT_ROMS, default
# _dev_/roms; the programs in _dev_). None of them is shipped; the pictures
# show them running. Files are overwritten; nothing is deleted. Needs a build
# with the tools (build/sidmidi-gui-snapshot) and ImageMagick (identify).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "${here}/.." && pwd)"
out="${1:-${src}/docs/images}"
snap="${src}/build/sidmidi-gui-snapshot"
roms="${SIDMIDI_SHOT_ROMS:-${src}/_dev_/roms}"
work="${src}/build/screenshot-work"
mkdir -p "$out" "$work/ccmaps"
[ -x "$snap" ] || { echo "no ${snap}: build with the tools" >&2; exit 1; }

# Only the factory maps: an empty user map folder.
export SIDMIDI_CCMAP_DIR="${work}/ccmaps"

shot() {  # shot <name> <rom dir> <snapshot args...>
   local name="$1" romdir="$2"; shift 2
   SIDMIDI_ROM_DIR="$romdir" timeout 300 "$snap" "${out}/${name}.png" "$@" >/dev/null
}
# The ROM page shows the default ROM folder as a user would see it -- never
# the developer's own paths (the folder cannot be created there; it does not
# need to be).
shot_no_roms() {  # shot_no_roms <name> <snapshot args...>
   local name="$1"; shift
   env -u SIDMIDI_ROM_DIR -u XDG_DATA_HOME HOME=/home/user timeout 300 "$snap" "${out}/${name}.png" "$@" >/dev/null
}

shot window  "$roms" --load "${src}/_dev_/cynthcart/bin/cynthcart.prg" --seconds 6
shot sidsynth "$roms" --load "${src}/_dev_/SID_Synth/sidsynth.d64" --seconds 10
shot cc      "$roms" --load "${src}/_dev_/SID_Synth/sidsynth.d64" --seconds 10 --page cc
shot machine "$roms" --load "${src}/_dev_/cynthcart/bin/cynthcart.prg" --seconds 6 --page machine \
             --sids 2 --model 2:8580R5
shot disk    "$roms" --seconds 3 --open "${src}/_dev_/SID_Synth/sidsynth.d64"
shot_no_roms roms --seconds 0

for f in window sidsynth cc machine disk roms; do
   printf '    %-16s %s\n' "${f}.png" "$(identify -format '%wx%h' "${out}/${f}.png")"
done
