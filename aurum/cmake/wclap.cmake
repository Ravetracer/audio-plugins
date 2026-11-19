# The WCLAP: Aurum compiled to WebAssembly with wasi-sdk, for hosts that load
# CLAP modules as .wasm (the WebCLAP browser host, wclap-bridge in a native
# DAW). Engine, parameters, state and presets as in the native plugin; no
# window -- the host shows its generic parameter view.
#
#   cmake -S . -B build-wasm -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
#   ninja -C build-wasm
#
# gives build-wasm/Aurum.wclap.wasm, a self-contained module. The p1 toolchain
# has no threads, so the decay designer computes synchronously
# (DecayDesigner::hasWorker()).
#
# Exports follow the WCLAP conventions (github.com/WebCLAP): clap_entry, a
# growable function table the host can add its callbacks to, malloc, and a
# memory that may grow to 2 GB. The link options are the ones clap-wrapper's
# make_clapfirst_plugins() uses for WASI.

set(CLAP_SDK_ROOT "${AURUM_SDK_DIR}/clap" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)

add_library(aurum-impl STATIC
    src/plugin/Params.cpp
    src/plugin/AurumPlugin.cpp
    src/state/StateIO.cpp
    src/state/Settings.cpp
    src/state/PresetManager.cpp
    src/state/FactoryPresets.cpp
    src/state/FfpImport.cpp
    src/state/AudioFile.cpp
    src/state/IrImport.cpp
    src/plugin/PresetSession.cpp
)
target_include_directories(aurum-impl PUBLIC src)
target_compile_definitions(aurum-impl PUBLIC AURUM_WITH_GUI=0)
target_link_libraries(aurum-impl PUBLIC aurum-dsp clap)

include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/cmake/wclap.cmake")

add_executable(Aurum-wclap src/entry.cpp)
target_link_libraries(Aurum-wclap PRIVATE aurum-impl)
set_target_properties(Aurum-wclap PROPERTIES OUTPUT_NAME "Aurum.wclap")
wclap_module(Aurum-wclap)

# The self-test: the same objects as a WASI command, run with
#   node ../shared/tools/run-wasi.mjs build-wasm/aurum-wclap-selftest.wasm
if (AURUM_BUILD_TOOLS)
    add_executable(aurum-wclap-selftest tests/wclap_selftest.cpp src/entry.cpp)
    target_link_libraries(aurum-wclap-selftest PRIVATE aurum-impl)
    target_include_directories(aurum-wclap-selftest PRIVATE src)
    set_target_properties(aurum-wclap-selftest PROPERTIES SUFFIX ".wasm")
    target_link_options(aurum-wclap-selftest PRIVATE "-Wl,--strip-debug")
endif()

# ------------------------------------------------------------------ the editor
#
# ui.wasm: the native editor (gui/), its preset browser and preset session,
# drawn with Cairo into the page's canvas (gui/WebWindow, web/UiMain.cpp). It
# runs in the page, not in the host's audio thread, and reaches the plugin
# only through the webview messages (plugin/WebProtocol.h). Needs Cairo,
# pixman and FreeType for wasm32-wasip1, from
#   ../shared/cmake/build-wasm-cairo.sh <wasi-sdk> ../wasmbuild/cairo-wasi
if (WCLAP_HAVE_CAIRO)
    add_executable(Aurum-ui
        src/web/UiMain.cpp
        src/gui/WebWindow.cpp
        src/gui/WebFonts.cpp
        src/gui/FileDialog.cpp
        src/gui/PresetBrowser.cpp
        src/gui/Widget.cpp
        src/gui/Widgets.cpp
        src/gui/EqPanel.cpp
        src/gui/Editor.cpp
        src/plugin/Params.cpp
        src/plugin/PresetSession.cpp
        src/state/StateIO.cpp
        src/state/Settings.cpp
        src/state/PresetManager.cpp
        src/state/FactoryPresets.cpp
        src/state/FfpImport.cpp
        src/state/AudioFile.cpp
        src/state/IrImport.cpp
    )
    target_include_directories(Aurum-ui PRIVATE src)
    target_link_libraries(Aurum-ui PRIVATE aurum-dsp clap)
    wclap_ui(Aurum-ui)

    # The bundle hosts import: aurum.wclap/, packed as aurum.wclap.tar.gz.
    set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
    wclap_bundle(NAME aurum MODULE Aurum-wclap UI Aurum-ui PAGE "${_web}"
        FONTS "${_web}/fonts/Inter-Medium.ttf" "${_web}/fonts/Inter-Bold.ttf" "${_web}/fonts/OFL.txt")
    message(STATUS "Aurum: building the web editor (ui.wasm) against ${WCLAP_CAIRO}")
else()
    message(STATUS "Aurum: no wasm Cairo at WCLAP_CAIRO=${WCLAP_CAIRO}; the WCLAP has no editor")
endif()
