cmake_minimum_required(VERSION 3.21)
project(SidMidi VERSION 0.5.1 LANGUAGES C CXX)

# SidMidi builds as a CLAP and, through the clap-wrapper, as a VST3, for Linux
# and Windows (cross-compiled with mingw-w64). Like Aurum and Substrike it does
# not use ../shared; the Windows and VST3 blocks follow ../substrike.
#
# Licence: this folder is GPL-2.0-or-later, because the SID chips are reSIDfp
# (third_party/libresidfp). The rest of the repository stays MIT.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "SidMidi builds for Linux and for Windows only.")
endif()

set(SIDMIDI_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CLAP" CACHE PATH "Directory holding clap, clap-wrapper and vst3sdk")
option(SIDMIDI_BUILD_PLUGIN "Build the CLAP plugin" ON)
option(SIDMIDI_BUILD_VST3 "Also build a VST3 through the clap-wrapper" ON)
option(SIDMIDI_BUILD_TOOLS "Build the headless runner and the CPU test" ON)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.clap" CACHE PATH "Install prefix" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
set(SIDMIDI_ARCH_FLAGS "-march=x86-64-v3" CACHE STRING "Target architecture flags")
add_compile_options(${SIDMIDI_ARCH_FLAGS} -fno-math-errno)

function(sidmidi_static_runtime target)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_options(${target} PRIVATE
            "-static-libgcc" "-static-libstdc++"
            "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic")
    endif()
endfunction()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- reSIDfp
#
# libresidfp 1.2.3, vendored with our patches (third_party/patches/, see
# third_party/README.md). Its two generated headers (siddefs-fp.h,
# residfp/sidversion.h) are written out in the tree.
file(GLOB RESIDFP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libresidfp/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libresidfp/src/resample/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libresidfp/src/residfp/*.cpp")
add_library(sidmidi-residfp STATIC ${RESIDFP_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libresidfp/src/version.cc")
target_include_directories(sidmidi-residfp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libresidfp/src")
target_compile_options(sidmidi-residfp PRIVATE -w -O3)
target_link_libraries(sidmidi-residfp PUBLIC Threads::Threads)

# ---------------------------------------------------------------- embedded data
set(SIDMIDI_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SIDMIDI_EMBED_HEADER "${SIDMIDI_GENERATED}/EmbeddedData.h")
file(GLOB SIDMIDI_PALETTES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/palettes/*.vpl")
file(GLOB SIDMIDI_CCMAPS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ccmaps/*.ccmap")
add_custom_command(
    OUTPUT "${SIDMIDI_EMBED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DPALETTE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/palettes"
            "-DCCMAP_DIR=${CMAKE_CURRENT_SOURCE_DIR}/ccmaps"
            "-DOUTPUT=${SIDMIDI_EMBED_HEADER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_data.cmake"
    DEPENDS ${SIDMIDI_PALETTES} ${SIDMIDI_CCMAPS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_data.cmake"
    COMMENT "Embedding the palettes and the factory CC maps")
# HerMIDI's PIC firmware (WTF-licensed, from SID-Wizard), run on the emulated PIC.
set(SIDMIDI_HERMIDI_HEADER "${SIDMIDI_GENERATED}/HerMidiFirmware.h")
add_custom_command(
    OUTPUT "${SIDMIDI_HERMIDI_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DHEX=${CMAKE_CURRENT_SOURCE_DIR}/third_party/hermidi/HerMIDI-PIC.hex"
            "-DOUTPUT=${SIDMIDI_HERMIDI_HEADER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_hermidi.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hermidi/HerMIDI-PIC.hex" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_hermidi.cmake"
    COMMENT "Embedding the HerMIDI firmware")
add_custom_target(sidmidi-embedded DEPENDS "${SIDMIDI_EMBED_HEADER}" "${SIDMIDI_HERMIDI_HEADER}")

# ---------------------------------------------------------------- the machine
add_library(sidmidi-emu STATIC
    src/emu/C64.cpp
    src/emu/Cia6526.cpp
    src/emu/Vic.cpp
    src/emu/SidBank.cpp
    src/emu/MidiCart.cpp
    src/emu/Cartridge.cpp
    src/emu/D64.cpp
    src/emu/VirtualDrive.cpp
    src/emu/Via6522.cpp
    src/emu/Gcr.cpp
    src/emu/Drive1541.cpp
    src/emu/Pic12F509.cpp
    src/emu/HerMidi.cpp
    src/engine/Engine.cpp
    src/engine/Resources.cpp
)
target_include_directories(sidmidi-emu PUBLIC src "${SIDMIDI_GENERATED}")
add_dependencies(sidmidi-emu sidmidi-embedded)
target_compile_options(sidmidi-emu PRIVATE -O3)
target_link_libraries(sidmidi-emu PUBLIC sidmidi-residfp)

if (SIDMIDI_BUILD_PLUGIN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/plugin.cmake")
    include(cmake/plugin.cmake)
endif()

# ---------------------------------------------------------------- tools
if (SIDMIDI_BUILD_TOOLS)
    # The machine without a host: boots, autostarts, plays a MIDI script,
    # writes a WAV and screenshots (through Cairo; the Windows build uses the
    # cross-built one, never this machine's).
    add_executable(sidmidi-run tools/run.cpp)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(sidmidi-run PRIVATE sidmidi-emu sidmidi-gui-deps)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SIDMIDI_CAIRO REQUIRED IMPORTED_TARGET cairo)
        target_link_libraries(sidmidi-run PRIVATE sidmidi-emu PkgConfig::SIDMIDI_CAIRO)
    endif()
    sidmidi_static_runtime(sidmidi-run)

    if (SIDMIDI_BUILD_PLUGIN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # The built .clap loaded as a host loads it: program, notes, state round trip.
        # The keyboard mapping, symbolic and positional.
        add_executable(sidmidi-keytest tools/keytest.cpp)
        target_link_libraries(sidmidi-keytest PRIVATE sidmidi-impl)

        # The window rendered offscreen, for the manual's screenshots.
        add_executable(sidmidi-gui-snapshot tools/gui-snapshot.cpp)
        target_link_libraries(sidmidi-gui-snapshot PRIVATE sidmidi-impl)

        add_executable(sidmidi-selftest tools/selftest.cpp)
        target_include_directories(sidmidi-selftest PRIVATE src)
        target_link_libraries(sidmidi-selftest PRIVATE clap ${CMAKE_DL_LIBS})
        add_dependencies(sidmidi-selftest SidMidi)

    endif()
    if (SIDMIDI_BUILD_PLUGIN)
        # A minimal host that opens the window (X11; win32 under wine).
        add_executable(sidmidi-gui-host tests/clap_gui_host.cpp)
        target_link_libraries(sidmidi-gui-host PRIVATE clap Threads::Threads)
        if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
            target_link_libraries(sidmidi-gui-host PRIVATE user32 gdi32)
            sidmidi_static_runtime(sidmidi-gui-host)
        else()
            pkg_check_modules(SIDMIDI_X11 REQUIRED IMPORTED_TARGET x11)
            target_link_libraries(sidmidi-gui-host PRIVATE PkgConfig::SIDMIDI_X11 ${CMAKE_DL_LIBS})
        endif()
    endif()

    # The 6502 core against Klaus Dormann's functional and decimal tests
    # (fetched into the build tree by tools/fetch-cpu-tests.sh, never committed).
    add_executable(sidmidi-cputest tools/cputest.cpp)
    target_include_directories(sidmidi-cputest PRIVATE src)
    sidmidi_static_runtime(sidmidi-cputest)
endif()
