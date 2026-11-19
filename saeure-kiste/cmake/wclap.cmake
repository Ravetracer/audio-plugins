# The WCLAP (WebCLAP): SaeureKiste compiled to WebAssembly with wasi-sdk, for
# browser hosts such as openDAW. See shared/web/README.md for the architecture.
#
#   cmake -S . -B build-wasm -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake \
#         -DWASI_SDK_PREFIX=<wasi-sdk>
#   ninja -C build-wasm
#
# gives build-wasm/saeurekiste.wclap/ and build-wasm/saeurekiste.wclap.tar.gz:
#
#   module.wasm   the plugin: engine, sequencer, parameters, state, and the
#                 page's messages (src/web/protocol.h). Runs on the host's
#                 audio thread, so it has no window.
#   ui.wasm       the editor: the native window (gui/seqwindow.cpp) drawn into
#                 the page's canvas, over a second instance of the plugin that
#                 never processes audio and mirrors the first one through the
#                 page's messages (src/web/plugin_web.inc, src/web/ui_main.cpp).
#
# Included from CMakeLists.txt when CMAKE_SYSTEM_NAME is WASI. The p1 toolchain
# has no threads; nothing here starts one.

set(CMAKE_CXX_STANDARD 17)
add_compile_options(-fno-exceptions -fno-math-errno -msimd128)

set(PLUGINCORE_WITH_GUI OFF)
set(PLUGINCORE_SHARED_WINDOW OFF)
add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)

include("${PLUGINCORE_SHARED_DIR}/cmake/wclap.cmake")

# Everything both modules share: the engine, the patterns, the preset format.
add_library(SaeureKiste-core STATIC
   src/preset_provider.cpp
   src/preset.cpp
   src/params.cpp
   src/dsp/acid_engine.cpp
   src/dsp/drive.cpp
   src/dsp/delay.cpp
   src/dsp/wavetable.cpp
   src/dsp/chain.cpp
   src/dsp/chain_time.cpp
   src/pattern.cpp
   src/midifile.cpp
   src/patternimport.cpp
   "${SAEUREKISTE_GENERATED_HEADER}"
)
add_dependencies(SaeureKiste-core saeurekiste_presets)
target_link_libraries(SaeureKiste-core PUBLIC plugincore::shared)
target_include_directories(SaeureKiste-core PUBLIC
   "${CLAP_INCLUDE_DIR}"
   "${CMAKE_CURRENT_SOURCE_DIR}/src"
   "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_definitions(SaeureKiste-core PUBLIC
   SAEUREKISTE_CMAKE_VERSION="${PROJECT_VERSION}")

# module.wasm: plugin.cpp without the window; its gui extension offers the
# webview API and the page /index.html.
add_executable(SaeureKiste-wclap src/entry.cpp src/plugin.cpp)
target_link_libraries(SaeureKiste-wclap PRIVATE SaeureKiste-core)
set_target_properties(SaeureKiste-wclap PROPERTIES OUTPUT_NAME "SaeureKiste.wclap")
wclap_module(SaeureKiste-wclap)

# The self-test: the same objects as a WASI command, run with
#   node ../shared/tools/run-wasi.mjs build-wasm/saeurekiste-wclap-selftest.wasm
if(SAEUREKISTE_BUILD_TOOLS)
   add_executable(saeurekiste-wclap-selftest tools/wclap_selftest.cpp src/entry.cpp src/plugin.cpp)
   target_link_libraries(saeurekiste-wclap-selftest PRIVATE SaeureKiste-core)
   set_target_properties(saeurekiste-wclap-selftest PROPERTIES SUFFIX ".wasm")
   target_link_options(saeurekiste-wclap-selftest PRIVATE "-Wl,--strip-debug")
endif()

# ui.wasm: the editor. plugin.cpp again, with the window, as the stand-in the
# window edits; Cairo, pixman and FreeType from WCLAP_CAIRO.
if(WCLAP_HAVE_CAIRO)
   add_executable(SaeureKiste-ui
      src/web/ui_main.cpp
      src/web/web_fonts.cpp
      src/entry.cpp
      src/plugin.cpp
      src/gui/gui.cpp
      src/gui/seqwindow.cpp
      src/gui/filedialog.cpp
   )
   target_link_libraries(SaeureKiste-ui PRIVATE SaeureKiste-core)
   target_compile_definitions(SaeureKiste-ui PRIVATE SAEUREKISTE_WITH_GUI)
   # plugincore/gui/toolkit.h includes <X11/Xlib.h> outside Windows and macOS.
   # The window and the shared toolkit pick their fonts through Cairo's toy
   # API, which has no font configuration to ask in WebAssembly; this header
   # routes it to the fonts in the bundle (src/web/web_fonts.cpp).
   set_source_files_properties(src/gui/seqwindow.cpp src/gui/gui.cpp PROPERTIES
      COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/web/web_fonts.h")
   wclap_ui(SaeureKiste-ui)

   set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
   wclap_bundle(NAME saeurekiste MODULE SaeureKiste-wclap UI SaeureKiste-ui PAGE "${_web}"
      FONTS "${_web}/fonts/NotoSans-Regular.ttf" "${_web}/fonts/NotoSans-Bold.ttf"
            "${_web}/fonts/OFL.txt")
   message(STATUS "SaeureKiste: building the web editor (ui.wasm) against ${WCLAP_CAIRO}")
else()
   message(STATUS "SaeureKiste: no wasm Cairo at WCLAP_CAIRO=${WCLAP_CAIRO}; no WCLAP bundle")
endif()
