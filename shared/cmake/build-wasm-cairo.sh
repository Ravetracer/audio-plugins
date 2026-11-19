#!/usr/bin/env bash
# Cross-builds Cairo for WebAssembly (wasm32-wasip1) with wasi-sdk, for plugin
# editors that run in a browser page as their own wasm module (Aurum's WCLAP
# editor). Image surfaces only: no X11, no win32, no fontconfig, no PNG. Text
# goes through FreeType, built here as well, from fonts the editor embeds --
# the same rasteriser the Linux window uses, so the text looks the same.
#
#   shared/cmake/build-wasm-cairo.sh <wasi-sdk> <install-prefix>
#
# Gives <prefix>/include and <prefix>/lib (libcairo.a, libpixman-1.a,
# libfreetype.a). Build it once and keep it; the repository's default prefix
# is wasmbuild/cairo-wasi (gitignored).
#
# Needs meson (winbuild/venv/bin/meson from setup-winbuild.sh works).
set -euo pipefail

sdk="${1:?usage: $0 <wasi-sdk> <install-prefix>}"
sdk="$(cd "$sdk" && pwd)"
prefix="${2:?usage: $0 <wasi-sdk> <install-prefix>}"
prefix="$(mkdir -p "$prefix" && cd "$prefix" && pwd)"
work="${prefix}/src"
mkdir -p "$work"

MESON="${MESON:-meson}"
PIXMAN=pixman-0.46.4
CAIRO=cairo-1.18.4
FREETYPE=freetype-2.13.3

cross="${work}/wasi-cross.ini"
cat > "$cross" <<INI
[binaries]
c = ['${sdk}/bin/clang', '--target=wasm32-wasip1', '--sysroot=${sdk}/share/wasi-sysroot']
cpp = ['${sdk}/bin/clang++', '--target=wasm32-wasip1', '--sysroot=${sdk}/share/wasi-sysroot']
ar = '${sdk}/bin/llvm-ar'
ranlib = '${sdk}/bin/llvm-ranlib'
strip = '${sdk}/bin/llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
# wasi-libc hides the emulated signal and mman bits behind these. FreeType's
# rasteriser recovers from a cell-pool overflow with longjmp, and its public
# headers include <setjmp.h>, which WebAssembly only has through exception
# handling -- so the whole stack, and the module that links it, is built with
# it. Every current browser runs that; the DSP
# module does not link any of this and stays plain. No threads in
# wasm32-wasip1, and the editor draws from one thread: CAIRO_NO_MUTEX. Cairo
# 1.18 passes pixman_image_t** as an atomic pointer, an error in clang 20+.
c_args = ['-msimd128', '-DCAIRO_NO_MUTEX=1', '-Wno-error=incompatible-pointer-types', '-mexception-handling', '-mllvm', '-wasm-enable-sjlj', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_MMAN', '-D_WASI_EMULATED_PROCESS_CLOCKS']
# No c_link_args: meson wraps them in --start-group, which wasm-ld rejects, and
# every link test then fails. The module that links these libraries adds
# -lsetjmp -lwasi-emulated-signal -lwasi-emulated-mman itself.

[host_machine]
system = 'wasi'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'
INI

cd "$work"
fetch() { [ -f "$2" ] || curl -sSL -o "$2" "$1"; [ -d "$3" ] || tar xf "$2"; }
fetch "https://cairographics.org/releases/${PIXMAN}.tar.xz" "${PIXMAN}.tar.xz" "$PIXMAN"
fetch "https://cairographics.org/releases/${CAIRO}.tar.xz" "${CAIRO}.tar.xz" "$CAIRO"
fetch "https://download.savannah.gnu.org/releases/freetype/${FREETYPE}.tar.xz" "${FREETYPE}.tar.xz" "$FREETYPE"

export PKG_CONFIG_LIBDIR="${prefix}/lib/pkgconfig"
export PKG_CONFIG_PATH=""

# -Db_ndebug=true for the same reason as in build-windows-cairo.sh: no
# assert() from inside Cairo may survive into a shipped plugin.
common=(--cross-file="$cross" --prefix="$prefix" --buildtype=release
        --default-library=static -Db_ndebug=true --wrap-mode=nodownload)

rm -rf "${work}/${FREETYPE}/_b" "${work}/${PIXMAN}/_b" "${work}/${CAIRO}/_b"
"$MESON" setup "${work}/${FREETYPE}/_b" "${work}/${FREETYPE}" "${common[@]}" \
   -Dzlib=disabled -Dpng=disabled -Dbzip2=disabled -Dbrotli=disabled -Dharfbuzz=disabled \
   -Dtests=disabled
ninja -C "${work}/${FREETYPE}/_b" install

"$MESON" setup "${work}/${PIXMAN}/_b" "${work}/${PIXMAN}" "${common[@]}" \
   -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled \
   -Dloongson-mmi=disabled -Dmmx=disabled -Dsse2=disabled -Dssse3=disabled -Dvmx=disabled \
   -Darm-simd=disabled -Dneon=disabled -Da64-neon=disabled -Dmips-dspr2=disabled -Drvv=disabled
ninja -C "${work}/${PIXMAN}/_b" install

"$MESON" setup "${work}/${CAIRO}/_b" "${work}/${CAIRO}" "${common[@]}" \
   -Dxlib=disabled -Dxcb=disabled -Dfreetype=enabled -Dfontconfig=disabled \
   -Dpng=disabled -Dzlib=disabled -Dglib=disabled -Dtests=disabled \
   -Dspectre=disabled -Dsymbol-lookup=disabled -Dgtk_doc=false -Dquartz=disabled \
   -Ddwrite=disabled -Dlzo=disabled
ninja -C "${work}/${CAIRO}/_b" install

echo
echo "WASI Cairo installed in ${prefix}"
