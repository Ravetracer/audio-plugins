#!/usr/bin/env bash
#
# Builds every plugin in the suite and packs one "audio-plugins" archive
# containing the Linux and Windows binaries plus everything needed to install
# them.
#
# One archive per plugin and one for the suite, each holding both platforms.
#
#   ./release.sh 0.1.0                  Linux + Windows (needs cross-built Cairo)
#   ./release.sh 0.1.0 --linux-only     skip the Windows half
#   ./release.sh 0.1.0 --tarball        also emit .tar.gz beside the .zip files
#   ./release.sh 0.1.0 --no-manuals     do not build or ship the PDF manuals
#   ./release.sh 0.1.0 --no-vst3        CLAP only; do not build or ship the VST3s
#
# Every plugin ships in both formats: the .clap folder and the .vst3 bundle sit
# side by side in each platform folder. The VST3 is the same plugin behind the
# clap-wrapper, and it needs CLAP/clap-wrapper and CLAP/vst3sdk checked out. If
# either is missing the release goes out CLAP-only rather than failing.
#
# Windows builds need a Cairo cross-built with mingw-w64. Build it once with
#
#   ./setup-winbuild.sh
#
# which puts it in winbuild/cairo-mingw, where this script finds it by itself.
# PLUGINCORE_WIN_CAIRO overrides that location.
#
# Without it the Windows plugins would build with no plugin window at all, so
# this script refuses to produce that silently -- pass --windows-no-gui if a
# window-less Windows build is genuinely what you want.
set -euo pipefail

# Each build step sends its normal output to /dev/null so the log stays short.
# That makes a failure look like the script simply stopped, so say what died.
step=""
trap 'rc=$?; [ $rc -eq 0 ] || echo "!!  failed during: ${step:-startup} (exit $rc)" >&2' EXIT

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version="${1:-}"
shift || true

if [ -z "$version" ]; then
   echo "usage: $0 <version> [--linux-only] [--windows-no-gui] [--tarball] [--no-manuals] [--no-vst3]" >&2
   exit 1
fi

linux_only=0
windows_no_gui=0
tarball=0
no_manuals=0
no_vst3=0
for arg in "$@"; do
   case "$arg" in
      --linux-only)     linux_only=1 ;;
      --windows-no-gui) windows_no_gui=1 ;;
      --tarball)        tarball=1 ;;
      --no-manuals)     no_manuals=1 ;;
      --no-vst3)        no_vst3=1 ;;
      *) echo "unknown option: $arg" >&2; exit 1 ;;
   esac
done

# The Windows Cairo. Built once into winbuild/cairo-mingw by setup-winbuild.sh
# and found there automatically; PLUGINCORE_WIN_CAIRO overrides for a copy kept
# somewhere else.
win_cairo="${PLUGINCORE_WIN_CAIRO:-${here}/winbuild/cairo-mingw}"
[ -n "$win_cairo" ] && win_cairo="$(cd "$win_cairo" 2>/dev/null && pwd || echo "$win_cairo")"

stage="${here}/dist/audio-plugins-${version}"
out_dir="${here}/dist"

# ---------------------------------------------------------------- plugin list
#
# Every subdirectory with a CMakeLists.txt is a plugin, minus the few that are
# not. Keeping this discovered rather than hardcoded means a new plugin joins a
# release by existing; "shared" carries a CMakeLists.txt of its own and is a
# library, not a plugin.
in_git_tree=0
git -C "$here" rev-parse --is-inside-work-tree >/dev/null 2>&1 && in_git_tree=1

plugins=()
for d in "${here}"/*/; do
   d="${d%/}"
   name="$(basename "$d")"
   case "$name" in
      CLAP|dist|shared|winbuild|_designs) continue ;;
   esac
   [ -f "${d}/CMakeLists.txt" ] || continue
   # What actually makes it a plugin: a CLAP entry point of its own.
   [ -f "${d}/src/plugin.cpp" ] || continue
   # A release ships what is in the repository. A plugin folder git does not
   # track is work in progress living beside the suite -- another branch
   # checked out here, or something under .git/info/exclude -- and discovering
   # by directory alone would put it in an archive by accident. Skipped only
   # when this really is a git checkout, so an exported tree still releases.
   if [ "$in_git_tree" = 1 ] && [ -z "$(git -C "$here" ls-files -- "$name" | head -1)" ]; then
      echo "    skipping ${name}: not tracked by git"
      continue
   fi
   plugins+=("$name")
done

if [ ${#plugins[@]} -eq 0 ]; then
   echo "no plugins found in ${here}" >&2
   exit 1
fi

# CMake project name, e.g. rainyday -> RainyDay. Read from the CMakeLists rather
# than guessed, because the folder is lowercase and the artifact is not.
project_name() {
   sed -n 's/^project(\([A-Za-z0-9_]*\).*/\1/p' "${here}/$1/CMakeLists.txt" | head -1
}
project_version() {
   sed -n 's/^project([A-Za-z0-9_]* VERSION \([0-9.]*\).*/\1/p' "${here}/$1/CMakeLists.txt" | head -1
}

