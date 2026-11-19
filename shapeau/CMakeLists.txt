cmake_minimum_required(VERSION 3.16)
# macOS: std::filesystem needs 10.15. Set before project(), ignored elsewhere.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(Shapeau VERSION 0.6.0 LANGUAGES CXX)
if(APPLE)
   # The window's Cocoa view (src/gui/cocoaview.mm).
   enable_language(OBJCXX)
endif()

# Shapeau builds as a CLAP and as a VST3, for Linux and for Windows, and on
# macOS as a CLAP, a VST3 and an Audio Unit. macOS is built natively on
# GitHub's runners (.github/workflows/shapeau-macos.yml), never here.
#
# It is built on the PluginCore shared library -- the parameter model, the
# preset format, the window toolkit -- and expects it at ../shared. The shape
# editor is this plugin's own file (src/gui/window.cpp, forked from the shared
# window), because a drawable curve cannot be expressed as panels of knobs.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# WASI is the WCLAP build: the plugin compiled to WebAssembly for browser hosts
# (openDAW), with its editor as a second module for the page. See
# cmake/wclap.cmake.
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   set(SHAPEAU_WASM ON)
elseif(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows|Darwin)$")
   message(FATAL_ERROR
      "Shapeau builds for Linux, Windows and macOS. Nothing in it is specific to any "
      "of them beyond the window, which has a backend for each -- but it has never been "
      "built or run anywhere else, and a plugin nobody has run is worse than one that "
      "refuses to configure.")
endif()

option(SHAPEAU_BUILD_TOOLS "Build the offline render/verification host" ON)
option(SHAPEAU_BUILD_GUI "Build the plugin window (X11, win32 or Cocoa, with Cairo)" ON)
option(SHAPEAU_BUILD_VST3 "Also build a VST3 through the clap-wrapper" OFF)
option(SHAPEAU_BUILD_AUV2 "Also build an Audio Unit through the clap-wrapper (macOS only)" OFF)
if(NOT APPLE)
   set(SHAPEAU_BUILD_AUV2 OFF)
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT CMAKE_CROSSCOMPILING)
   if(APPLE)
      set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP" CACHE PATH "Install prefix" FORCE)
   else()
      set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.clap" CACHE PATH "Install prefix" FORCE)
   endif()
endif()

