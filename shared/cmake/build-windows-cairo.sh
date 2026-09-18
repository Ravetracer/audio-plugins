#!/usr/bin/env bash
# Cross-builds the Cairo the Windows plugin window needs.
#
# Cairo has no MinGW package, but it has a win32 backend, so cross-building it
# means the window keeps one drawing implementation for both platforms instead
# of a second one written against a Windows drawing API. Everything else is
# switched off: no X11, no freetype or fontconfig (the win32 backend has its own
# font handling), no PNG, no zlib, no glib. That leaves pixman as the only
# dependency, which is small and has none of its own.
#
#   ./cmake/build-windows-cairo.sh /some/prefix
#   cmake -S . -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#         -DPLUGINCORE_WIN_CAIRO=/some/prefix
#
# The result is just include/ and lib/, so build it once and keep it. This
# machine has one at ~/projects/private_stuff/rainyday-winbuild; point
# PLUGINCORE_WIN_CAIRO there and there is no need to run this script at all.
#
# Needs meson; if the system python refuses to install it, a venv works:
#   python3 -m venv venv && venv/bin/pip install meson
set -euo pipefail

prefix="${1:?usage: $0 <install-prefix>}"
prefix="$(mkdir -p "$prefix" && cd "$prefix" && pwd)"
work="${prefix}/src"
mkdir -p "$work"

PIXMAN=pixman-0.46.4
CAIRO=cairo-1.18.4

cross="${work}/mingw-cross.ini"
cat > "$cross" <<INI
[binaries]
c = 'x86_64-w64-mingw32-gcc'
cpp = 'x86_64-w64-mingw32-g++'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
INI

cd "$work"
for pkg in "$PIXMAN" "$CAIRO"; do
   [ -f "${pkg}.tar.xz" ] || curl -sSLO "https://cairographics.org/releases/${pkg}.tar.xz"
   [ -d "$pkg" ] || tar xf "${pkg}.tar.xz"
done

export PKG_CONFIG_LIBDIR="${prefix}/lib/pkgconfig"

# -Db_ndebug=true is not optional. Meson does not define NDEBUG for
# --buildtype=release; b_ndebug defaults to false, so every assert() inside
# Cairo and pixman would otherwise survive into the static library and into the
# shipped plugin. On Linux the distribution's Cairo is built with NDEBUG and
# those checks are compiled out, so a failing internal assertion can only bite
# on Windows -- where it is an abort() behind a modal CRT dialog that takes the
# host down with it. Reported as issue #1: an assertion in cairo-hash.c froze
# Cubase 15 mid-session. -DCMAKE_BUILD_TYPE=Release in release.sh does not
# cover this, because Cairo is already a prebuilt static library by then.

meson setup "${work}/${PIXMAN}/_b" "${work}/${PIXMAN}" --cross-file="$cross" \
   --prefix="$prefix" --buildtype=release --default-library=static -Db_ndebug=true \
   -Dtests=disabled -Ddemos=disabled -Dgtk=disabled
ninja -C "${work}/${PIXMAN}/_b" install

meson setup "${work}/${CAIRO}/_b" "${work}/${CAIRO}" --cross-file="$cross" \
   --prefix="$prefix" --buildtype=release --default-library=static -Db_ndebug=true \
   -Dxlib=disabled -Dxcb=disabled -Dfreetype=disabled -Dfontconfig=disabled \
   -Dpng=disabled -Dzlib=disabled -Dglib=disabled -Dtests=disabled \
   -Dspectre=disabled -Dsymbol-lookup=disabled -Dgtk_doc=false
ninja -C "${work}/${CAIRO}/_b" install

echo
echo "Windows Cairo installed in ${prefix}"
