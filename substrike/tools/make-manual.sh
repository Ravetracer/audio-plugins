#!/usr/bin/env bash
#
# Builds Substrike's manual as PDF, and the HTML it was rendered from.
#
#   substrike/tools/make-manual.sh [outdir]
#
# Like Aurum's: the generated sections come from tools/docgen.cpp, compiled
# against Substrike's own parameter table and factory presets, and the
# rendering is the shared manual.py and manual.css, used unchanged, so the
# manuals look alike. release.sh calls this script for Substrike.
#
# It works in build/manual-work/, which it overwrites; it deletes nothing.
#
# Needs: a C++20 compiler, cmake, python3 with the markdown module, and
# wkhtmltopdf.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"      # substrike/tools
src="$(cd "${here}/.." && pwd)"                            # substrike
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
cmake -DPRESET_DIR="${src}/presets" -DOUTPUT="${work}/FactoryPresetData.h" -P "${src}/cmake/embed_presets.cmake"
"${CXX:-g++}" -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
   -I"${src}/src" -I"${work}" \
   "${here}/docgen.cpp" "${src}/src/plugin/Params.cpp" "${src}/src/state/StateIO.cpp" \
   "${src}/src/state/Presets.cpp" "${src}/src/state/Settings.cpp" \
   -o "${work}/docgen"

"${work}/docgen" --params-brief > "${work}/params-brief.md"
"${work}/docgen" --presets > "${work}/presets.md"

# ---------------------------------------------------------------- the document
logo="${root}/_designs/plugincore-logo-horizontal-4000.png"
[ -f "$logo" ] || logo=""

mkdir -p "$out_dir"
base="${name}-${version}-Manual"

# The parameters carry no help text, so the full reference and the brief one
# are the same table; the manual uses {{PARAMETER_SUMMARY}}.
python3 "${shared_tools}/manual.py" \
   --plugin "$name" --display-name "$name" --version "$version" \
   --source "${src}/docs/manual.md" \
   --params "${work}/params-brief.md" --params-brief "${work}/params-brief.md" \
   --presets "${work}/presets.md" \
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
