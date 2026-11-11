#!/usr/bin/env bash
# Builds the static Cairo a macOS plugin window needs, as a universal
# (arm64 + x86_64) library. Runs on a Mac -- or in the GitHub workflow, which
# is where the macOS builds are made (.github/workflows/resident-macos.yml).
#
# The counterpart of build-windows-cairo.sh: pixman and Cairo from upstream
# source, static, with only the backend the platform needs. Here that is
# Quartz, which Cairo uses for the toy font API (text through Core Text);
# drawing itself goes to image surfaces. No X11, freetype, fontconfig, PNG,
# zlib or glib, so pixman is the only dependency. Homebrew's Cairo is not an
# option for a plugin: its libraries are dylibs, for one architecture, built
# for the macOS version of the machine they were installed on.
#
#   shared/cmake/build-macos-cairo.sh macbuild/cairo-macos
#   cmake -S resident -B resident/build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#         (RESIDENT_MAC_CAIRO defaults to ../macbuild/cairo-macos)
#
# Each architecture is built on its own and the archives are joined with
# lipo: pixman picks its SIMD code by architecture at configure time, which a
# single "-arch arm64 -arch x86_64" build cannot do.
#
# Needs meson, ninja and pkg-config (brew install meson ninja pkgconf).
# ARCHS="arm64" builds one architecture only; MACOSX_DEPLOYMENT_TARGET sets the
# minimum macOS version (default 10.15, as the plugins use).
set -euo pipefail

prefix="${1:?usage: $0 <install-prefix>}"
prefix="$(mkdir -p "$prefix" && cd "$prefix" && pwd)"
work="${prefix}/src"
mkdir -p "$work"
archs="${ARCHS:-arm64 x86_64}"
minver="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
export MACOSX_DEPLOYMENT_TARGET="$minver"

PIXMAN=pixman-0.46.4
CAIRO=cairo-1.18.4

cd "$work"
for pkg in "$PIXMAN" "$CAIRO"; do
   [ -f "${pkg}.tar.xz" ] || curl -fsSLO "https://cairographics.org/releases/${pkg}.tar.xz"
   [ -d "$pkg" ] || tar xf "${pkg}.tar.xz"
done

for arch in $archs; do
   case "$arch" in
      arm64) family=aarch64; simd="-Da64-neon=disabled" ;;
      x86_64) family=x86_64; simd="-Dmmx=disabled" ;;
      *) echo "unknown architecture: $arch" >&2; exit 1 ;;
   esac
   aprefix="${prefix}/arch/${arch}"
   cross="${work}/macos-${arch}.ini"
   cat > "$cross" <<INI
[binaries]
c = ['clang', '-arch', '${arch}']
cpp = ['clang++', '-arch', '${arch}']
objc = ['clang', '-arch', '${arch}']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-mmacosx-version-min=${minver}']
c_link_args = ['-mmacosx-version-min=${minver}']
cpp_args = ['-mmacosx-version-min=${minver}']
cpp_link_args = ['-mmacosx-version-min=${minver}']
objc_args = ['-mmacosx-version-min=${minver}']
objc_link_args = ['-mmacosx-version-min=${minver}']

[host_machine]
system = 'darwin'
cpu_family = '${family}'
cpu = '${arch}'
endian = 'little'
INI
   export PKG_CONFIG_LIBDIR="${aprefix}/lib/pkgconfig"

   # -Db_ndebug=true: meson keeps assert() in a release build otherwise, and a
   # failing assertion inside Cairo is an abort() that takes the host down
   # (see build-windows-cairo.sh, and Verdalis issue #1).
   meson setup "${work}/${PIXMAN}/_b-${arch}" "${work}/${PIXMAN}" --cross-file="$cross" \
      --prefix="$aprefix" --libdir=lib --buildtype=release --default-library=static -Db_ndebug=true \
      -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled $simd
   ninja -C "${work}/${PIXMAN}/_b-${arch}" install

   meson setup "${work}/${CAIRO}/_b-${arch}" "${work}/${CAIRO}" --cross-file="$cross" \
      --prefix="$aprefix" --libdir=lib --buildtype=release --default-library=static -Db_ndebug=true \
      -Dquartz=enabled -Dxlib=disabled -Dxcb=disabled -Dfreetype=disabled -Dfontconfig=disabled \
      -Dpng=disabled -Dzlib=disabled -Dglib=disabled -Dtests=disabled \
      -Dspectre=disabled -Dsymbol-lookup=disabled -Dgtk_doc=false
   ninja -C "${work}/${CAIRO}/_b-${arch}" install
done

# One include/ (the headers do not differ by architecture) and universal archives.
mkdir -p "${prefix}/lib" "${prefix}/include"
first="$(echo $archs | awk '{print $1}')"
cp -R "${prefix}/arch/${first}/include/." "${prefix}/include/"
for lib in libcairo.a libpixman-1.a; do
   inputs=()
   for arch in $archs; do inputs+=("${prefix}/arch/${arch}/lib/${lib}"); done
   lipo -create "${inputs[@]}" -output "${prefix}/lib/${lib}"
   lipo -info "${prefix}/lib/${lib}"
done

echo
echo "macOS Cairo ($archs, macOS ${minver}+) installed in ${prefix}"