generator=()
command -v ninja >/dev/null 2>&1 && generator=(-G Ninja)

echo "==> Audio Plugins collection ${version}"
echo "    plugins: ${plugins[*]}"

# ------------------------------------------------------------ windows preflight
build_windows=1
if [ "$linux_only" = 1 ]; then
   build_windows=0
   echo "    windows: skipped (--linux-only)"
elif ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
   echo "!!  x86_64-w64-mingw32-g++ not found -- skipping the Windows build." >&2
   echo "    Install mingw-w64, or pass --linux-only to silence this." >&2
   build_windows=0
elif [ -z "$win_cairo" ] || [ ! -f "${win_cairo}/lib/libcairo.a" ]; then
   if [ "$windows_no_gui" = 1 ]; then
      echo "!!  No Windows Cairo -- Windows plugins will have NO plugin window." >&2
      win_cairo=""
   else
      echo "!!  No Windows Cairo at ${win_cairo}" >&2
      echo "    Windows plugins would be built without a plugin window, which is" >&2
      echo "    not something to ship by accident. Build it once with:" >&2
      echo "      ./setup-winbuild.sh" >&2
      echo "    or re-run with --linux-only, or --windows-no-gui to accept it." >&2
      exit 1
   fi
fi

# ------------------------------------------------------------- vst3 preflight
#
# The VST3s come from the clap-wrapper, which needs its own checkout and the
# VST3 SDK beside it in CLAP/. Both are gitignored third-party clones, so a
# fresh working copy will not have them -- say so and ship CLAP-only rather
# than failing a release over a second format.
build_vst3=1
wrapper_dir="${here}/CLAP/clap-wrapper"
vst3_sdk_dir="${PLUGINCORE_VST3_SDK:-${here}/CLAP/vst3sdk}"
if [ "$no_vst3" = 1 ]; then
   build_vst3=0
   echo "    vst3:    skipped (--no-vst3)"
elif [ ! -f "${wrapper_dir}/cmake/wrap_vst3.cmake" ]; then
   echo "!!  No clap-wrapper at ${wrapper_dir} -- shipping CLAP only." >&2
   echo "    git clone https://github.com/free-audio/clap-wrapper.git CLAP/clap-wrapper" >&2
   build_vst3=0
elif [ ! -f "${vst3_sdk_dir}/public.sdk/source/main/pluginfactory.cpp" ]; then
   echo "!!  No VST3 SDK at ${vst3_sdk_dir} -- shipping CLAP only." >&2
   echo "    See 'CLAP SDK' in CLAUDE.md for the checkout, and note it must be" >&2
   echo "    VST 3.8 or newer: earlier SDKs are not MIT licensed." >&2
   build_vst3=0
fi

# Record exactly what produced the VST3s. The wrapper needs a local patch to
# build against VST3 3.8 (shared/patches/), so "which checkout" is not a
# rhetorical question when a build has to be reproduced.
CLAP_WRAPPER_VER=""
VST3_SDK_VER=""
if [ "$build_vst3" = 1 ]; then
   CLAP_WRAPPER_VER="$(sed -n 's/^[[:space:]]*VERSION \([0-9.]*\).*/\1/p' \
      "${wrapper_dir}/CMakeLists.txt" | head -1)"
   VST3_SDK_VER="$(sed -n 's/^[[:space:]]*VERSION \([0-9.]*\).*/\1/p' \
      "${vst3_sdk_dir}/CMakeLists.txt" | head -1)"
   echo "    vst3:    clap-wrapper ${CLAP_WRAPPER_VER:-?}, VST3 SDK ${VST3_SDK_VER:-?}"
fi

# Which operating systems this release actually contains. Every archive holds
# both, so this is what the install notes and the packing loops read.
targets=(linux)
[ "$build_windows" = 1 ] && targets+=(windows)

has_target() {
   local t
   for t in "${targets[@]}"; do [ "$t" = "$1" ] && return 0; done
   return 1
}

rm -rf "$stage"
mkdir -p "$stage"

