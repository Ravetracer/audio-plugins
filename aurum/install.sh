#!/usr/bin/env bash
#
# Builds Aurum and installs it into the per-user plugin folders:
# ~/.clap/Aurum.clap and ~/.vst3/Aurum.vst3.
#
#   ./install.sh [--no-selftest]
#
# Aurum does not use ../shared/tools/install-plugin.sh: it is not built on the
# shared PluginCore library and has no offline renderer, so its self-test is
# the DSP test suite in tests/.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
cmake -S "${here}" -B "${build_dir}" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release

echo "==> building"
cmake --build "${build_dir}" --parallel

if [ "$selftest" = 1 ]; then
   echo "==> verifying"
   "${build_dir}/tests/geq_test"
   "${build_dir}/tests/engine_test" stability
fi

echo "==> installing"
mkdir -p "${HOME}/.clap" "${HOME}/.vst3"
cp -f "${build_dir}/Aurum.clap" "${HOME}/.clap/"
rm -rf "${HOME}/.vst3/Aurum.vst3"
cp -r "${build_dir}/vst3/Aurum.vst3" "${HOME}/.vst3/"

cat <<TXT

Installed:
  ${HOME}/.clap/Aurum.clap
  ${HOME}/.vst3/Aurum.vst3

Rescan plugins in the host.
TXT
