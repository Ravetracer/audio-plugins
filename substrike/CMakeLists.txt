cmake_minimum_required(VERSION 3.21)
project(Substrike VERSION 0.8.0 LANGUAGES C CXX)

# Substrike builds as a CLAP and, through the clap-wrapper, as a VST3, for
# Linux and Windows (cross-compiled with mingw-w64). Like Aurum it does not use
# ../shared; the Windows and VST3 blocks below follow ../aurum/CMakeLists.txt,
# and ../saeure-kiste/CMakeLists.txt is where the reasons for each link option
# are written down.
#
# The editor is drawn with Cairo, on X11 (Linux) or win32 (Windows); the window
# backends and widgets come from ../aurum and belong to Substrike from then on.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "Substrike builds for Linux and for Windows only.")
endif()

set(SUBSTRIKE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CLAP" CACHE PATH "Directory holding clap, clap-wrapper and vst3sdk")
option(SUBSTRIKE_BUILD_VST3 "Also build a VST3 through the clap-wrapper" ON)
# The renderer and self-test. release.sh switches them off as <PROJECT>_BUILD_TOOLS.
option(SUBSTRIKE_BUILD_TOOLS "Build the offline renderer and self-test" ON)

# CLAP plugins live in ~/.clap on Linux. A cross build has no business guessing
# a path on the target machine (release.sh sets it to a staging tree).
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.clap" CACHE PATH "Install prefix" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
set(SUBSTRIKE_ARCH_FLAGS "-march=x86-64-v3" CACHE STRING "Target architecture flags")
add_compile_options(${SUBSTRIKE_ARCH_FLAGS} -fno-math-errno)

# On Windows every binary links the runtime in, so nothing needs MinGW DLLs
# beside it -- not the plugin, and not the renderer under wine, which otherwise
# dies before main() with exit code 53.
function(substrike_static_runtime target)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_options(${target} PRIVATE
            "-static-libgcc" "-static-libstdc++"
            "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic")
    endif()
endfunction()

set(CLAP_SDK_ROOT "${SUBSTRIKE_SDK_DIR}/clap" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)

# ------------------------------------------------------------------- gui deps
#
# pkg-config on this machine describes this machine, so a cross build must not
# consult it. On Windows, SUBSTRIKE_WIN_CAIRO points at a Cairo cross-built with
# its win32 backend (../setup-winbuild.sh puts one in ../winbuild/cairo-mingw).
add_library(substrike-gui-deps INTERFACE)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SUBSTRIKE_WIN_CAIRO "${CMAKE_CURRENT_SOURCE_DIR}/../winbuild/cairo-mingw"
        CACHE PATH "Cairo cross-built for Windows (setup-winbuild.sh)")
    if (NOT EXISTS "${SUBSTRIKE_WIN_CAIRO}/lib/libcairo.a")
        # Like Aurum: a kick designer with no window is not something to ship
        # by accident.
        message(FATAL_ERROR "Substrike: no Windows Cairo at SUBSTRIKE_WIN_CAIRO=${SUBSTRIKE_WIN_CAIRO}; run ../setup-winbuild.sh")
    endif()
    # Both levels: the code says <cairo/cairo.h>.
    target_include_directories(substrike-gui-deps INTERFACE
        "${SUBSTRIKE_WIN_CAIRO}/include" "${SUBSTRIKE_WIN_CAIRO}/include/cairo")
    target_link_directories(substrike-gui-deps INTERFACE "${SUBSTRIKE_WIN_CAIRO}/lib")
    # Cairo's win32 backend draws and measures text through GDI; shell32 and
    # ole32 are drag and drop, comdlg32 the file chooser.
    target_link_libraries(substrike-gui-deps INTERFACE
        cairo pixman-1 gdi32 msimg32 user32 shell32 comdlg32 ole32 uuid)
    # Cairo's headers declare everything dllimport unless told the library is
    # static, which is how it is built here.
    target_compile_definitions(substrike-gui-deps INTERFACE CAIRO_WIN32_STATIC_BUILD)
    set(SUBSTRIKE_WINDOW_SOURCE src/gui/Win32Window.cpp)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SUBSTRIKE_GUI REQUIRED IMPORTED_TARGET cairo x11)
    target_link_libraries(substrike-gui-deps INTERFACE PkgConfig::SUBSTRIKE_GUI)
    set(SUBSTRIKE_WINDOW_SOURCE src/gui/X11Window.cpp)
endif()

