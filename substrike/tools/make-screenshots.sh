#!/usr/bin/env bash
#
# Regenerates the manual's screenshots into docs/images/.
#
#   substrike/tools/make-screenshots.sh [outdir]
#
# The manual embeds these as data URIs, so they are committed rather than
# built at release time. Run this by hand whenever the interface changes, and
# look at what comes out.
#
# The editor is rendered offscreen by substrike-gui-snapshot (no display, no
# window on anyone's desktop), with a factory preset loaded and a page
# selected; panels are cut out of the 1120 x 720 window at the coordinates of
# sectionsFor() in src/gui/Editor.cpp. If the layout moves, the constants
# below move with it. Everything is at scale 1, like the other manuals.
#
# Files are overwritten; nothing is deleted. Needs ImageMagick (convert) and
# a build with the tools (build/substrike-gui-snapshot).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "${here}/.." && pwd)"
out="${1:-${src}/docs/images}"
snap="${src}/build/substrike-gui-snapshot"
work="${src}/build/screenshot-work"
mkdir -p "$out" "$work"
[ -x "$snap" ] || { echo "no ${snap}: build with SUBSTRIKE_BUILD_TOOLS=ON" >&2; exit 1; }

# The sections, as x,y,w,h in the base window.
RACK=12,46,236,662
STRIP=256,46,852,100
SOURCE=256,154,852,262
CHAIN=256,424,852,276
RIGHT=256,46,852,662          # strip, source and chain together

shot() {  # shot <name> <snapshot args...>
   local name="$1"; shift
   "$snap" "${work}/${name}.png" "$@" >/dev/null
}
crop() {  # crop <from> <x,y,w,h> <to>
   IFS=, read -r x y w h <<<"$2"
   convert "${work}/$1.png" -crop "${w}x${h}+${x}+${y}" +repage "${out}/$3.png"
}

shot hard      --preset techno-concrete-thump --lane 1
shot hard-env  --preset techno-concrete-thump --lane 1 --tab amp
shot rumble3   --preset rumble-classic --lane 3
shot guard     --preset rumble-classic --lane 3 --slot guard
shot master    --preset rumble-classic --lane master
shot mod       --preset show-macro-performance --lane mod
shot env       --preset trap-glide-boom --lane mod --mod-tab 5
shot modknob   --preset show-wobble-tail --lane 1
shot browser   --preset rumble-hard --lane 1 --browser
shot kit       --preset show-kick-kit --lane 1

cp "${work}/hard.png" "${out}/window.png"
crop hard "$RACK" rack
crop hard "$STRIP" lane-strip
crop hard "$SOURCE" body
crop hard-env "$SOURCE" body-amp
crop hard "$CHAIN" chain
crop rumble3 "$RIGHT" bus-lane
crop guard "$CHAIN" guard
crop master "$STRIP" master
crop mod "$RIGHT" modulation
crop env "$SOURCE" envelope
crop modknob "$CHAIN" modulated-knob
crop browser "$RIGHT" browser

for f in "${out}"/*.png; do
   printf '    %-24s %s\n' "$(basename "$f")" "$(identify -format '%wx%h' "$f")"
done
