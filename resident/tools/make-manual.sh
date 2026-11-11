#!/usr/bin/env bash
#
# Builds ReSIDent's manual as PDF, and the HTML it was rendered from.
#
#   resident/tools/make-manual.sh [outdir]
#
# ReSIDent is not built on shared/, so the shared docgen cannot read its
# parameters. tools/docgen.cpp is compiled here straight from ReSIDent's
# sources instead (release builds switch the offline tools off) and writes
# the parameter table and the CC map library, from ccmaps/*.ccmap; the
# rendering is the shared manual.py and manual.css, so the manuals look alike.
# release.sh calls this script for ReSIDent rather than the shared one.
#
# Intermediate files go to build/manual-work and are overwritten, never
# deleted. Needs: a C++20 compiler, cmake, python3 with the markdown module,
# and wkhtmltopdf.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"      # resident/tools
src="$(cd "${here}/.." && pwd)"                            # resident
root="$(cd "${src}/.." && pwd)"                            # the repository
shared_tools="${root}/shared/tools"
out_dir="${1:-${root}/dist/manuals}"

name="$(sed -n 's/^project(\([A-Za-z0-9_]*\).*/\1/p' "${src}/CMakeLists.txt" | head -1)"
version="$(sed -n 's/^project([A-Za-z0-9_]* VERSION \([0-9.]*\).*/\1/p' "${src}/CMakeLists.txt" | head -1)"

for tool in python3 wkhtmltopdf cmake; do
   command -v "$tool" >/dev/null 2>&1 || { echo "${tool} not found -- cannot build the manual" >&2; exit 1; }
done
python3 -c "import markdown" 2>/dev/null || {
   echo "python3 markdown module not found -- pip install markdown" >&2; exit 1; }

work="${src}/build/manual-work"
mkdir -p "$work"

# ------------------------------------------------------- the generated sections
cmake "-DPALETTE_DIR=${src}/palettes" "-DCCMAP_DIR=${src}/ccmaps" "-DOUTPUT=${work}/EmbeddedData.h" \
   -P "${src}/cmake/embed_data.cmake"
"${CXX:-g++}" -std=c++20 -O1 -Wall -Wextra -I"${src}/src" -I"${work}" \
   "${here}/docgen.cpp" "${src}/src/plugin/Params.cpp" "${src}/src/plugin/CcMap.cpp" \
   "${src}/src/engine/Resources.cpp" -o "${work}/docgen"

"${work}/docgen" --params-brief > "${work}/params-brief.md"
"${work}/docgen" --ccmaps "${src}"/ccmaps/*.ccmap > "${work}/ccmaps.md"

# ---------------------------------------------------------------- the document
logo="${root}/_designs/plugincore-logo-horizontal-4000.png"
[ -f "$logo" ] || logo=""

mkdir -p "$out_dir"
base="${name}-${version}-Manual"

# The CC map library takes the place a preset library has in the other
# manuals ({{PRESET_LIBRARY}}).
python3 "${shared_tools}/manual.py" \
   --plugin "$name" --display-name "$name" --version "$version" \
   --source "${src}/docs/manual.md" \
   --params "${work}/params-brief.md" --params-brief "${work}/params-brief.md" \
   --presets "${work}/ccmaps.md" \
   --css "${shared_tools}/manual.css" \
   ${logo:+--logo "$logo"} \
   --out "${out_dir}/${base}.html"

wkhtmltopdf \
   --quiet \
   --page-size A4 \
   --margin-top 16mm --margin-bottom 16mm --margin-left 16mm --margin-right 16mm \
   --encoding utf-8 \
   --title "${name} ${version} — Manual" \
   "${out_dir}/${base}.html" "${out_dir}/${base}.pdf"

pages="$(pdfinfo "${out_dir}/${base}.pdf" 2>/dev/null | sed -n 's/^Pages: *//p')"
printf '    %-44s %s%s\n' "${base}.pdf" "$(du -h "${out_dir}/${base}.pdf" | cut -f1)" \
   "${pages:+, ${pages} pages}"
