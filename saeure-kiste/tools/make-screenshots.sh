#!/usr/bin/env bash
#
# Regenerates the manual's screenshots into docs/images/.
#
#   ./tools/make-screenshots.sh [outdir]
#
# The manual embeds these as data URIs, so they are committed rather than built
# at release time: `release.sh` runs on machines with no X display and must not
# depend on one. Run this by hand whenever the interface changes, and look at
# what comes out -- a screenshot that quietly went stale is worse than none.
#
# It drives the real editor through `saeurekiste-guihost`, the same standalone
# host the GUI is developed against, and cuts the panels out of full-window
# captures at coordinates taken from the layout in src/gui/gui.cpp. If a panel
# moves, the constants below move with it; there is no way to ask the window
# where something is from outside it.
#
# **Everything is captured at scale 1 on purpose.** The editor can be asked for
# more -- `guihost --scale 1.5` -- and the result is sharper in print, but the
# expanded window is 987 pixels tall at scale 1 and a window manager will not
# hand out anything past the usable height of the screen. At 1.5 the bottom row
# of the collapsible half is simply cut off, which is exactly the half that
# needs photographing. One scale for every image beats a sharp one for some of
# them.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_dir="$(cd "${here}/.." && pwd)"
out="${1:-${plugin_dir}/docs/images}"
build="${plugin_dir}/build"

for tool in xdotool import convert; do
   command -v "$tool" >/dev/null 2>&1 || { echo "${tool} not found" >&2; exit 1; }
done
[ -x "${build}/saeurekiste-guihost" ] || {
   echo "no guihost at ${build} -- configure with -DSAEUREKISTE_BUILD_TOOLS=ON" >&2; exit 1; }

mkdir -p "$out"

# A clean preset library for the whole run.
#
# The browser shows whatever is in the user's own preset directory, and a
# screenshot of it is going into a manual that strangers read. Without this it
# shipped a picture of one of the author's own presets. Pointing
# XDG_CONFIG_HOME at an empty directory leaves the real one untouched and gives
# every machine the same factory-only library to photograph.
clean_config="$(mktemp -d)"
export XDG_CONFIG_HOME="$clean_config"

# ----------------------------------------------------------------- the layout
#
# From gui.cpp: kMargin 16, kGap 8, kCellW 84, kPanelPad 8, kPanelTitleH 24,
# kCellH 96, kHeaderH 66, kBarH 32, kSeqPaneHeight 291, kContentW 1408. A
# panel is cols*84 + 16 wide, except the last on a row, which is stretched to
# the right margin.
WIN_W=1440; WIN_H=715; WIN_H_OPEN=987
ROW0_Y=74; ROW1_Y=210; PANEL_H=128
SEQ_Y=346; SEQ_H=291; BANK_X=1192; BANK_W=232; SEQ_W=1168
BAR_Y=645; ADV_ROW2_Y=685; ADV_ROW3_Y=821

# Where the things this script clicks are, so a coordinate appears once.
MODE_CHIP_X=98; MODE_CHIP_Y=261      # the SEQUENCER panel's Mode chip, right arrow
ADVANCED_X=570; ADVANCED_Y=663       # the ADVANCED button on the preset bar
MAP_X=826; PREV_X=740; NEXT_X=782; ROW_Y=352
PRESET_NAME_X=258; PRESET_NAME_Y=663

host_pid=""
host_started=0
window=""

cleanup() {
   [ -n "$host_pid" ] && kill "$host_pid" 2>/dev/null
   [ -n "${clean_config:-}" ] && rm -rf "$clean_config"
   true
}
trap cleanup EXIT

# Starts the editor and finds its window.
#
# By size, not by name: the search matches any window with the plugin's name in
# its title, and a file manager sitting on the preset folder is enough to be
# picked instead -- which does not fail, it silently photographs the wrong
# thing.
start_host() {
   local want_h="$1"; shift
   "${build}/saeurekiste-guihost" "${build}/SaeureKiste.clap" "" 120 "$@" >/dev/null 2>&1 &
   host_pid=$!
   host_started=$(date +%s)
   window=""
   for _ in $(seq 1 40); do
      sleep 0.5
      for w in $(xdotool search --name SaeureKiste 2>/dev/null); do
         local geom
         geom="$(xdotool getwindowgeometry --shell "$w" 2>/dev/null)" || continue
         eval "$geom"
         if [ "${WIDTH:-0}" = "$WIN_W" ] && [ "${HEIGHT:-0}" = "$want_h" ]; then
            window="$w"; WX="$X"; WY="$Y"; return 0
         fi
      done
   done
   echo "no ${WIN_W}x${want_h} editor window appeared" >&2
   exit 1
}

