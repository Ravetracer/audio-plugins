# The WCLAP: Substrike compiled to WebAssembly with wasi-sdk, for browser hosts
# such as openDAW. See ../shared/web/README.md for the bundle and the editor's
# contract.
#
#   cmake -S . -B build-wasm -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake -DWASI_SDK_PREFIX=<wasi-sdk>
#   ninja -C build-wasm
#
# gives build-wasm/substrike.wclap/ and substrike.wclap.tar.gz, which is what a
# host imports:
#
#   module.wasm   the plugin: engine, parameters, state, presets, and the page
#                 protocol (plugin/WebProtocol.h) behind clap.webview
#   ui.wasm       the native editor, drawn with Cairo into the page's canvas;
#                 it renders the hit preview and the export itself
#
# The p1 toolchain has no threads: the hit preview renders a slice per frame
# (gui/HitPreview), and the file dialog is the page's picker.

# The engine, the parameter table and the state: in the plugin and the editor.
add_library(substrike-core STATIC
    src/dsp/Engine.cpp
    src/dsp/Sources.cpp
    src/dsp/Drive.cpp
    src/dsp/Effects.cpp
    src/dsp/TimeEffects.cpp
    src/plugin/Params.cpp
    src/plugin/HitRender.cpp
    src/plugin/Player.cpp
    src/state/StateIO.cpp
    src/state/Settings.cpp
    src/state/Presets.cpp
)
target_include_directories(substrike-core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(substrike-core substrike-presets)
# Lets src/substrike.h static_assert that kPluginVersion matches project().
target_compile_definitions(substrike-core PUBLIC SUBSTRIKE_CMAKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(substrike-core PUBLIC clap)

add_library(substrike-wclap-impl STATIC
    src/plugin/SubstrikePlugin.cpp
    src/state/PresetProvider.cpp
)
target_link_libraries(substrike-wclap-impl PUBLIC substrike-core)

include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/cmake/wclap.cmake")

add_executable(Substrike-wclap src/entry.cpp)
target_link_libraries(Substrike-wclap PRIVATE substrike-wclap-impl)
set_target_properties(Substrike-wclap PROPERTIES OUTPUT_NAME "Substrike.wclap")
wclap_module(Substrike-wclap)

# The self-test: the same objects as a WASI command, run with
#   node ../shared/tools/run-wasi.mjs build-wasm/substrike-wclap-selftest.wasm
if (SUBSTRIKE_BUILD_TOOLS)
    add_executable(substrike-wclap-selftest tests/wclap_selftest.cpp src/entry.cpp)
    target_link_libraries(substrike-wclap-selftest PRIVATE substrike-wclap-impl)
    set_target_properties(substrike-wclap-selftest PROPERTIES SUFFIX ".wasm")
    target_link_options(substrike-wclap-selftest PRIVATE "-Wl,--strip-debug")
endif()

# ------------------------------------------------------------------ the editor
#
# ui.wasm: the native editor (gui/), its preset browser and curve editors, in
# the page (gui/WebWindow, web/UiMain.cpp). It reaches the plugin only through
# the webview messages. Needs Cairo, pixman and FreeType for wasm32-wasip1, from
#   ../shared/cmake/build-wasm-cairo.sh <wasi-sdk> ../wasmbuild/cairo-wasi
if (WCLAP_HAVE_CAIRO)
    add_executable(Substrike-ui
        src/web/UiMain.cpp
        src/gui/WebWindow.cpp
        src/gui/WebFonts.cpp
        src/gui/FileDialog.cpp
        src/gui/HitPreview.cpp
        src/gui/Widget.cpp
        src/gui/Widgets.cpp
        src/gui/CurveEditor.cpp
        src/gui/Editor.cpp
        src/gui/PresetBrowser.cpp
    )
    target_link_libraries(Substrike-ui PRIVATE substrike-core)
    wclap_ui(Substrike-ui)

    set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
    wclap_bundle(NAME substrike MODULE Substrike-wclap UI Substrike-ui PAGE "${_web}"
        FONTS "${_web}/fonts/Inter-Medium.ttf" "${_web}/fonts/Inter-Bold.ttf" "${_web}/fonts/OFL.txt")
    message(STATUS "Substrike: building the web editor (ui.wasm) against ${WCLAP_CAIRO}")
else()
    message(STATUS "Substrike: no wasm Cairo at WCLAP_CAIRO=${WCLAP_CAIRO}; the WCLAP has no editor")
endif()
