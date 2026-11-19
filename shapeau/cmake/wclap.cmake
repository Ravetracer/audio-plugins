# The WCLAP: Shapeau compiled to WebAssembly with wasi-sdk, for browser hosts
# (openDAW first). See ../shared/web/README.md for the design.
#
#   cmake -S . -B build-wasm -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake \
#         -DWASI_SDK_PREFIX=<wasi-sdk>
#   ninja -C build-wasm
#
# gives build-wasm/shapeau.wclap/ and shapeau.wclap.tar.gz, which hosts import:
#
#   module.wasm  the plugin: engine, parameters, state, presets, and the
#                clap.gui webview API with clap.webview/3 (src/plugin.cpp)
#   ui.wasm      the editor: the window fork, the shape editor and the preset
#                browser, drawn with Cairo into the page's canvas
#                (src/web/uimain.cpp), talking to the plugin only through the
#                webview messages (src/web/protocol.h)
#   index.html   the page, with the shared runtime (../shared/web)
#   fonts/       Noto Sans and its licence
#
# and shapeau-wclap-selftest.wasm, the self-test, run with
#   node ../shared/tools/run-wasi.mjs build-wasm/shapeau-wclap-selftest.wasm

# Included from CMakeLists.txt after CLAP_INCLUDE_DIR and the embedded preset
# table are set up.

include("${PLUGINCORE_SHARED_DIR}/cmake/wclap.cmake")

# wasi-sdk's libc++ has no exception runtime, and nothing here throws. 128-bit
# SIMD lets the compiler vectorise the per-sample loops (measured: see
# STATUS.md).
set(SHAPEAU_WASM_FLAGS "-msimd128" CACHE STRING "Target flags of the WCLAP build")
add_compile_options(-fno-exceptions -fno-math-errno ${SHAPEAU_WASM_FLAGS})

set(PLUGINCORE_WITH_GUI OFF)
add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)

set(SHAPEAU_WASM_DSP
   src/plugin.cpp
   src/preset_provider.cpp
   src/preset.cpp
   src/presetlibrary.cpp
   src/params.cpp
   src/gui/curvemodel.cpp
   src/dsp/curve.cpp
   src/dsp/engine.cpp
)

add_library(Shapeau-impl STATIC ${SHAPEAU_WASM_DSP} "${SHAPEAU_GENERATED_HEADER}")
add_dependencies(Shapeau-impl shapeau_presets)
target_link_libraries(Shapeau-impl PUBLIC plugincore::shared)
target_include_directories(Shapeau-impl PUBLIC
   "${CLAP_INCLUDE_DIR}"
   "${CMAKE_CURRENT_SOURCE_DIR}/src"
   "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_options(Shapeau-impl PRIVATE -Wall -Wextra)
target_compile_definitions(Shapeau-impl PRIVATE SHAPEAU_CMAKE_VERSION="${PROJECT_VERSION}")

add_executable(Shapeau-wclap src/entry.cpp)
target_link_libraries(Shapeau-wclap PRIVATE Shapeau-impl)
set_target_properties(Shapeau-wclap PROPERTIES OUTPUT_NAME "Shapeau")
# wclap_module() gives it a 1 MB stack: the engine copies the whole CurveSet
# (21 KB) onto the stack every block, and a preset load parses one beside it.
wclap_module(Shapeau-wclap)

# The self-test: the same objects as a WASI command.
if(SHAPEAU_BUILD_TOOLS)
   add_executable(shapeau-wclap-selftest tools/wclap_selftest.cpp src/entry.cpp)
   target_link_libraries(shapeau-wclap-selftest PRIVATE Shapeau-impl)
   set_target_properties(shapeau-wclap-selftest PROPERTIES SUFFIX ".wasm")
   target_link_options(shapeau-wclap-selftest PRIVATE "-Wl,--strip-debug" "-Wl,-z,stack-size=1048576")
endif()

# ------------------------------------------------------------------ the editor
if(WCLAP_HAVE_CAIRO)
   add_executable(Shapeau-ui
      src/web/uimain.cpp
      src/web/webfonts.cpp
      src/gui/gui.cpp
      src/gui/window.cpp
      src/gui/curvepanel.cpp
      src/gui/curvemodel.cpp
      src/presetlibrary.cpp
      src/preset.cpp
      src/params.cpp
      src/dsp/curve.cpp
      "${SHAPEAU_GENERATED_HEADER}"
   )
   add_dependencies(Shapeau-ui shapeau_presets)
   target_include_directories(Shapeau-ui PRIVATE
      "${CLAP_INCLUDE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_BINARY_DIR}/generated"
   )
   target_compile_options(Shapeau-ui PRIVATE -Wall -Wextra
      "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/web/webfonts.h")
   target_compile_definitions(Shapeau-ui PRIVATE SHAPEAU_CMAKE_VERSION="${PROJECT_VERSION}")
   target_link_libraries(Shapeau-ui PRIVATE plugincore::shared)
   wclap_ui(Shapeau-ui)

   set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
   wclap_bundle(NAME shapeau MODULE Shapeau-wclap UI Shapeau-ui PAGE "${_web}"
      FONTS "${_web}/fonts/NotoSans-Regular.ttf" "${_web}/fonts/NotoSans-Bold.ttf"
            "${_web}/fonts/OFL.txt")
   message(STATUS "Shapeau: building the web editor (ui.wasm) against ${WCLAP_CAIRO}")
else()
   message(WARNING "Shapeau: no wasm Cairo at WCLAP_CAIRO=${WCLAP_CAIRO}; no WCLAP bundle")
endif()
