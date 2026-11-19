# The WCLAP: ReSIDent compiled to WebAssembly with wasi-sdk, for browser hosts
# that load CLAP modules as .wasm (openDAW first). Included from
# CMakeLists.txt when CMAKE_SYSTEM_NAME is WASI:
#
#   cmake -S . -B build-wasm -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake \
#         -DWASI_SDK_PREFIX=<wasi-sdk>
#   ninja -C build-wasm
#
# gives build-wasm/resident.wclap/ and resident.wclap.tar.gz, which hosts
# import: module.wasm (the machine and the plugin, clap.gui with the webview
# API and clap.webview), ui.wasm (the editor, drawn by Cairo into the page's
# canvas), the page, the fonts and the licence texts. See
# ../shared/web/README.md for how the two modules work together.
#
# There is no ROM folder in a browser: the editor asks for the ROMs and hands
# them to the plugin, which keeps them in the project's state. No ROM is ever
# part of the bundle.

set(CLAP_SDK_ROOT "${RESIDENT_SDK_DIR}/clap" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)

include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/cmake/wclap.cmake")

# The plugin without a native window. The editor is ui.wasm, below.
add_library(resident-impl STATIC
    src/emu/Snapshot.cpp
    src/plugin/MachineSettings.cpp
    src/plugin/CcMap.cpp
    src/plugin/Params.cpp
    src/plugin/ResidentPlugin.cpp
    src/plugin/WebView.cpp
)
target_include_directories(resident-impl PUBLIC src)
target_compile_definitions(resident-impl PUBLIC RESIDENT_CMAKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(resident-impl PUBLIC resident-emu clap)

add_executable(ReSIDent-wclap src/entry.cpp)
target_link_libraries(ReSIDent-wclap PRIVATE resident-impl)
set_target_properties(ReSIDent-wclap PROPERTIES OUTPUT_NAME "ReSIDent.wclap")
wclap_module(ReSIDent-wclap)
# wasm's default stack is 64 KB, and an overflow silently writes into the
# data below it; the state is read through a 64 KB buffer on the stack.
target_link_options(ReSIDent-wclap PRIVATE "-Wl,-z,stack-size=1048576")

# The self-test: the same objects as a WASI command. It needs the ROMs, which
# tools/run-wclap-selftest.mjs hands in from a folder (never compiled in):
#   node tools/run-wclap-selftest.mjs build-wasm/resident-wclap-selftest.wasm _dev_/roms [program]
if (RESIDENT_BUILD_TOOLS)
    add_executable(resident-wclap-selftest tests/wclap_selftest.cpp src/entry.cpp)
    target_link_libraries(resident-wclap-selftest PRIVATE resident-impl)
    set_target_properties(resident-wclap-selftest PROPERTIES SUFFIX ".wasm")
    target_link_options(resident-wclap-selftest PRIVATE "-Wl,--strip-debug" "-Wl,-z,stack-size=1048576")
endif()

# ------------------------------------------------------------------ the editor
#
# ui.wasm: the native editor (gui/) with its widgets, pages and keyboard
# mapping, running in the plugin's page and drawn with Cairo into a canvas
# (gui/WebWindow, web/UiMain.cpp). It reaches the plugin only through the
# webview messages (plugin/WebProtocol.h); RemoteLink stands in for the plugin.
if (WCLAP_HAVE_CAIRO)
    add_executable(ReSIDent-ui
        src/web/UiMain.cpp
        src/gui/WebWindow.cpp
        src/gui/WebFonts.cpp
        src/gui/FileDialog.cpp
        src/gui/Widget.cpp
        src/gui/Widgets.cpp
        src/gui/Controls.cpp
        src/gui/Keymap.cpp
        src/gui/Editor.cpp
        src/plugin/MachineSettings.cpp
        src/plugin/CcMap.cpp
        src/plugin/Params.cpp
    )
    target_include_directories(ReSIDent-ui PRIVATE src)
    target_compile_definitions(ReSIDent-ui PRIVATE RESIDENT_CMAKE_VERSION="${PROJECT_VERSION}")
    # The machine's library for its names and tables (palettes, SID models,
    # MIDI interfaces, ROM recognition); the linker keeps only what is used.
    target_link_libraries(ReSIDent-ui PRIVATE resident-emu clap)
    wclap_ui(ReSIDent-ui)

    # The licence texts of everything in the bundle. Cairo, pixman and
    # FreeType are in ui.wasm; their texts come from the sources the wasm
    # Cairo was built from (shared/cmake/build-wasm-cairo.sh).
    set(_lic "${CMAKE_BINARY_DIR}/licences")
    file(MAKE_DIRECTORY "${_lic}")
    set(_files "${CMAKE_CURRENT_SOURCE_DIR}/COPYING" "${CMAKE_CURRENT_SOURCE_DIR}/web/THIRD-PARTY.txt")
    file(GLOB _cairo_src LIST_DIRECTORIES true "${WCLAP_CAIRO}/src/cairo-*")
    file(GLOB _pixman_src LIST_DIRECTORIES true "${WCLAP_CAIRO}/src/pixman-*")
    file(GLOB _freetype_src LIST_DIRECTORIES true "${WCLAP_CAIRO}/src/freetype-*")
    list(FILTER _cairo_src EXCLUDE REGEX "\\.tar\\.")
    list(FILTER _pixman_src EXCLUDE REGEX "\\.tar\\.")
    list(FILTER _freetype_src EXCLUDE REGEX "\\.tar\\.")
    foreach (_pair
             "_cairo_src;COPYING-LGPL-2.1;COPYING-LGPL-2.1-cairo.txt"
             "_cairo_src;COPYING;COPYING-cairo.txt"
             "_pixman_src;COPYING;COPYING-pixman.txt"
             "_freetype_src;docs/FTL.TXT;LICENSE-FreeType-FTL.txt")
        list(GET _pair 0 _var)
        list(GET _pair 1 _rel)
        list(GET _pair 2 _name)
        set(_src "")
        if (${_var})
            list(GET ${_var} 0 _dir)
            set(_src "${_dir}/${_rel}")
        endif()
        if (_src AND EXISTS "${_src}")
            configure_file("${_src}" "${_lic}/${_name}" COPYONLY)
            list(APPEND _files "${_lic}/${_name}")
        else()
            message(WARNING "ReSIDent WCLAP: no ${_rel} in ${WCLAP_CAIRO}/src for ${_name}; the bundle lacks it")
        endif()
    endforeach()

    # The bundle hosts import: resident.wclap/, packed as resident.wclap.tar.gz.
    set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
    wclap_bundle(NAME resident MODULE ReSIDent-wclap UI ReSIDent-ui PAGE "${_web}"
        LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        FONTS "${_web}/fonts/Inter-Medium.ttf" "${_web}/fonts/Inter-Bold.ttf" "${_web}/fonts/OFL.txt"
        FILES ${_files})
    message(STATUS "ReSIDent: building the web editor (ui.wasm) against ${WCLAP_CAIRO}")
else()
    message(STATUS "ReSIDent: no wasm Cairo at WCLAP_CAIRO=${WCLAP_CAIRO}; the WCLAP has no editor")
endif()
