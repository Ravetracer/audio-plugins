#!/usr/bin/env bash
#
# Renders the manual's screenshots into docs/images/.
#
#   tools/make-screenshots.sh [build-dir]
#
# Everything is offscreen: a private Xvfb, scratch XDG_CONFIG_HOME and
# XDG_DATA_HOME, and the plugin's own gui host. Nothing here touches the
# desktop, and a run leaves nothing behind but the PNGs -- which is the rule
# Aurum and Substrike follow for the same reason: a screenshot script that
# needs a session cannot be run in a release, and one that shares a config
# directory writes into the user's own plugin settings.
#
# Needs Xvfb, xdotool and ImageMagick's `import`, all of which the release
# machine already has for the other plugins.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_dir="$(dirname "$here")"
build_dir="${1:-${plugin_dir}/build}"
out_dir="${plugin_dir}/docs/images"

[ -f "${build_dir}/Shapeau.clap" ] || { echo "missing ${build_dir}/Shapeau.clap -- build first" >&2; exit 1; }
[ -x "${build_dir}/shapeau-guihost" ] || { echo "missing ${build_dir}/shapeau-guihost" >&2; exit 1; }
for tool in Xvfb xdotool import; do
   command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 1; }
done

mkdir -p "$out_dir"
scratch="$(mktemp -d)"
display=":${SHAPEAU_SHOT_DISPLAY:-97}"

Xvfb "$display" -screen 0 1600x1100x24 >/dev/null 2>&1 &
xvfb_pid=$!
trap 'kill ${gui_pid:-} "$xvfb_pid" 2>/dev/null; rm -rf "$scratch"' EXIT
sleep 2

export DISPLAY="$display"
export XDG_CONFIG_HOME="${scratch}/config"
export XDG_DATA_HOME="${scratch}/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"

start_host() {
   "${build_dir}/shapeau-guihost" "${build_dir}/Shapeau.clap" 25 >"${scratch}/guihost.log" 2>&1 &
   gui_pid=$!
   sleep 3
   window="$(xdotool search --name Shapeau | tail -1)"
   [ -n "$window" ] || { echo "the window never appeared" >&2; cat "${scratch}/guihost.log" >&2; exit 1; }
}

start_host

# Coordinates are the window's own, at its 1x scale. The effect bar sits at
# y = 90: the chain's tabs from x = 26, 89 px apart, then the + tab; the band
# tabs (ALL 685, LOW 736, MID 787, HIGH 838), SNAP 901, STEP 961 and SHAPE 1044
# stay where they are. The shape menu's rows are 18 px apart from y = 112. An
# empty chain shows seven buttons across the graph at y = 309, VOLUME at
# x = 343 and 130 px apart in the order the bar lists them. The graph is
# x 60..1406, y 112..486. The host plays a drum loop, which is what the scope
# behind the shape shows.
tab_y=90
picker_y=309

step() { # step <file> <description>
   import -window "$window" "${out_dir}/$1"
   printf '    %-18s %s\n' "$1" "$2"
}

fresh_host() {
   kill "$gui_pid" 2>/dev/null || true
   wait "$gui_pid" 2>/dev/null || true
   sleep 1
   start_host
}

step window.png "the window as it opens: an empty chain and its picker"

# A ducking shape on the low band, which is what the plugin is mostly used for:
# the selected band draws thick, the other two stay flat and dim underneath,
# and the kick it ducks for is in the scope behind it.
xdotool mousemove --window "$window" 343 "$picker_y" click 1 # VOLUME
sleep 0.4
xdotool mousemove --window "$window" 736 "$tab_y" click 1 # the LOW band
sleep 0.3
xdotool mousemove --window "$window" 1044 "$tab_y" click 1 # SHAPE
sleep 0.3
xdotool mousemove --window "$window" 1040 130 click 1 # DUCK
sleep 0.3
xdotool mousemove --window "$window" 700 560
sleep 1.5
step curve.png "a duck drawn on the low band, over the drum loop it ducks"

# A chain of three, for the effect bar: + adds the first effect not in the
# chain yet, twice.
for _ in 1 2; do
   case $_ in 1) plus_x=128 ;; 2) plus_x=217 ;; esac
   xdotool mousemove --window "$window" "$plus_x" "$tab_y" click 1
   sleep 0.3
   xdotool mousemove --window "$window" "$((plus_x + 20))" 117 click 1
   sleep 0.5
done
xdotool mousemove --window "$window" 700 560
sleep 1.5
step chain.png "a chain of three effects, the last one selected"

# A filter sweep drawn by hand on the mid band: click adds a point, and the
# corner weights are what make a drawn line read as a sweep rather than a
# zigzag.
fresh_host
xdotool mousemove --window "$window" 473 "$picker_y" click 1 # FILTER
sleep 0.4
xdotool mousemove --window "$window" 787 "$tab_y" click 1 # the MID band
sleep 0.2
for point in "230 420" "520 220" "820 420" "1150 180"; do
   set -- $point
   xdotool mousemove --window "$window" "$1" "$2" click 1
   sleep 0.25
done
xdotool mousemove --window "$window" 700 560
sleep 1.5
step filter.png "a cutoff sweep drawn on the filter's mid band"

echo "    -> $out_dir"