stop_host() { kill "$host_pid" 2>/dev/null || true; host_pid=""; sleep 1; }

# Sleeps until N seconds after the host started.
#
# The notes --note plays are on the host's own clock, and the steps that arm a
# target for them are on this script's. Timing the two against each other by
# counting sleeps here does not survive a capture taking longer than it did
# last time -- which is how the first run of this bound two pads to the wrong
# things and photographed the result without complaining. So every learn step
# is placed against the host's clock instead.
wait_until() {
   local target="$1" now
   while :; do
      now=$(( $(date +%s) - host_started ))
      [ "$now" -ge "$target" ] && return 0
      sleep 0.5
   done
}

click() { xdotool mousemove $((WX + $1)) $((WY + $2)) click 1; sleep "${3:-0.4}"; }

# One crop out of a fresh capture of the whole window. The pointer is parked
# off every control first, so nothing is photographed in its hover state.
# Off every control, so nothing is photographed lit up under the pointer.
park() { xdotool mousemove $((WX + WIN_W - 4)) $((WY + 4)); sleep 0.35; }

grab() {
   local name="$1" x="$2" y="$3" w="$4" h="$5"
   park
   import -window "$window" "${out}/.full.png"
   convert "${out}/.full.png" -crop "${w}x${h}+${x}+${y}" +repage "${out}/${name}.png"
   printf '    %-28s %sx%s\n' "${name}.png" "$w" "$h"
}

# --------------------------------------------------------- the closed window
start_host "$WIN_H"

grab window            0        0        $WIN_W $WIN_H
grab panel-vco         16       $ROW0_Y  184    $PANEL_H
grab panel-vcf         208      $ROW0_Y  604    $PANEL_H
grab panel-drive       820      $ROW0_Y  436    $PANEL_H
grab panel-output      1264     $ROW0_Y  160    $PANEL_H
grab panel-sequencer   16       $ROW1_Y  352    $PANEL_H
grab panel-generator   376      $ROW1_Y  520    $PANEL_H
grab panel-delay       904      $ROW1_Y  520    $PANEL_H
grab grid              16       $SEQ_Y   $SEQ_W $SEQ_H
grab bank              $BANK_X  $SEQ_Y   $BANK_W $SEQ_H
grab preset-bar        16       $BAR_Y   1408   40

# The long note. Drag along one row of the piano roll and the window writes the
# note on every step with a slide out of all but the last, and draws the run as
# one bar -- which is the thing the manual has to show rather than describe.
xdotool mousemove $((WX + 299)) $((WY + 440)) mousedown 1
for x in 320 360 400 450 500 560 626; do
   xdotool mousemove $((WX + x)) $((WY + 440)); sleep 0.05
done
xdotool mouseup 1; sleep 0.5
grab grid-long-note    16       $SEQ_Y   $SEQ_W $SEQ_H
# And the same run split in two by taking one slide out of the middle.
click 429 577 0.5
grab grid-note-split   16       $SEQ_Y   $SEQ_W $SEQ_H

stop_host

# ------------------------------------------------------------------ live mode
#
# Three notes, played into the editor on a timer, because the pattern map is
# learned by playing the note a pad should send and there is no other way to
# reach that half of the window from a script.
start_host "$WIN_H" --note 48@20 --note 50@28 --note 52@36

click $MODE_CHIP_X $MODE_CHIP_Y 0.3
click $MODE_CHIP_X $MODE_CHIP_Y 0.3          # MIDI -> Sequencer -> Live
grab panel-sequencer-live 16 $ROW1_Y 352 $PANEL_H
click $MAP_X $ROW_Y 0.4                      # the bank becomes the pad layout