# The factory presets, compiled in: presets/*.substrike as a header of string
# literals. A preset added or removed is picked up at the next configure; an
# edit to one at the next build.
file(GLOB SUBSTRIKE_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/*.substrike")
set(SUBSTRIKE_PRESET_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/FactoryPresetData.h")
add_custom_command(
    OUTPUT "${SUBSTRIKE_PRESET_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -DPRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets"
            -DOUTPUT="${SUBSTRIKE_PRESET_HEADER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_presets.cmake"
    DEPENDS ${SUBSTRIKE_PRESET_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_presets.cmake"
    COMMENT "Embedding the factory presets")
add_custom_target(substrike-presets DEPENDS "${SUBSTRIKE_PRESET_HEADER}")

add_library(substrike-impl STATIC
    src/dsp/Engine.cpp
    src/dsp/Sources.cpp
    src/dsp/Drive.cpp
    src/dsp/Effects.cpp
    src/dsp/TimeEffects.cpp
    src/plugin/Params.cpp
    src/plugin/HitRender.cpp
    src/plugin/Player.cpp
    src/plugin/SubstrikePlugin.cpp
    src/state/StateIO.cpp
    src/state/Settings.cpp
    src/state/Presets.cpp
    src/state/PresetProvider.cpp
    src/gui/Widget.cpp
    src/gui/Widgets.cpp
    src/gui/FileDialog.cpp
    src/gui/HitPreview.cpp
    src/gui/CurveEditor.cpp
    src/gui/Editor.cpp
    src/gui/PresetBrowser.cpp
    ${SUBSTRIKE_WINDOW_SOURCE}
)
target_include_directories(substrike-impl PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(substrike-impl substrike-presets)
# Lets src/substrike.h static_assert that kPluginVersion matches project().
target_compile_definitions(substrike-impl PUBLIC SUBSTRIKE_CMAKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(substrike-impl PUBLIC clap substrike-gui-deps Threads::Threads)

# ------------------------------------------------------------- the CLAP module
add_library(Substrike MODULE src/entry.cpp)
target_link_libraries(Substrike PRIVATE substrike-impl)
set_target_properties(Substrike PROPERTIES PREFIX "" OUTPUT_NAME "Substrike" SUFFIX ".clap")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # clap_entry carries dllexport from CLAP_EXPORT; --exclude-all-symbols
    # keeps the rest of the DLL to itself.
    target_link_options(Substrike PRIVATE "-Wl,--exclude-all-symbols" "-Wl,--no-undefined")
    substrike_static_runtime(Substrike)
else()
    target_link_options(Substrike PRIVATE "-Wl,--no-undefined")
endif()

# ------------------------------------------------------------------------ VST3
#
# The same plugin behind the clap-wrapper. Needs CLAP/clap-wrapper and
# CLAP/vst3sdk (3.8 or newer), with ../shared/patches/ applied to the wrapper.
# release.sh builds the target Substrike-vst3 and picks the bundle up from
# build/vst3/Substrike.vst3.
if (SUBSTRIKE_BUILD_VST3)
    if (NOT EXISTS "${SUBSTRIKE_SDK_DIR}/clap-wrapper/cmake/wrap_vst3.cmake")
        message(FATAL_ERROR "Substrike: no clap-wrapper at ${SUBSTRIKE_SDK_DIR}/clap-wrapper")
    endif()
    if (NOT EXISTS "${SUBSTRIKE_SDK_DIR}/vst3sdk/public.sdk/source/main/pluginfactory.cpp")
        message(FATAL_ERROR "Substrike: no VST3 SDK at ${SUBSTRIKE_SDK_DIR}/vst3sdk")
    endif()
    set(VST3_SDK_ROOT "${SUBSTRIKE_SDK_DIR}/vst3sdk" CACHE PATH "" FORCE)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_OUTPUT_NAME "Substrike" CACHE STRING "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS FALSE CACHE BOOL "" FORCE)
    add_subdirectory("${SUBSTRIKE_SDK_DIR}/clap-wrapper" clap-wrapper EXCLUDE_FROM_ALL)

    add_library(Substrike-vst3 MODULE src/entry.cpp)
    target_link_libraries(Substrike-vst3 PRIVATE substrike-impl)
    target_add_vst3_wrapper(
        TARGET Substrike-vst3
        OUTPUT_NAME "Substrike"
        BUNDLE_IDENTIFIER "de.ravetracer.substrike.vst3"
        BUNDLE_VERSION "${PROJECT_VERSION}"
        ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vst3"
    )
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # The wrapper leaves CMake's "lib" prefix on a mingw MODULE, and a
        # VST3 bundle's binary must be named exactly like the bundle.
        set_target_properties(Substrike-vst3 PROPERTIES PREFIX "")
        # GetPluginFactory carries dllexport from SMTG_EXPORT_SYMBOL, so no
        # --exclude-all-symbols here.
        target_link_options(Substrike-vst3 PRIVATE "-Wl,--no-undefined")
        substrike_static_runtime(Substrike-vst3)
    endif()
endif()

# ---------------------------------------------------------------------- install
install(TARGETS Substrike LIBRARY DESTINATION "Substrike")

# ----------------------------------------------------------------------- tools
if (SUBSTRIKE_BUILD_TOOLS)
    # The offline renderer loads the built .clap the way a host does, so the
    # self-test exercises exactly what gets installed.
    add_executable(substrike-render tools/render.cpp)
    target_include_directories(substrike-render PRIVATE src)
    target_link_libraries(substrike-render PRIVATE clap ${CMAKE_DL_LIBS})
    substrike_static_runtime(substrike-render)
    add_dependencies(substrike-render Substrike)

    # Kick metrics (pitch, envelope, click, harmonics) from WAV files: what the
    # factory presets are checked against the reference set with.
    add_executable(substrike-metrics tools/metrics.cpp)
    substrike_static_runtime(substrike-metrics)

    # Parses every factory preset and reports what it got wrong: unknown
    # keys, values out of range, missing metadata.
    add_executable(substrike-preset-check tests/preset_check.cpp)
    target_link_libraries(substrike-preset-check PRIVATE substrike-impl)
    substrike_static_runtime(substrike-preset-check)

    # The editor rendered offscreen to a PNG, with frame timings.
    add_executable(substrike-gui-snapshot tests/gui_snapshot.cpp)
    target_link_libraries(substrike-gui-snapshot PRIVATE substrike-impl)
    substrike_static_runtime(substrike-gui-snapshot)

    # A minimal CLAP host that opens the plugin's window: X11 on Linux, a real
    # win32 window on Windows (under wine, how the win32 backend gets
    # exercised from here at all).
    add_executable(substrike-gui-host tests/clap_gui_host.cpp)
    target_link_libraries(substrike-gui-host PRIVATE clap Threads::Threads)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(substrike-gui-host PRIVATE user32 gdi32)
        substrike_static_runtime(substrike-gui-host)
    else()
        target_link_libraries(substrike-gui-host PRIVATE PkgConfig::SUBSTRIKE_GUI ${CMAKE_DL_LIBS})
    endif()
endif()
