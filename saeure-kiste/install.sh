#!/usr/bin/env bash
#
# Builds SaeureKiste and installs it where CLAP hosts look, so Bitwig Studio
# (and any other CLAP host) picks it up.
#
#   ./install.sh [--no-selftest]
#
# This does not go through ../shared/tools/install-plugin.sh, which the other
# plugins use. That script derives the tool names and the environment variable
# it reads from the plugin's folder name, and "saeure-kiste" is not a usable
# shell identifier, so it would need changing for this one plugin's sake.
#
# Override the destination with SAEUREKISTE_PREFIX=/some/where ./install.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
prefix="${SAEUREKISTE_PREFIX:-${PLUGINCORE_PREFIX:-${HOME}/.clap}}"
build_dir="${here}/build"

selftest=1
for arg in "$@"; do
   case "$arg" in
      --no-selftest) selftest=0 ;;
      *) echo "unknown argument: $arg" >&2; exit 1 ;;
   esac
done

generator=()
if command -v ninja >/dev/null 2>&1; then
   generator=(-G Ninja)
fi

echo "==> configuring"
cmake -S "${here}" -B "${build_dir}" "${generator[@]}" \
   -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_INSTALL_PREFIX="${prefix}" \
   ${CLAP_INCLUDE_DIR:+-DCLAP_INCLUDE_DIR="${CLAP_INCLUDE_DIR}"}

echo "==> building"
cmake --build "${build_dir}" --parallel

if [ "$selftest" = 1 ]; then
   echo "==> verifying"
   "${build_dir}/saeurekiste-render" --plugin "${build_dir}/SaeureKiste.clap" --selftest
fi

echo "==> installing to ${prefix}/SaeureKiste"
cmake --install "${build_dir}"

cat <<EOF

Installed:
  ${prefix}/SaeureKiste/SaeureKiste.clap
  ${prefix}/SaeureKiste/presets/   ($(ls -1 "${here}/presets" | grep -c '\.saeurekiste$' || true) presets)

Restart Bitwig Studio, or rescan plugins under
Settings -> Locations -> Plug-in Locations.
EOF
