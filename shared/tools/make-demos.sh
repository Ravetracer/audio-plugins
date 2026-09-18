#!/bin/bash
#
# Renders the website demo audio for one plugin into dist/demos/<Plugin>/.
#
# One MP3 per factory preset, straight out of the plugin's own offline renderer:
# 16 seconds held plus a 3 second tail, 48 kHz, MP3 256 kbps joint stereo. There
# is no editing, no layering and no dynamics processing. The only post-step is a
# single linear gain towards -16 LUFS, backed off if that would push the true
# peak above -1 dBTP, so the relative loudness the presets were fitted to
# survives and a sparse preset stays quiet.
#
# The seed is pinned and sox's dither is off, so a re-run reproduces the files.
#
# It also updates dist/demos/demos.json and dist/demos/README.md in place. Only
# this plugin's section is touched; the other plugins' entries and the README's
# hand-written closing notes are left exactly as they are.
#
# Each demo's blurb comes from <plugin>/presets/demo-descriptions.txt if that
# file has a line for the preset, and from the preset file's own description if
# it does not. The two are written for different readers: a preset description
# is documentation and says what was measured and why, and the website wants a
# sentence a musician can read. Keeping them apart is why the override exists --
# the plugin's own documentation does not have to be dumbed down to make the
# site readable. The format is one "Preset Name = text" per line, blank lines
# and # comments ignored.
#
# Every new plugin gets a run of this and a section in _designs/website-copy.md.
# See "The website material" in the suite CLAUDE.md.
#
# Needs ffmpeg, sox, lame and python3. Run it from anywhere:
#
#    shared/tools/make-demos.sh <plugin-folder> [--build DIR] [--seed N]
#                                               [--simulates TEXT] [--text-only]
#
# --text-only refreshes demos.json and README.md from the preset files and the
# override without re-rendering anything, which is what a wording change needs.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
suite_dir="$(dirname "$(dirname "$here")")"

plugin=""
build_dir=""
seed=7
simulates=""
text_only=0

while [ $# -gt 0 ]; do
   case "$1" in
      --build)     build_dir="$2"; shift 2 ;;
      --seed)      seed="$2"; shift 2 ;;
      --simulates) simulates="$2"; shift 2 ;;
      --text-only) text_only=1; shift ;;
      -*)          echo "unknown option: $1" >&2; exit 2 ;;
      *)           plugin="$1"; shift ;;
   esac
done

[ -n "$plugin" ] || { echo "usage: make-demos.sh <plugin-folder> [--build DIR] [--seed N] [--simulates TEXT] [--text-only]" >&2; exit 2; }

plugin_dir="${suite_dir}/${plugin}"
[ -d "$plugin_dir" ] || { echo "no such plugin folder: $plugin_dir" >&2; exit 1; }
build_dir="${build_dir:-${plugin_dir}/build}"

# The display name is the CMake project name, which is also what the .clap is
# called. Nothing here hardcodes a plugin.
name="$(sed -n 's/^project(\([A-Za-z0-9_]*\) .*/\1/p' "${plugin_dir}/CMakeLists.txt" | head -1)"
[ -n "$name" ] || { echo "could not read the project name from ${plugin_dir}/CMakeLists.txt" >&2; exit 1; }

render="${build_dir}/${plugin}-render"
clap="${build_dir}/${name}.clap"
if [ "$text_only" -eq 0 ]; then
   for f in "$render" "$clap"; do
      [ -f "$f" ] || { echo "missing: $f -- build the plugin first" >&2; exit 1; }
   done
   for t in ffmpeg sox lame; do
      command -v "$t" >/dev/null || { echo "$t is required" >&2; exit 1; }
   done
fi
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }

# What the plugin simulates, for the section heading. Whatever demos.json
# already says wins, because those headings were written by hand and read
# better than the README table's terse "winds, storms". A plugin with no entry
# yet falls back to the table, and --simulates overrides both.
if [ -z "$simulates" ] && [ -f "${suite_dir}/dist/demos/demos.json" ]; then
   simulates="$(python3 - "${suite_dir}/dist/demos/demos.json" "$name" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
print(data.get(sys.argv[2], {}).get("simulates", ""))
PY
)"
fi
if [ -z "$simulates" ]; then
   simulates="$(python3 - "$suite_dir/README.md" "$plugin" <<'PY'
import re, sys
readme, folder = sys.argv[1], sys.argv[2]
for line in open(readme, encoding="utf-8"):
    if line.startswith("|") and f"]({folder}/)" in line:
        cols = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cols) >= 2:
            print(re.sub(r"\*|\(|\)", "", cols[1]).strip())
        break
PY
)"
fi
[ -n "$simulates" ] || { echo "could not work out what $name simulates; pass --simulates" >&2; exit 1; }

out="${suite_dir}/dist/demos/${name}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"