# ------------------------------------------------------------------ the builds
build_one() {
   # `plugin` is a loop variable in the callers too, and a function that
   # walks the plugin list without declaring it local leaves the caller
   # iterating the last entry instead of its own.
   local plugin
   local plugin="$1" target="$2" build_dir="$3" install_root="$4"
   local name upper
   name="$(project_name "$plugin")"
   # The option prefix is the CMake project name uppercased, not the folder
   # name: a folder may carry hyphens ("three-o-three") and a CMake variable may
   # not. Getting this wrong is quiet -- CMake only warns that a
   # manually-specified variable went unused, and the build silently keeps its
   # defaults, which for a release means shipping with the offline tools on.
   upper="$(echo "$name" | tr '[:lower:]' '[:upper:]')"

   local args=(
      -S "${here}/${plugin}"
      -B "$build_dir"
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX="$install_root"
   )
   # The offline tools are development aids and are not part of a release.
   args+=("-D${upper}_BUILD_TOOLS=OFF")
   args+=("-D${upper}_BUILD_VST3=$([ "$build_vst3" = 1 ] && echo ON || echo OFF)")

   if [ "$target" = windows ]; then
      args+=(-DCMAKE_TOOLCHAIN_FILE="${here}/shared/cmake/mingw-w64-x86_64.cmake")
      [ -n "$win_cairo" ] && args+=("-D${upper}_WIN_CAIRO=${win_cairo}")
   fi

   echo "--> ${name} (${target})"
   step="${name} (${target}) configure"
   cmake "${generator[@]}" "${args[@]}" >/dev/null
   step="${name} (${target}) build"
   cmake --build "$build_dir" --parallel "$(nproc)" >/dev/null
   if [ "$build_vst3" = 1 ]; then
      step="${name} (${target}) build vst3"
      cmake --build "$build_dir" --parallel "$(nproc)" --target "${name}-vst3" >/dev/null
   fi
   step="${name} (${target}) install"
   cmake --install "$build_dir" >/dev/null

   # The VST3 is a bundle, not something CMake installs through the prefix, so
   # it is copied into the same platform folder beside the .clap folder.
   if [ "$build_vst3" = 1 ]; then
      step="${name} (${target}) stage vst3"
      local bundle="${build_dir}/vst3/${name}.vst3"
      if [ ! -d "$bundle" ]; then
         echo "!!  ${name} (${target}): no VST3 bundle at ${bundle}" >&2
         exit 1
      fi
      rm -rf "${install_root}/${name}.vst3"
      cp -r "$bundle" "${install_root}/"
   fi
}

work="${here}/dist/.build"
rm -rf "$work"

for plugin in "${plugins[@]}"; do
   build_one "$plugin" linux "${work}/${plugin}-linux" "${stage}/linux"
   if [ "$build_windows" = 1 ]; then
      build_one "$plugin" windows "${work}/${plugin}-windows" "${stage}/windows"
   fi
done

# ----------------------------------------------------------------- manuals
#
# One PDF per plugin, generated from <plugin>/docs/manual.md with the parameter
# reference and the preset library read out of the plugin itself. They go into
# that plugin's own archives and into a manuals/ folder in the suite archives.
#
# Documentation is not worth failing a release over: if the toolchain for it is
# not installed, this says so and the archives simply go out without manuals.
manual_dir="${here}/dist/manuals"
rm -rf "$manual_dir"

manual_for() {
   local plugin
   echo "${manual_dir}/$(project_name "$1")-$(project_version "$1")-Manual.pdf"
}

if [ "$no_manuals" = 1 ]; then
   echo "    manuals: skipped (--no-manuals)"
else
   echo "--> manuals"
   for plugin in "${plugins[@]}"; do
      step="$(project_name "$plugin") manual"
      if [ ! -f "${here}/${plugin}/docs/manual.md" ]; then
         echo "!!  ${plugin} has no docs/manual.md -- shipping it without a manual" >&2
         continue
      fi
      if ! "${here}/shared/tools/make-manual.sh" "$plugin" "$manual_dir"; then
         echo "!!  the ${plugin} manual could not be built -- shipping it without one" >&2
      fi
   done
fi

