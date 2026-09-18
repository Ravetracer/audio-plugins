#!/usr/bin/env bash
#
# Builds one plugin's manual as PDF, and the HTML it was rendered from.
#
#   shared/tools/make-manual.sh rainyday [outdir]
#
# The manual is written in <plugin>/docs/manual.md. Two of its sections are not
# written by hand but generated from the plugin itself: {{PARAMETER_REFERENCE}}
# comes out of the ParamDesc table and {{PRESET_LIBRARY}} out of the preset
# files, so neither can drift from the build being documented.
#
# The generator is compiled here with the plain compiler rather than through
# the plugin's CMake project, because release builds switch the offline tools
# off and the manual still has to build. It needs nothing but the plugin's
# params.cpp and the shared parameter code -- no CLAP headers, no X11, no Cairo.
#
# Needs: a C++17 compiler, python3 with the markdown module, and wkhtmltopdf.
# release.sh calls this for every plugin and skips the manuals if any of them
# is missing rather than failing a release over documentation.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"      # shared/tools
root="$(cd "${here}/../.." && pwd)"                       # the suite root

plugin="${1:-}"
if [ -z "$plugin" ]; then
   echo "usage: $0 <plugin-folder> [outdir]" >&2
   exit 1
fi
plugin="$(basename "$plugin")"
src="${root}/${plugin}"
out_dir="${2:-${root}/dist/manuals}"

[ -f "${src}/CMakeLists.txt" ] || { echo "no such plugin: ${plugin}" >&2; exit 1; }
[ -f "${src}/docs/manual.md" ] || { echo "no manual source: ${plugin}/docs/manual.md" >&2; exit 1; }

name="$(sed -n 's/^project(\([A-Za-z0-9_]*\).*/\1/p' "${src}/CMakeLists.txt" | head -1)"
version="$(sed -n 's/^project([A-Za-z0-9_]* VERSION \([0-9.]*\).*/\1/p' "${src}/CMakeLists.txt" | head -1)"

for tool in python3 wkhtmltopdf; do
   command -v "$tool" >/dev/null 2>&1 || { echo "${tool} not found -- cannot build the manual" >&2; exit 1; }
done
python3 -c "import markdown" 2>/dev/null || {
   echo "python3 markdown module not found -- pip install markdown" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ------------------------------------------------------- the generated sections
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra \
   "-DPLUGINCORE_DOC_NS=${plugin}" \
   -I"${root}/shared/include" -I"${src}/src" \
   "${here}/docgen.cpp" "${src}/src/params.cpp" "${root}/shared/src/params.cpp" \
   -o "${work}/docgen"

"${work}/docgen" --params > "${work}/params.md"
"${work}/docgen" --presets "${src}/presets" > "${work}/presets.md"

# ---------------------------------------------------------------- the document
# There is no collection-wide logo yet. When one is added at this path it is
# picked up automatically; until then the cover carries the plugin wordmark.
logo="${root}/_designs/plugincore-logo-horizontal-4000.png"
[ -f "$logo" ] || logo=""

mkdir -p "$out_dir"
base="${name}-${version}-Manual"

python3 "${here}/manual.py" \
   --plugin "$name" --version "$version" \
   --source "${src}/docs/manual.md" \
   --params "${work}/params.md" --presets "${work}/presets.md" \
   --css "${here}/manual.css" \
   ${logo:+--logo "$logo"} \
   --out "${out_dir}/${base}.html"

# Page geometry lives here rather than in the CSS, which wkhtmltopdf ignores
# @page in. Nothing here needs the patched-qt build: that one could add page
# footers and a PDF outline, and where it is missing the manual simply has no
# page numbers -- which is why the contents page is a list of links rather than
# a list of page numbers.
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
