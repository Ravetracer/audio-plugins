#!/usr/bin/env bash
#
# Regenerates the manual's screenshots into docs/images/.
#
#   aurum/tools/make-screenshots.sh [outdir]
#
# The manual embeds these as data URIs, so they are committed rather than built
# at release time. Run this by hand whenever the interface changes, and look at
# what comes out -- a screenshot that quietly went stale is worse than none.
#
# It opens the real editor in clap_gui_host on a private Xvfb display -- never
# the desktop it is started from -- and drives it with xdotool. Panels are cut
# out of full-window captures at coordinates taken from Editor::layout(),
# sectionsFor() and PresetBrowser's constructor, all in the 1000 x 660 base
# size; if the layout moves, the constants below move with it.
#
# Everything is captured at scale 1, like the other plugins' manuals: the
# stylesheet prints a screenshot at its own pixel size, so the figures keep the
# proportions of the panels they show.
#
# Needs a build with the test programs (the default): AURUM_BUILD points at it,
# default aurum/build. Needs Xvfb, xdotool and ImageMagick.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_dir="$(cd "${here}/.." && pwd)"
out="${1:-${plugin_dir}/docs/images}"
build="${AURUM_BUILD:-${plugin_dir}/build}"

for tool in Xvfb xdotool import convert; do
   command -v "$tool" >/dev/null 2>&1 || { echo "${tool} not found" >&2; exit 1; }
done
for f in clap_gui_host Aurum.clap; do
   [ -e "${build}/${f}" ] || { echo "no ${f} in ${build} -- build with the test programs on" >&2; exit 1; }
done
mkdir -p "$out"

# A clean preset library and settings for the run. The browser shows whatever
# is in the user's own preset folder, and these pictures go into a manual that
# strangers read; the user's settings (window size, scaling, MIDI map) would
# change the pictures too.
scratch="$(mktemp -d)"
export XDG_CONFIG_HOME="${scratch}/config" XDG_DATA_HOME="${scratch}/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"

xvfb_pid=""
host_pid=""
cleanup() {
   [ -n "$host_pid" ] && kill "$host_pid" 2>/dev/null
   [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
   rm -rf "$scratch"
   true
}
trap cleanup EXIT

# A display of its own, numbered by the server.
exec 3<> <(:)
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
xvfb_pid=$!
read -r -t 10 display_num <&3 || { echo "Xvfb did not start" >&2; exit 1; }
exec 3<&-
export DISPLAY=":${display_num}"

# ----------------------------------------------------------------- the layout
#
# Base size 1000 x 660. Sections (sectionsFor): room {12,44,976,156},
# character {12,208,484,122}, output {504,208,484,122}, decay {12,338,484,310},
# tone {504,338,484,310}. Top bar buttons are 26 high at y 8.
PRESET_X=283; BAR_Y=21                # the preset name button
IO_X=896                              # I/O
MIDI_X=853                            # MIDI
MENU_X=973                            # the options menu
MIX_X=935; MIX_Y=255                  # the Mix knob
ALGO_X=350; ALGO_Y=137                # the Algorithm selector
BROWSER_MENU_X=122; BROWSER_MENU_Y=61 # the browser's own menu button
PARK_X=500; PARK_Y=654                # background between the panels: no tooltip
DECAY_CHIP_X=80; TONE_CHIP_X=572; CHIP_Y=596

ox=0; oy=0
start_host() {
   "${build}/clap_gui_host" "${build}/Aurum.clap" "$1" >/dev/null 2>&1 &
   host_pid=$!
   local w
   w="$(xdotool search --sync --name "Aurum test host" | head -1)"
   eval "$(xdotool getwindowgeometry --shell "$w")"
   ox=$X; oy=$Y
   [ "$WIDTH" = 1000 ] && [ "$HEIGHT" = 660 ] || {
      echo "unexpected window size ${WIDTH}x${HEIGHT}" >&2; exit 1; }
   sleep 1
}
stop_host() {
   kill "$host_pid" 2>/dev/null || true
   wait "$host_pid" 2>/dev/null || true
   host_pid=""
}
click() { xdotool mousemove $((ox + $1)) $((oy + $2)) click "${3:-1}"; sleep 0.4; }
park() { xdotool mousemove $((ox + PARK_X)) $((oy + PARK_Y)); sleep 0.3; }
# shot <name> [WxH+X+Y]: the whole window, or a piece of it.
shot() {
   local full="${scratch}/full.png"
   import -window root -crop "1000x660+${ox}+${oy}" +repage "$full"
   if [ -n "${2:-}" ]; then
      convert "$full" -crop "$2" +repage "${out}/$1.png"
   else
      cp "$full" "${out}/$1.png"
   fi
   echo "    $1.png"
}
load_preset() {
   click "$PRESET_X" "$BAR_Y"
   xdotool type --delay 30 "$1"
   sleep 0.3
   xdotool key Return
   sleep 0.6
   park
}

echo "==> screenshots into ${out}"
start_host 600

load_preset "Stone Church"
shot window
shot top-bar "1000x42+0+0"
shot panel-room "984x164+8+40"
shot panel-character "492x130+8+204"
shot panel-output "492x130+500+204"
click "$DECAY_CHIP_X" "$CHIP_Y"; park
shot decay-contour "492x318+8+334"

load_preset "Vocal Hall"
click "$TONE_CHIP_X" "$CHIP_Y"; park
shot tone-eq "492x318+500+334"

load_preset "Wide Space"
click "$TONE_CHIP_X" "$CHIP_Y"; park
shot tone-eq-side "492x318+500+334"

click "$ALGO_X" "$ALGO_Y"; park
shot algorithm-menu "470x210+8+40"
xdotool key Escape; click "$PARK_X" "$PARK_Y"

load_preset "Concert Hall"
click "$PRESET_X" "$BAR_Y"; park
shot preset-browser "808x468+96+36"
click "$BROWSER_MENU_X" "$BROWSER_MENU_Y"; park
shot browser-menu "316x336+96+36"
xdotool key Escape; sleep 0.3; xdotool key Escape; sleep 0.3

click "$IO_X" "$BAR_Y"; park
shot io-panel "348x160+652+0"
xdotool key Escape; click "$PARK_X" "$PARK_Y"

click "$MENU_X" "$BAR_Y"; park
shot options-menu "300x150+700+0"
xdotool key Escape; click "$PARK_X" "$PARK_Y"
stop_host

# MIDI learn needs a controller. The test host sends a CC ramp (AURUM_TEST_CC)
# between 3 and 4.5 seconds after it starts, so learning is switched on and the
# Mix knob picked before then, and the picture taken after.
export AURUM_TEST_CC=21
start_host 12
click "$MIDI_X" "$BAR_Y"
click "$MIX_X" "$MIX_Y"
sleep 4.5
park
shot midi-learn
click "$MIDI_X" "$BAR_Y"
stop_host
unset AURUM_TEST_CC

echo "==> done"