get_filename_component(PLUGINCORE_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared" ABSOLUTE)

# ---------------------------------------------------------------- CLAP headers
set(CLAP_INCLUDE_DIR "" CACHE PATH "Directory containing clap/clap.h")

if(NOT CLAP_INCLUDE_DIR)
   set(_clap_hints
      "${CMAKE_CURRENT_SOURCE_DIR}/../CLAP/clap/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/../clap/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/external/clap/include"
      "/usr/local/include"
      "/usr/include"
   )
   foreach(_hint ${_clap_hints})
      get_filename_component(_abs "${_hint}" ABSOLUTE)
      if(EXISTS "${_abs}/clap/clap.h")
         set(CLAP_INCLUDE_DIR "${_abs}" CACHE PATH "Directory containing clap/clap.h" FORCE)
         break()
      endif()
   endforeach()
endif()

if(NOT CLAP_INCLUDE_DIR OR NOT EXISTS "${CLAP_INCLUDE_DIR}/clap/clap.h")
   message(FATAL_ERROR
      "Could not find clap/clap.h. Configure with -DCLAP_INCLUDE_DIR=/path/to/clap/include")
endif()
message(STATUS "Shapeau: using CLAP headers from ${CLAP_INCLUDE_DIR}")

# -------------------------------------------------------- embedded preset table
set(SHAPEAU_PRESET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/presets")
file(GLOB SHAPEAU_PRESET_FILES "${SHAPEAU_PRESET_DIR}/*.shapeau")
set(SHAPEAU_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/presets_generated.h")

add_custom_command(
   OUTPUT "${SHAPEAU_GENERATED_HEADER}"
   COMMAND "${CMAKE_COMMAND}"
      -DPRESET_DIR=${SHAPEAU_PRESET_DIR}
      -DOUTPUT=${SHAPEAU_GENERATED_HEADER}
      -DNAMESPACE=shapeau
      -DPRESET_EXT=shapeau
      -P "${PLUGINCORE_SHARED_DIR}/cmake/embed_presets.cmake"
   DEPENDS ${SHAPEAU_PRESET_FILES} "${PLUGINCORE_SHARED_DIR}/cmake/embed_presets.cmake"
   COMMENT "Embedding ${SHAPEAU_PRESET_DIR} into presets_generated.h"
   VERBATIM
)
add_custom_target(shapeau_presets DEPENDS "${SHAPEAU_GENERATED_HEADER}")

if(SHAPEAU_WASM)
   include(cmake/wclap.cmake)
   return()
endif()

# ------------------------------------------------------------------- gui deps
set(SHAPEAU_GUI_SOURCES "")
if(SHAPEAU_BUILD_GUI AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
   if(NOT SHAPEAU_WIN_CAIRO OR NOT EXISTS "${SHAPEAU_WIN_CAIRO}/lib/libcairo.a")
      set(SHAPEAU_BUILD_GUI OFF)
      message(WARNING
         "Shapeau: no Windows Cairo at SHAPEAU_WIN_CAIRO, building without a window")
   else()
      set(SHAPEAU_GUI_DEPS_INCLUDE_DIRS
         "${SHAPEAU_WIN_CAIRO}/include" "${SHAPEAU_WIN_CAIRO}/include/cairo")
      set(SHAPEAU_GUI_DEPS_LIBRARY_DIRS "${SHAPEAU_WIN_CAIRO}/lib")
      set(SHAPEAU_GUI_DEPS_LIBRARIES cairo pixman-1 gdi32 msimg32 user32 ole32 uuid shell32
          comdlg32)
      set(SHAPEAU_GUI_SOURCES src/gui/gui.cpp src/gui/window.cpp src/gui/curvepanel.cpp)
      set(SHAPEAU_GUI_DEFINES CAIRO_WIN32_STATIC_BUILD)
      set(SHAPEAU_GUI_FOUND_WIN TRUE)
      message(STATUS "Shapeau: building the plugin window (Win32 + Cairo)")
   endif()
endif()

if(SHAPEAU_BUILD_GUI AND APPLE)
   # A static, universal Cairo with its Quartz backend (fonts) and pixman, from
   # ../shared/cmake/build-macos-cairo.sh. Nothing from Homebrew: its libraries
   # are dylibs for one architecture and for that machine's macOS version.
   # A hard failure, unlike Windows: no release path builds a Mac plugin
   # without a window, and one that silently lost it would still ship.
   set(SHAPEAU_MAC_CAIRO "${CMAKE_CURRENT_SOURCE_DIR}/../macbuild/cairo-macos"
      CACHE PATH "Static Cairo for macOS (shared/cmake/build-macos-cairo.sh)")
   if(NOT EXISTS "${SHAPEAU_MAC_CAIRO}/lib/libcairo.a")
      message(FATAL_ERROR
         "Shapeau: no macOS Cairo at SHAPEAU_MAC_CAIRO=${SHAPEAU_MAC_CAIRO}; "
         "run ../shared/cmake/build-macos-cairo.sh, or configure with -DSHAPEAU_BUILD_GUI=OFF")
   endif()
   set(SHAPEAU_GUI_DEPS_INCLUDE_DIRS
      "${SHAPEAU_MAC_CAIRO}/include" "${SHAPEAU_MAC_CAIRO}/include/cairo")
   set(SHAPEAU_GUI_DEPS_LIBRARIES
      "${SHAPEAU_MAC_CAIRO}/lib/libcairo.a" "${SHAPEAU_MAC_CAIRO}/lib/libpixman-1.a"
      "-framework Cocoa" "-framework CoreGraphics" "-framework CoreText"
      "-framework CoreFoundation" "-framework ApplicationServices")
   set(SHAPEAU_GUI_SOURCES src/gui/gui.cpp src/gui/window.cpp src/gui/curvepanel.cpp
       src/gui/cocoaview.mm)
   set(SHAPEAU_GUI_FOUND_MAC TRUE)
   message(STATUS "Shapeau: building the plugin window (Cocoa + Cairo)")
endif()

if(SHAPEAU_BUILD_GUI AND NOT SHAPEAU_GUI_FOUND_WIN AND NOT SHAPEAU_GUI_FOUND_MAC)
   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND)
      pkg_check_modules(SHAPEAU_GUI_DEPS QUIET x11 cairo)
   endif()
   if(SHAPEAU_GUI_DEPS_FOUND)
      set(SHAPEAU_GUI_SOURCES src/gui/gui.cpp src/gui/window.cpp src/gui/curvepanel.cpp)
      message(STATUS "Shapeau: building the plugin window (X11 + Cairo)")
   else()
      set(SHAPEAU_BUILD_GUI OFF)
      message(WARNING "Shapeau: x11/cairo not found, building without a GUI")
   endif()
endif()

# ------------------------------------------------------------------ shared code
if(SHAPEAU_BUILD_GUI)
   set(PLUGINCORE_WITH_GUI ON)
   set(PLUGINCORE_GUI_INCLUDE_DIRS ${SHAPEAU_GUI_DEPS_INCLUDE_DIRS})
   set(PLUGINCORE_GUI_LIBRARY_DIRS ${SHAPEAU_GUI_DEPS_LIBRARY_DIRS})
   set(PLUGINCORE_GUI_LIBRARIES ${SHAPEAU_GUI_DEPS_LIBRARIES})
   set(PLUGINCORE_GUI_DEFINES ${SHAPEAU_GUI_DEFINES})
endif()

# The window is this plugin's own fork (src/gui/window.cpp), so the shared one
# is not compiled. It would not link into the plugin anyway, being unused, but
# on macOS it would not even compile: it has no Cocoa backend.
set(PLUGINCORE_SHARED_WINDOW OFF)
add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)

# --------------------------------------------------------- the implementation
add_library(Shapeau-impl STATIC
   src/plugin.cpp
   src/preset_provider.cpp
   src/preset.cpp
   src/presetlibrary.cpp
   src/params.cpp
   src/gui/curvemodel.cpp
   src/dsp/curve.cpp
   src/dsp/engine.cpp
   ${SHAPEAU_GUI_SOURCES}
   "${SHAPEAU_GENERATED_HEADER}"
)
add_dependencies(Shapeau-impl shapeau_presets)

target_link_libraries(Shapeau-impl PUBLIC plugincore::shared)

target_include_directories(Shapeau-impl PUBLIC
   "${CLAP_INCLUDE_DIR}"
   "${CMAKE_CURRENT_SOURCE_DIR}/src"
   "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(APPLE)
   # Every retain in the .mm file has its release written out.
   set_source_files_properties(src/gui/cocoaview.mm PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
endif()

target_compile_options(Shapeau-impl PRIVATE
   -Wall -Wextra
   -fvisibility=hidden
   # Deliberately NOT -ffast-math: on x86 GCC that links crtfastmath.o, which
   # flips FTZ/DAZ for the whole host process. A plugin has no business doing
   # that to its host.
   -fno-math-errno
)

target_link_libraries(Shapeau-impl PUBLIC ${CMAKE_DL_LIBS})
target_compile_definitions(Shapeau-impl PRIVATE SHAPEAU_CMAKE_VERSION="${PROJECT_VERSION}")

if(SHAPEAU_BUILD_GUI)
   target_compile_definitions(Shapeau-impl PRIVATE SHAPEAU_WITH_GUI ${SHAPEAU_GUI_DEFINES})
   target_include_directories(Shapeau-impl PUBLIC ${SHAPEAU_GUI_DEPS_INCLUDE_DIRS})
   target_link_libraries(Shapeau-impl PUBLIC ${SHAPEAU_GUI_DEPS_LIBRARIES})
   target_link_directories(Shapeau-impl PUBLIC ${SHAPEAU_GUI_DEPS_LIBRARY_DIRS})
   find_package(Threads REQUIRED)
   target_link_libraries(Shapeau-impl PUBLIC Threads::Threads)
endif()

# --------------------------------------------------------------- the CLAP module
add_library(Shapeau MODULE src/entry.cpp)
target_link_libraries(Shapeau PRIVATE Shapeau-impl)
target_compile_options(Shapeau PRIVATE -Wall -Wextra -fvisibility=hidden)

if(WIN32)
   target_link_options(Shapeau PRIVATE
      "-Wl,--exclude-all-symbols"
      "-Wl,--no-undefined"
      "-static-libgcc" "-static-libstdc++"
      "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
   )
elseif(APPLE)
   # Only clap_entry is exported. Undefined symbols are an error by default
   # with Apple's linker, so there is nothing to add for that.
   target_link_options(Shapeau PRIVATE "-Wl,-exported_symbol,_clap_entry")
else()
   target_link_options(Shapeau PRIVATE
      "-Wl,--version-script=${PLUGINCORE_SHARED_DIR}/cmake/clap_entry.version"
      "-Wl,--no-undefined"
   )
endif()

if(APPLE)
   # A bundle, Shapeau.clap/Contents/MacOS/Shapeau, as hosts expect a CLAP on
   # macOS and as the clap-wrapper lays its own out.
   set_target_properties(Shapeau PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION clap
      LIBRARY_OUTPUT_NAME "Shapeau"
      MACOSX_BUNDLE_GUI_IDENTIFIER "de.ravetracer.shapeau"
      MACOSX_BUNDLE_BUNDLE_NAME "Shapeau"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
   )
else()
   set_target_properties(Shapeau PROPERTIES
      PREFIX ""
      OUTPUT_NAME "Shapeau"
      SUFFIX ".clap"
   )
endif()

# ------------------------------------------------------------- offline test host
if(SHAPEAU_BUILD_TOOLS)
   add_executable(shapeau-render
      tools/render.cpp
      src/preset.cpp
      src/params.cpp
      src/dsp/curve.cpp
      src/dsp/engine.cpp
   )
   target_link_libraries(shapeau-render PRIVATE plugincore::shared)
   target_include_directories(shapeau-render PRIVATE
      "${CLAP_INCLUDE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_BINARY_DIR}/generated"
   )
   target_compile_options(shapeau-render PRIVATE -Wall -Wextra)
   target_link_libraries(shapeau-render PRIVATE ${CMAKE_DL_LIBS})
   target_compile_definitions(shapeau-render PRIVATE
      SHAPEAU_PRESET_DIR="${SHAPEAU_PRESET_DIR}")

   if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      target_link_options(shapeau-render PRIVATE
         "-static-libgcc" "-static-libstdc++"
         "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
      )
   endif()

   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT APPLE)
      pkg_check_modules(GUIHOST_X11 QUIET x11)
   endif()
   # The GUI host embeds the editor in an X11 window of its own, so it exists
   # on Linux only; the Windows editor is checked by loading the .clap.
   if(GUIHOST_X11_FOUND)
      add_executable(shapeau-guihost tools/guihost.cpp)
      target_include_directories(shapeau-guihost PRIVATE
         "${CLAP_INCLUDE_DIR}" ${GUIHOST_X11_INCLUDE_DIRS})
      target_compile_options(shapeau-guihost PRIVATE -Wall -Wextra)
      target_link_libraries(shapeau-guihost PRIVATE ${CMAKE_DL_LIBS} ${GUIHOST_X11_LIBRARIES})
   endif()
endif()

# ------------------------------------------------------------------------ VST3
#
# The VST3 and the Audio Unit are the same plugin behind the clap-wrapper, not
# ports. The wrapper needs shared/patches/ applied: the VST 3.8 patch for the
# VST3, the main-bus patch for the Audio Unit.
if(SHAPEAU_BUILD_VST3 OR SHAPEAU_BUILD_AUV2)
   get_filename_component(SHAPEAU_CLAP_DIR "${CLAP_INCLUDE_DIR}/../.." ABSOLUTE)
   set(SHAPEAU_WRAPPER_DIR "${SHAPEAU_CLAP_DIR}/clap-wrapper"
      CACHE PATH "Directory containing a clap-wrapper checkout")
   set(SHAPEAU_VST3_SDK_DIR "${SHAPEAU_CLAP_DIR}/vst3sdk"
      CACHE PATH "Directory containing a VST3 SDK checkout")
   set(SHAPEAU_AU_SDK_DIR "${SHAPEAU_CLAP_DIR}/AudioUnitSDK"
      CACHE PATH "Directory containing Apple's AudioUnitSDK checkout")

   if(NOT EXISTS "${SHAPEAU_WRAPPER_DIR}/cmake/wrap_vst3.cmake")
      message(FATAL_ERROR "Shapeau: no clap-wrapper at ${SHAPEAU_WRAPPER_DIR}")
   endif()

   set(CLAP_SDK_ROOT "${SHAPEAU_CLAP_DIR}/clap" CACHE PATH "" FORCE)
   set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE CACHE BOOL "" FORCE)
   set(CLAP_WRAPPER_OUTPUT_NAME "Shapeau" CACHE STRING "" FORCE)
   set(CLAP_WRAPPER_BUILD_TESTS FALSE CACHE BOOL "" FORCE)
   if(SHAPEAU_BUILD_VST3)
      if(NOT EXISTS "${SHAPEAU_VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp")
         message(FATAL_ERROR "Shapeau: no VST3 SDK at ${SHAPEAU_VST3_SDK_DIR}")
      endif()
      set(VST3_SDK_ROOT "${SHAPEAU_VST3_SDK_DIR}" CACHE PATH "" FORCE)
   endif()
   if(SHAPEAU_BUILD_AUV2)
      # Apple's AudioUnitSDK (Apache 2.0), checked out beside the others:
      #   git clone --branch AudioUnitSDK-1.1.0 https://github.com/apple/AudioUnitSDK.git CLAP/AudioUnitSDK
      if(NOT EXISTS "${SHAPEAU_AU_SDK_DIR}/src/AudioUnitSDK")
         message(FATAL_ERROR "Shapeau: no AudioUnitSDK at ${SHAPEAU_AU_SDK_DIR}")
      endif()
      set(AUDIOUNIT_SDK_ROOT "${SHAPEAU_AU_SDK_DIR}" CACHE PATH "" FORCE)
   endif()

   add_subdirectory("${SHAPEAU_WRAPPER_DIR}" clap-wrapper EXCLUDE_FROM_ALL)
endif()

if(SHAPEAU_BUILD_VST3)
   set(SHAPEAU_VST3_ASSET_DIR "${CMAKE_BINARY_DIR}/vst3")

   add_library(Shapeau-vst3 MODULE src/entry.cpp)
   target_link_libraries(Shapeau-vst3 PRIVATE Shapeau-impl)
   target_compile_options(Shapeau-vst3 PRIVATE -Wall -Wextra)

   target_add_vst3_wrapper(
      TARGET Shapeau-vst3
      OUTPUT_NAME "Shapeau"
      BUNDLE_IDENTIFIER "de.ravetracer.shapeau.vst3"
      BUNDLE_VERSION "${PROJECT_VERSION}"
      ASSET_OUTPUT_DIRECTORY "${SHAPEAU_VST3_ASSET_DIR}"
   )

   if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      set_target_properties(Shapeau-vst3 PROPERTIES PREFIX "")
      target_link_options(Shapeau-vst3 PRIVATE
         "-Wl,--no-undefined"
         "-static-libgcc" "-static-libstdc++"
         "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
      )
   endif()
endif()

# ------------------------------------------------------------------------ AUv2
#
# macOS only: the same plugin as an Audio Unit, with the CLAP entry linked in
# as the VST3 has it. A music effect ("aumf"), not a plain effect: it takes
# MIDI notes as triggers, and an "aufx" gets none. The sidechain is the AU's
# second input bus. The manufacturer and subtype codes are Shapeau's for good:
# AU hosts keep projects by them, exactly as CLAP hosts keep them by the
# plugin id.
if(SHAPEAU_BUILD_AUV2)
   add_library(Shapeau-auv2 MODULE src/entry.cpp)
   target_link_libraries(Shapeau-auv2 PRIVATE Shapeau-impl)
   target_compile_options(Shapeau-auv2 PRIVATE -Wall -Wextra)
   target_add_auv2_wrapper(
      TARGET Shapeau-auv2
      OUTPUT_NAME "Shapeau"
      BUNDLE_IDENTIFIER "de.ravetracer.shapeau.auv2"
      BUNDLE_VERSION "${PROJECT_VERSION}"
      MANUFACTURER_NAME "Ravetracer"
      MANUFACTURER_CODE "Rvtr"
      SUBTYPE_CODE "Shpu"
      INSTRUMENT_TYPE "aumf"
   )
endif()

# ---------------------------------------------------------------------- install
install(TARGETS Shapeau LIBRARY DESTINATION "Shapeau")
install(FILES ${SHAPEAU_PRESET_FILES} DESTINATION "Shapeau/presets")

message(STATUS "Shapeau: 'cmake --install' will write to ${CMAKE_INSTALL_PREFIX}/Shapeau")
