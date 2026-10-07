cmake_minimum_required(VERSION 3.21)
project(Substrike VERSION 0.3.0 LANGUAGES C CXX)

# Substrike builds as a CLAP and, through the clap-wrapper, as a VST3, for
# Linux and Windows (cross-compiled with mingw-w64). Like Aurum it does not use
# ../shared; the Windows and VST3 blocks below follow ../aurum/CMakeLists.txt,
# and ../saeure-kiste/CMakeLists.txt is where the reasons for each link option
# are written down.
#
# Phase 1 has no editor, so there is no Cairo or X11 dependency yet; hosts show
# their generic parameter view.

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

add_library(substrike-impl STATIC
    src/dsp/Engine.cpp
    src/dsp/Sources.cpp
    src/dsp/Drive.cpp
    src/dsp/Effects.cpp
    src/plugin/Params.cpp
    src/plugin/SubstrikePlugin.cpp
    src/state/StateIO.cpp
)
target_include_directories(substrike-impl PUBLIC src)
# Lets src/substrike.h static_assert that kPluginVersion matches project().
target_compile_definitions(substrike-impl PUBLIC SUBSTRIKE_CMAKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(substrike-impl PUBLIC clap)

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
endif()