if [ "$text_only" -eq 1 ]; then
   echo "==> ${name}: refreshing the text only, leaving the audio alone"
else

echo "==> rendering ${name}'s presets"
"$render" --plugin "$clap" --all --outdir "$work" \
   --seconds 16 --tail 3 --rate 48000 --param randomseed="$seed" >/dev/null

shopt -s nullglob
wavs=("$work"/*.wav)
[ ${#wavs[@]} -gt 0 ] || { echo "the renderer produced nothing" >&2; exit 1; }

echo "==> normalising and encoding ${#wavs[@]} demos"
for wav in "${wavs[@]}"; do
   base="$(basename "$wav" .wav)"
   # One ebur128 pass gives both the integrated loudness and the true peak, so
   # the gain and the ceiling are decided together and applied once.
   read -r loudness truepeak <<<"$(ffmpeg -nostats -hide_banner -i "$wav" \
      -filter_complex ebur128=peak=true -f null - 2>&1 |
      awk '/Integrated loudness:/{s=1} s&&/^ *I: /{i=$2} /True peak:/{p=1} p&&/^ *Peak: /{t=$2} END{print i, t}')"
   gain="$(python3 -c "
i, t = float('$loudness'), float('$truepeak')
g = -16.0 - i
if t + g > -1.0:
    g = -1.0 - t
print(f'{g:.2f}')")"
   sox -D "$wav" -t wav - gain "$gain" |
      lame --quiet -b 256 -m j -h - "${out}/${base}.mp3"
   printf '    %-34s %+7s dB\n' "${base}.mp3" "$gain"
done

fi

echo "==> updating dist/demos/demos.json and README.md"
python3 - "$suite_dir" "$plugin" "$name" "$simulates" <<'PY'
import json, os, re, sys

suite, folder, name, simulates = sys.argv[1:5]
presets = os.path.join(suite, folder, "presets")
demos_dir = os.path.join(suite, "dist", "demos")

def field(path, key):
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith(key + " = "):
                return line.split(" = ", 1)[1].strip()
    return ""

# The website's own wording, where there is any. One "Preset Name = text" per
# line; anything without a line here falls back to the preset's description.
overrides = {}
opath = os.path.join(presets, "demo-descriptions.txt")
if os.path.exists(opath):
    with open(opath, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or " = " not in line:
                continue
            k, v = line.split(" = ", 1)
            overrides[k.strip()] = v.strip()

def safe(s):
    return "".join(c if c.isalnum() else "_" for c in s)

entries = []
for f in sorted(os.listdir(presets)):
    path = os.path.join(presets, f)
    title = field(path, "name")
    if not title:
        continue
    mp3 = safe(title) + ".mp3"
    if not os.path.exists(os.path.join(demos_dir, name, mp3)):
        print(f"    warning: no demo rendered for {title!r}", file=sys.stderr)
        continue
    entries.append({"preset": title, "file": mp3,
                    "description": overrides.get(title) or field(path, "description")})
entries.sort(key=lambda e: e["preset"])

# demos.json -- this plugin's entry replaced or appended, the others untouched.
jpath = os.path.join(demos_dir, "demos.json")
data = {}
if os.path.exists(jpath):
    with open(jpath, encoding="utf-8") as fh:
        data = json.load(fh)
data[name] = {"simulates": simulates, "demos": entries}
with open(jpath, "w", encoding="utf-8") as fh:
    json.dump(data, fh, indent=2, ensure_ascii=False)
    fh.write("\n")

# README.md -- only this plugin's "## <Name> — <simulates>" section is written.
# A new one goes in ahead of the hand-written closing notes, which are left be.
rpath = os.path.join(demos_dir, "README.md")
section = [f"## {name} — {simulates}", "",
           "| Preset | File | Description |", "|---|---|---|"]
section += [f"| {e['preset']} | `{e['file']}` | {e['description']} |" for e in entries]
block = "\n".join(section) + "\n"

text = open(rpath, encoding="utf-8").read() if os.path.exists(rpath) else ""
pattern = re.compile(rf"^## {re.escape(name)} — .*?(?=^## |\Z)", re.S | re.M)
if pattern.search(text):
    text = pattern.sub(block + "\n", text, count=1)
else:
    tail = re.search(r"^## Demos that are not a single plain render", text, re.M)
    if tail:
        text = text[:tail.start()] + block + "\n" + text[tail.start():]
    else:
        text = text.rstrip("\n") + "\n\n" + block
open(rpath, "w", encoding="utf-8").write(text)
missing = [e["preset"] for e in entries if e["preset"] not in overrides]
if missing and overrides:
    print(f"    no website wording for: {', '.join(missing)}", file=sys.stderr)
print(f"    {len(entries)} demos recorded for {name}"
      f" ({len(entries) - len(missing)} with their own website wording)")
PY

echo
echo "${name} demos: ${out}"
