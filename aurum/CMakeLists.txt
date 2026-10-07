cmake_minimum_required(VERSION 3.21)
project(Aurum VERSION 0.3.1 LANGUAGES C CXX)

# Aurum builds as a CLAP and, through the clap-wrapper, as a VST3, for Linux
# (X11 + Cairo) and for Windows (win32 + Cairo, cross-compiled with mingw-w64).
# It does not use ../shared; the Windows and VST3 blocks below follow
# ../saeure-kiste/CMakeLists.txt, which is where the reasons for each link
# option are written down.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "Aurum builds for Linux and for Windows; the window has a backend for those two only.")
endif()

set(AURUM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CLAP" CACHE PATH "Directory holding clap, clap-wrapper and vst3sdk")
option(AURUM_BUILD_PLUGIN "Build the CLAP plugin" ON)
option(AURUM_BUILD_VST3 "Also build a VST3 through the clap-wrapper" ON)
# The test programs. release.sh switches them off as <PROJECT>_BUILD_TOOLS.
option(AURUM_BUILD_TOOLS "Build the tests and test hosts" ON)

# CLAP plugins live in ~/.clap on Linux. A cross build has no business guessing
# a path on the target machine (release.sh sets it to a staging tree).
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.clap" CACHE PATH "Install prefix" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# x86-64-v3 (AVX2/FMA) keeps the attenuation banks vectorised.
set(AURUM_ARCH_FLAGS "-march=x86-64-v3" CACHE STRING "Target architecture flags")
add_compile_options(${AURUM_ARCH_FLAGS} -fno-math-errno)

# On Windows every binary links the runtime in, so nothing needs MinGW DLLs
# beside it -- not the plugin on a machine that has never seen a compiler, and
# not a test under wine, which otherwise dies before main() with exit code 53.
function(aurum_static_runtime target)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_options(${target} PRIVATE
            "-static-libgcc" "-static-libstdc++"
            "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic")
    endif()
endfunction()

add_library(aurum-dsp STATIC
    src/dsp/Geq.cpp
    src/dsp/DecayDesigner.cpp
    src/dsp/FdnTank.cpp
    src/dsp/ClassicTank.cpp
    src/dsp/PlateTank.cpp
    src/dsp/EarlyReflections.cpp
    src/dsp/PostEq.cpp
    src/dsp/ReverbEngine.cpp
)
target_include_directories(aurum-dsp PUBLIC src)
# Lets src/aurum.h static_assert that kPluginVersion matches project().
target_compile_definitions(aurum-dsp PUBLIC AURUM_CMAKE_VERSION="${PROJECT_VERSION}")
find_package(Threads REQUIRED)
target_link_libraries(aurum-dsp PUBLIC Threads::Threads)

if (AURUM_BUILD_TOOLS)
    add_subdirectory(tests)
endif()

if (AURUM_BUILD_PLUGIN)
    include(cmake/plugin.cmake)
endif()