wait_until 17; click 1319 379 0.3            # aim at pattern 5; note 48 at 20
wait_until 25; click $PREV_X $ROW_Y 0.3      # aim at PREV;      note 50 at 28
wait_until 33; click $NEXT_X $ROW_Y 0.3      # aim at NEXT;      note 52 at 36
wait_until 38

grab live-map          $BANK_X $SEQ_Y $BANK_W $SEQ_H
grab live-row          600      $SEQ_Y   600  26
click $MAP_X $ROW_Y 0.4                      # back to the ordinary bank
grab live-row-plain    600      $SEQ_Y   600  26

# What was learned, checked rather than assumed: three pads, and the one on
# pattern 5 is the one this is hardest to get right.
learned=$(convert "${out}/live-map.png" -crop 27x20+107+22 +repage -colorspace gray \
   -format '%[fx:mean]' info:)
awk -v m="$learned" 'BEGIN { exit (m > 0.10) ? 0 : 1 }' || {
   echo "pattern 5 has no note on it -- the learn did not take" >&2; exit 1; }

stop_host

# ---------------------------------------------------------- the preset browser
#
# The browser is a popup and only half of its rectangle is a constant.
# browserPanel() puts it at kMargin + 40 with contentW - 80 of width -- those
# never move -- but sizes its *height* from how many presets and folders the
# library has, so a hard-coded crop is wrong the moment somebody adds a shelf.
# The first attempt guessed the whole rectangle and shipped a picture of the
# browser's bottom-left corner.
#
# Diffing an open capture against a closed one does not help either: opening
# the browser dims the whole window behind it, so everything differs. What does
# work is a one-pixel column down the middle of the panel: inside it the fill
# is well above the dimmed backdrop, and the bounding box of that run is the
# browser's top and height.
BROWSER_X=56; BROWSER_W=1328

start_host "$WIN_H"
click $PRESET_NAME_X $PRESET_NAME_Y 0.8
park
import -window "$window" "${out}/.open.png"

span="$(convert "${out}/.open.png" -crop "1x${WIN_H}+$((BROWSER_X + BROWSER_W / 2))+0" +repage \
   -colorspace gray -threshold 15% -format '%@' info:)"     # WxH+X+Y
bh=${span#*x}; bh=${bh%%+*}
by=${span##*+}
[ "${bh:-0}" -gt 200 ] || { echo "the preset browser did not open (height ${bh:-0})" >&2; exit 1; }

pad=5
convert "${out}/.open.png" \
   -crop "$((BROWSER_W + 2 * pad))x$((bh + 2 * pad))+$((BROWSER_X - pad))+$((by - pad))" \
   +repage "${out}/preset-browser.png"
printf '    %-28s %s\n' "preset-browser.png" \
   "$(identify -format '%wx%h' "${out}/preset-browser.png")"
rm -f "${out}/.open.png"
stop_host

# ------------------------------------------------------- the collapsible half
start_host "$WIN_H"
click $ADVANCED_X $ADVANCED_Y 1.5
window=""
for w in $(xdotool search --name SaeureKiste 2>/dev/null); do
   geom="$(xdotool getwindowgeometry --shell "$w" 2>/dev/null)" || continue
   eval "$geom"
   [ "${WIDTH:-0}" = "$WIN_W" ] && [ "${HEIGHT:-0}" = "$WIN_H_OPEN" ] && { window="$w"; WX="$X"; WY="$Y"; }
done
[ -n "$window" ] || { echo "the editor did not grow when ADVANCED was pressed" >&2; exit 1; }

grab window-advanced   0        0        $WIN_W $WIN_H_OPEN
grab panel-mods        16       $ADV_ROW2_Y 856 $PANEL_H
grab panel-slide       880      $ADV_ROW2_Y 544 $PANEL_H
grab panel-accent      16       $ADV_ROW3_Y 352 $PANEL_H
grab panel-vibrato     376      $ADV_ROW3_Y 268 $PANEL_H
grab panel-amp         652      $ADV_ROW3_Y 772 $PANEL_H
stop_host

rm -f "${out}/.full.png"
echo
echo "screenshots written to ${out}"
