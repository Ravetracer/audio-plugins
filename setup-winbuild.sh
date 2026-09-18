#!/usr/bin/env bash
#
# One-time setup for cross-building the Windows half of the suite.
#
#   ./setup-winbuild.sh
#
# Produces winbuild/cairo-mingw, which release.sh finds by itself. Safe to
# re-run: it skips whatever is already there.
#
# Why this exists at all: the plugin window has one drawing implementation,
# Cairo, for both platforms -- on Linux against X11, on Windows against Cairo's
# win32 backend, which paints through GDI. There is no separate Windows
# renderer, so without this Cairo the Windows plugins build with no window.
#
# Cairo has no MinGW package, so it is cross-built from source here. meson is
# needed to do that and goes into a local venv rather than the system Python.
#
# Requires: mingw-w64, ninja, python3, curl.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
winbuild="${here}/winbuild"
prefix="${winbuild}/cairo-mingw"
venv="${winbuild}/venv"

missing=()
for tool in x86_64-w64-mingw32-g++ ninja python3 curl; do
   command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
   echo "missing: ${missing[*]}" >&2
   echo "on Debian/Ubuntu: sudo apt install mingw-w64 ninja-build python3-venv curl" >&2
   exit 1
fi

# A Cairo built before issue #1 has its assert()s compiled in, and a failing
# one aborts the host behind a modal CRT dialog. Those builds must not survive
# a re-run of this script, so the "already built" check tests for NDEBUG as
# well as for the library: the assertion text is only in the archive when the
# asserts are.
if [ -f "${prefix}/lib/libcairo.a" ]; then
   if strings "${prefix}/lib/libcairo.a" | grep -q "tmp.table_size - hash_table_sizes"; then
      echo "==> Windows Cairo in ${prefix} was built with assertions enabled"
      echo "    rebuilding it; see shared/cmake/build-windows-cairo.sh"
      rm -rf "${prefix}/lib" "${prefix}/include" \
             "${prefix}"/src/*/_b
   else
      echo "==> Windows Cairo already built: ${prefix}"
      echo "    delete winbuild/ to force a rebuild"
      exit 0
   fi
fi

mkdir -p "$winbuild"

# meson, in a venv: system Python installs are externally managed on current
# Debian/Ubuntu and refuse a plain pip install.
if [ ! -x "${venv}/bin/meson" ]; then
   echo "==> installing meson into ${venv}"
   python3 -m venv "$venv"
   "${venv}/bin/pip" install --quiet --upgrade pip
   "${venv}/bin/pip" install --quiet meson
fi
echo "    meson $("${venv}/bin/meson" --version)"

# The cross-build itself is shared by the suite.
builder="${here}/shared/cmake/build-windows-cairo.sh"
if [ ! -f "$builder" ]; then
   echo "missing ${builder}" >&2
   exit 1
fi

echo "==> cross-building pixman + Cairo (a few minutes)"
PATH="${venv}/bin:$PATH" "$builder" "$prefix"

echo
echo "==> done: ${prefix}"
echo "    ./release.sh <version>   now builds Windows with a plugin window"
