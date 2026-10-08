#!/usr/bin/env bash
#
# Builds Substrike and installs it into the per-user plugin folders:
# ~/.clap/Substrike.clap and ~/.vst3/Substrike.vst3.
#
#   ./install.sh [--no-selftest]
#
# Substrike does not use ../shared/tools/install-plugin.sh: like Aurum it is
# not built on the shared PluginCore library. Its self-test is the offline
# renderer's, which loads the freshly built .clap the way a host does.
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
   "${build_dir}/substrike-preset-check"
   "${build_dir}/substrike-render" --plugin "${build_dir}/Substrike.clap" --selftest
fi

echo "==> installing"
mkdir -p "${HOME}/.clap" "${HOME}/.vst3"
cp -f "${build_dir}/Substrike.clap" "${HOME}/.clap/"
# Copied over the installed bundle in place rather than deleting it first.
cp -rT "${build_dir}/vst3/Substrike.vst3" "${HOME}/.vst3/Substrike.vst3"

cat <<TXT

Installed:
  ${HOME}/.clap/Substrike.clap
  ${HOME}/.vst3/Substrike.vst3

Rescan plugins in the host.
TXT