# ------------------------------------------------------------- install notes
#
# Every archive carries both operating systems, so the note describes both --
# but only the ones the archive actually contains, because a --linux-only build
# must not point at a windows/ folder that is not in it.
install_note() {
   local what="$1" what_linux="$2" what_windows="$3"
   local what_linux_vst3="${4:-}" what_windows_vst3="${5:-}"
   echo "$what"
   printf '=%.0s' $(seq ${#what})
   echo
   if [ "$build_vst3" = 1 ]; then
      cat <<'TXT'

Each plugin is here in two formats: a CLAP and a VST3. They are the same
plugin -- same engine, same presets, same window -- so install whichever your
host prefers, or both. Each format loads from its own fixed location. Copy the
folders out of this archive into it and keep each one whole.
TXT
   else
      cat <<'TXT'

CLAP hosts load plugins from a fixed location. Copy the plugin folders out of
this archive into it, keeping each plugin's folder and its presets together: a
plugin finds its factory presets by looking for a presets directory next to its
own binary.
TXT
   fi
   if has_target linux; then
      cat <<TXT

Linux
-----

CLAP -- copy ${what_linux} into:

    ~/.clap/

so that you end up with ~/.clap/RainyDay/RainyDay.clap and so on. Keep the
presets folder with it: a CLAP looks for its factory presets in a presets
directory beside its own binary.
TXT
      if [ "$build_vst3" = 1 ]; then
         cat <<TXT

VST3 -- copy ${what_linux_vst3} into:

    ~/.vst3/

so that you end up with ~/.vst3/RainyDay.vst3/ and so on. Keep the .vst3 folder
whole -- it is a bundle, not a file. Its factory presets are built into the
plugin and appear in its own browser either way; the copies inside the bundle
are there to read and to edit.
TXT
      fi
      cat <<'TXT'

Both need X11 and Cairo, which any Linux machine that can run a DAW already has.
TXT
   fi
   if has_target windows; then
      cat <<TXT

Windows
-------

CLAP -- copy ${what_windows} into:

    C:\\Program Files\\Common Files\\CLAP\\

so that you end up with ...\\CLAP\\RainyDay\\RainyDay.clap and so on. Keep the
presets folder with it: a CLAP looks for its factory presets in a presets
directory beside its own binary.
TXT
      if [ "$build_vst3" = 1 ]; then
         cat <<TXT

VST3 -- copy ${what_windows_vst3} into:

    C:\\Program Files\\Common Files\\VST3\\

so that you end up with ...\\VST3\\RainyDay.vst3\\ and so on. Keep the .vst3
folder whole -- it is a bundle, not a file. Its factory presets are built into
the plugin and appear in its own browser either way; the copies inside the
bundle are there to read and to edit.
TXT
      fi
      cat <<'TXT'

Nothing else is needed: the plugin window and its Cairo are linked in, so there
are no DLLs to install beside it.
TXT
   fi
   cat <<'TXT'

Then rescan plugins in your host. Tested with Bitwig Studio and Reaper.
TXT
}

build_info() {
   local plugin
   echo "Audio Plugins ${version}"
   echo "built $(date -u '+%Y-%m-%d %H:%M UTC') on $(uname -srm)"
   echo
   echo "plugins:"
   for plugin in "${plugins[@]}"; do
      echo "  $(project_name "$plugin") $(project_version "$plugin")   (${plugin}/)"
   done
   echo
   echo "formats:"
   if [ "$build_vst3" = 1 ]; then
      echo "  CLAP      native"
      echo "  VST3      via clap-wrapper ${CLAP_WRAPPER_VER:-}, VST3 SDK ${VST3_SDK_VER:-} (MIT)"
   else
      echo "  CLAP      native"
      echo "  VST3      not built"
   fi
   echo
   echo "targets:"
   echo "  linux/    x86_64, GUI via X11 + Cairo"
   if [ "$build_windows" = 1 ]; then
      if [ -n "$win_cairo" ]; then
         echo "  windows/  x86_64 (mingw-w64), GUI via win32 + Cairo"
      else
         echo "  windows/  x86_64 (mingw-w64), NO PLUGIN WINDOW"
      fi
   else
      echo "  windows/  not built"
   fi
}

# --------------------------------------------------------------- archive extras
#
# A plugin may carry licence terms of its own on top of the suite's -- an
# embedded third-party asset with an attribution requirement, for instance --
# and those terms have to travel with anything that redistributes it. Where a
# plugin's LICENSE differs from the suite's, its own copy is what goes into its
# archives, and the suite archives carry it alongside as LICENSE-<Name>.
plugin_license() {
   if [ -f "${here}/$1/LICENSE" ] && ! cmp -s "${here}/$1/LICENSE" "${here}/LICENSE"; then
      printf '%s\n' "${here}/$1/LICENSE"
   fi
}

copy_extra_licenses() {
   local d="$1" p extra
   for p in "${plugins[@]}"; do
      extra="$(plugin_license "$p")"
      if [ -n "$extra" ]; then
         cp "$extra" "${d}/LICENSE-$(project_name "$p")"
      fi
   done
}

cp "${here}/README.md" "${here}/LICENSE" "$stage/"
copy_extra_licenses "$stage"

install_note "Audio Plugins ${version}" \
   "every <Name>/ folder in linux/" "every <Name>\\ folder in windows\\" \
   "every <Name>.vst3 folder in linux/" "every <Name>.vst3 folder in windows\\" \
   > "${stage}/INSTALL.txt"

for plugin in "${plugins[@]}"; do
   manual="$(manual_for "$plugin")"
   [ -f "$manual" ] || continue
   mkdir -p "${stage}/manuals"
   cp "$manual" "${stage}/manuals/"
done

build_info > "${stage}/BUILD-INFO.txt"

# ----------------------------------------------------------------------- pack
#
# One archive per plugin, plus one for the whole suite. Each carries the Linux
# and the Windows build together: one download per plugin is what a site wants
# to offer, and splitting it by operating system only made two links where one
# would do.
#
# Everything is a .zip, including the Linux builds. That is not the Unix habit,
# but a release nobody can publish is worse than one in the wrong format, and
# download managers commonly handle zip alone. Nothing is lost by it: zip records
# Unix permissions and a .clap is dlopen'd, which needs no execute bit. Pass
# --tarball to get .tar.gz alongside for anywhere that prefers it.
step="packing"
cd "$out_dir"

made=()

pack() {
   local dir="$1"
   if command -v zip >/dev/null 2>&1; then
      rm -f "${dir}.zip"; zip -qr "${dir}.zip" "$dir"; made+=("${dir}.zip")
   else
      echo "!!  zip not found: install it, or the release cannot be published" >&2
   fi
   if [ "$tarball" = 1 ]; then
      rm -f "${dir}.tar.gz"; tar czf "${dir}.tar.gz" "$dir"; made+=("${dir}.tar.gz")
   fi
   rm -rf "$dir"
}

# --- one per plugin, both operating systems in the one archive
for plugin in "${plugins[@]}"; do
   name="$(project_name "$plugin")"
   pver="$(project_version "$plugin")"
   d="${name}-${pver}"
   rm -rf "$d"; mkdir -p "$d"
   have=0
   for os in "${targets[@]}"; do
      # Both formats: the <Name>/ folder holding the .clap and its presets, and
      # the <Name>.vst3 bundle beside it. Copying only the first is a silent
      # failure -- the archive still builds, it is just missing a format -- so
      # the VST3 is checked for below rather than left to the eye.
      if [ -d "${stage}/${os}/${name}" ]; then
         mkdir -p "${d}/${os}"
         cp -r "${stage}/${os}/${name}" "${d}/${os}/"
         have=1
      fi
      if [ -d "${stage}/${os}/${name}.vst3" ]; then
         mkdir -p "${d}/${os}"
         cp -r "${stage}/${os}/${name}.vst3" "${d}/${os}/"
         have=1
      elif [ "$build_vst3" = 1 ]; then
         echo "!!  ${name}: no ${os} VST3 in the staging tree" >&2
         exit 1
      fi
   done
   if [ "$have" = 0 ]; then
      rm -rf "$d"
      continue
   fi
   own="$(plugin_license "$plugin")"
   cp "${own:-${here}/LICENSE}" "${d}/LICENSE"
   [ -f "${here}/${plugin}/README.md" ] && cp "${here}/${plugin}/README.md" "${d}/"
   manual="$(manual_for "$plugin")"
   [ -f "$manual" ] && cp "$manual" "${d}/"
   install_note "${name} ${pver}" \
      "linux/${name}" "windows\\${name}" \
      "linux/${name}.vst3" "windows\\${name}.vst3" > "${d}/INSTALL.txt"
   build_info > "${d}/BUILD-INFO.txt"
   pack "$d"
done

# --- the whole suite, every plugin for both platforms. Kept out of pack()
# because the staging tree it names is also where BUILD-INFO.txt is read from
# below.
base="audio-plugins-${version}"
if command -v zip >/dev/null 2>&1; then
   rm -f "${base}.zip"; zip -qr "${base}.zip" "$base"; made+=("${base}.zip")
fi
if [ "$tarball" = 1 ]; then
   rm -f "${base}.tar.gz"; tar czf "${base}.tar.gz" "$base"; made+=("${base}.tar.gz")
fi
rm -rf "$work"

echo
echo "==> ${out_dir}"
for f in "${made[@]}"; do
   printf "    %-46s %s\n" "$f" "$(du -h "$f" | cut -f1)"
done
echo
cat "${stage}/BUILD-INFO.txt"
