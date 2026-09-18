#!/usr/bin/env bash
#
# Builds one plugin and installs it where CLAP hosts look, so Bitwig
# Studio (and any other CLAP host) picks it up.
#
#   shared/tools/install-plugin.sh <plugin-dir> [--vst3]
#
# Normally reached through a plugin's own ./install.sh, which passes its own
# directory. Everything else is derived: the display name and the tool names
# come from the plugin's CMakeLists.txt, so this script never needs to know
# which plugin it is building.
#
# The install prefix is $<PLUGIN>_PREFIX if set (e.g. RAINYDAY_PREFIX), else
# $PLUGINCORE_PREFIX, else ~/.clap.
#
# With --vst3 the plugin is also built as a VST3 through the clap-wrapper and
# installed into ~/.vst3 (or $<PLUGIN>_VST3_PREFIX). That needs CLAP/clap-wrapper
# and CLAP/vst3sdk checked out beside the plugins, and only works for a plugin
# whose CMakeLists.txt offers the <PLUGIN>_BUILD_VST3 option.
set -euo pipefail

plugin_dir="${1:?usage: $0 <plugin-dir> [--vst3]}"
plugin_dir="$(cd "$plugin_dir" && pwd)"
shift

with_vst3=0
for arg in "$@"; do
   case "$arg" in
      --vst3) with_vst3=1 ;;
      *) echo "unknown argument: $arg" >&2; exit 1 ;;
   esac
done

folder="$(basename "$plugin_dir")"
name="$(sed -n 's/^project(\([A-Za-z0-9_]*\).*/\1/p' "${plugin_dir}/CMakeLists.txt" | head -1)"
if [ -z "$name" ]; then
   echo "could not read the project name from ${plugin_dir}/CMakeLists.txt" >&2
   exit 1
fi

upper="$(echo "$folder" | tr '[:lower:]' '[:upper:]')"
# Indirect expansion, so RAINYDAY_PREFIX still works for RainyDay.
prefix_var="${upper}_PREFIX"
prefix="${!prefix_var:-${PLUGINCORE_PREFIX:-${HOME}/.clap}}"
vst3_prefix_var="${upper}_VST3_PREFIX"
vst3_prefix="${!vst3_prefix_var:-${PLUGINCORE_VST3_PREFIX:-${HOME}/.vst3}}"

if [ "$with_vst3" = 1 ] && ! grep -q "option(${upper}_BUILD_VST3" "${plugin_dir}/CMakeLists.txt"; then
   echo "${name} has no ${upper}_BUILD_VST3 option; it cannot build a VST3 yet" >&2
   exit 1
fi

build_dir="${plugin_dir}/build"

generator=()
if command -v ninja >/dev/null 2>&1; then
   generator=(-G Ninja)
fi

echo "==> configuring"
cmake -S "${plugin_dir}" -B "${build_dir}" "${generator[@]}" \
   -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_INSTALL_PREFIX="${prefix}" \
   -D${upper}_BUILD_VST3=$([ "$with_vst3" = 1 ] && echo ON || echo OFF) \
   -D${upper}_VST3_INSTALL_DIR="${vst3_prefix}" \
   ${CLAP_INCLUDE_DIR:+-DCLAP_INCLUDE_DIR="${CLAP_INCLUDE_DIR}"}

echo "==> building"
cmake --build "${build_dir}" --parallel

echo "==> verifying"
"${build_dir}/${folder}-render" --plugin "${build_dir}/${name}.clap" --selftest

echo "==> installing to ${prefix}/${name}"
cmake --install "${build_dir}"

vst3_line=""
if [ "$with_vst3" = 1 ]; then
   echo "==> installing the VST3 to ${vst3_prefix}"
   cmake --build "${build_dir}" --target "${name}-vst3-install"
   vst3_line="  ${vst3_prefix}/${name}.vst3"
fi

cat <<EOF

Installed:
  ${prefix}/${name}/${name}.clap
  ${prefix}/${name}/presets/   ($(ls -1 "${plugin_dir}/presets" | wc -l) presets)
${vst3_line}

Restart Bitwig Studio, or rescan plugins under
Settings -> Locations -> Plug-in Locations.
EOF
