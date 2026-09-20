#!/usr/bin/env bash
#
# Two instances of the plugin in one process, each with its own editor, checked
# for state leaking from one to the other.
#
# A plugin binary is loaded once and instantiated many times, so anything at
# file scope -- a static, a global, a function-local `static` -- is shared by
# every track using the plugin. That cannot be seen from one instance, it does
# not show up in the offline self-test, and it does not exist on the audio side
# at all: it is a GUI-layer fault, and this is what finds it.
#
# What it does:
#   1. opens two instances with two different presets, side by side;
#   2. photographs the step grid of each and requires them to differ, because
#      the presets do;
#   3. clicks a pattern slot in one and requires the other not to move;
#   4. clicks a step in the other and requires the first not to move.
#
# The first of those is the decisive one: with the bank shared, both editors
# draw the same grid from the moment they open. The clicks that follow confirm
# that an edit reaches the instance it was aimed at and nothing bleeds out of
# it, but a window that simply has not repainted yet would pass them, so do not
# read a pass there as proof on its own.
#
# Needs X, xdotool and ImageMagick. Run it from a build directory:
#
#   ../tools/check-instances.sh
#
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin="${1:-./SaeureKiste.clap}"
host="${2:-./saeurekiste-guihost}"
presets="$here/../presets"
work="$(mktemp -d)"
trap 'pkill -x "$(basename "$host")" >/dev/null 2>&1; rm -rf "$work"' EXIT

for tool in xdotool import convert compare; do
   command -v "$tool" >/dev/null || { echo "need $tool"; exit 1; }
done
[ -e "$plugin" ] || { echo "no plugin at $plugin"; exit 1; }
[ -x "$host" ] || { echo "no guihost at $host"; exit 1; }

# The two presets differ in every step of the pattern, which is what makes a
# shared bank visible as two identical grids.
"$host" "$plugin" "$presets/blank_slate.saeurekiste" 120 \
   --also "$presets/teeth.saeurekiste" >"$work/host.log" 2>&1 &
sleep 4

win() { xdotool search --name "SaeureKiste #$1" | head -1; }
W1="$(win 1)"; W2="$(win 2)"
[ -n "$W1" ] && [ -n "$W2" ] || { echo "the two windows did not open"; cat "$work/host.log"; exit 1; }

origin() { eval "$(xdotool getwindowgeometry --shell "$1" | grep -E '^X=|^Y=')"; echo "$X $Y"; }
shot()   { import -window "$1" "$work/$2.png"; }
# The step grid and the pattern bank, as they sit in the window. These are the
# window's own design pixels: it opens at scale 1, and the numbers move when
# the layout does -- the whole set below has been shifted 84 px right twice, once when
# the drive panel grew a column in 0.5.0 and again when it grew another in
# 0.6.0, so if this check suddenly reports that
# nothing registers, compare the crops against a screenshot before believing it.
grid()   { convert "$work/$1.png" -crop 1114x300+10+345 "$work/$1-grid.png"; }
bank()   { convert "$work/$1.png" -crop 240x40+1132+365 "$work/$1-bank.png"; }
differs() { compare -metric AE "$work/$1.png" "$work/$2.png" null: 2>&1 || true; }

fails=0
ok()   { echo "  [ok] $1"; }
bad()  { echo "  [FAIL] $1"; fails=$((fails + 1)); }

# --- 1. two presets, two different grids.
shot "$W1" a1; shot "$W2" a2; grid a1; grid a2
n="$(differs a1-grid a2-grid)"
echo "       two presets, grids differ by $n pixels"
[ "${n:-0}" -gt 0 ] && ok "each instance draws its own pattern bank" \
                    || bad "both instances draw the same pattern bank"

# --- 2. a pattern slot clicked in instance 2 must not move instance 1.
read -r X2 Y2 <<<"$(origin "$W2")"
xdotool mousemove $((X2 + 1264)) $((Y2 + 380)); sleep 0.3; xdotool click 1; sleep 1
shot "$W1" b1; shot "$W2" b2; bank a1; bank a2; bank b1; bank b2
moved="$(differs a2-bank b2-bank)"; stayed="$(differs a1-bank b1-bank)"
echo "       pattern clicked in #2: #2 moved by $moved px, #1 by $stayed px"
[ "${moved:-0}" -gt 0 ] && ok "the click reached the instance it was aimed at" \
                        || bad "the click did not register at all"
[ "${stayed:-0}" -eq 0 ] && ok "selecting a pattern in one instance leaves the other alone" \
                         || bad "selecting a pattern in one instance moved the other"

# --- 3. and a step edited in one must not appear in the other. Instance 2 is
# on top, so it is the one that can be clicked without restacking.
xdotool mousemove $((X2 + 235)) $((Y2 + 453)); sleep 0.3; xdotool click 1; sleep 1
shot "$W1" c1; shot "$W2" c2; grid b2; grid c2; grid b1; grid c1
moved="$(differs b2-grid c2-grid)"; stayed="$(differs b1-grid c1-grid)"
echo "       step edited in #2: #2 changed by $moved px, #1 by $stayed px"
[ "${moved:-0}" -gt 0 ] && ok "the step edit reached the instance it was aimed at" \
                        || bad "the step edit did not register at all"
[ "${stayed:-0}" -eq 0 ] && ok "editing a step in one instance leaves the other alone" \
                         || bad "editing a step in one instance changed the other"

echo
echo "instance check: $fails failure(s)"
[ "$fails" -eq 0 ]
