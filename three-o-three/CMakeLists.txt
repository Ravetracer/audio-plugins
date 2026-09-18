cmake_minimum_required(VERSION 3.16)
project(ThreeOhThree VERSION 0.1.0 LANGUAGES CXX)

# ThreeOhThree is Linux-only and CLAP-only, on purpose.
#
# It is built on the PluginCore shared library -- the parameter model, the
# preset format, the window -- and expects it at ../shared. See README.md for
# the directory layout this needs.
#
# There is no mingw cross build and no VST3 wrapper here yet. Both are wanted
# eventually; the tooling for them is already in this repository (shared/cmake,
# setup-winbuild.sh) and the CMake blocks are a copy away. Leaving them out for
# now keeps this build short enough to read.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
   message(FATAL_ERROR
      "ThreeOhThree is a Linux-only plugin. Nothing in it is Linux-specific beyond "
      "the window, so a cross build is a matter of adding the mingw and VST3 blocks "
      "-- but it has never been built or tested anywhere else, "
      "and a plugin nobody has run is worse than one that refuses to configure.")
endif()

option(THREEOHTHREE_BUILD_TOOLS "Build the offline render/verification host" ON)
option(THREEOHTHREE_BUILD_GUI "Build the plugin window (needs X11 and Cairo)" ON)

# CLAP plugins live in ~/.clap on Linux, so default there rather than /usr/local.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
   set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.clap" CACHE PATH "Install prefix" FORCE)
endif()

# The shared code, beside this plugin at the repository root.
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
message(STATUS "ThreeOhThree: using CLAP headers from ${CLAP_INCLUDE_DIR}")

# ------------------------------------------------------- embedded preset table
set(THREEOHTHREE_PRESET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/presets")
file(GLOB THREEOHTHREE_PRESET_FILES "${THREEOHTHREE_PRESET_DIR}/*.threeohthree")
set(THREEOHTHREE_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/presets_generated.h")

add_custom_command(
   OUTPUT "${THREEOHTHREE_GENERATED_HEADER}"
   COMMAND "${CMAKE_COMMAND}"
      -DPRESET_DIR=${THREEOHTHREE_PRESET_DIR}
      -DOUTPUT=${THREEOHTHREE_GENERATED_HEADER}
      -DNAMESPACE=threeohthree
      -DPRESET_EXT=threeohthree
      -P "${PLUGINCORE_SHARED_DIR}/cmake/embed_presets.cmake"
   DEPENDS ${THREEOHTHREE_PRESET_FILES} "${PLUGINCORE_SHARED_DIR}/cmake/embed_presets.cmake"
   COMMENT "Embedding ${CMAKE_CURRENT_SOURCE_DIR}/presets into presets_generated.h"
   VERBATIM
)
add_custom_target(threeohthree_presets DEPENDS "${THREEOHTHREE_GENERATED_HEADER}")

# ------------------------------------------------------------------- gui deps
#
# X11 and Cairo only. Both are already on any Linux box that can run a DAW, and
# using them directly keeps this a single self-contained .clap file.
set(THREEOHTHREE_GUI_SOURCES "")
if(THREEOHTHREE_BUILD_GUI)
   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND)
      pkg_check_modules(THREEOHTHREE_GUI_DEPS QUIET x11 cairo)
   endif()
   if(THREEOHTHREE_GUI_DEPS_FOUND)
      set(THREEOHTHREE_GUI_SOURCES src/gui/gui.cpp src/gui/seqwindow.cpp)
      message(STATUS "ThreeOhThree: building the plugin window (X11 + Cairo)")
   else()
      set(THREEOHTHREE_BUILD_GUI OFF)
      message(WARNING "ThreeOhThree: x11/cairo not found, building without a GUI")
   endif()
endif()

# ------------------------------------------------------------------ shared code
if(THREEOHTHREE_BUILD_GUI)
   set(PLUGINCORE_WITH_GUI ON)
   set(PLUGINCORE_GUI_INCLUDE_DIRS ${THREEOHTHREE_GUI_DEPS_INCLUDE_DIRS})
   set(PLUGINCORE_GUI_LIBRARY_DIRS ${THREEOHTHREE_GUI_DEPS_LIBRARY_DIRS})
   set(PLUGINCORE_GUI_LIBRARIES ${THREEOHTHREE_GUI_DEPS_LIBRARIES})
   set(PLUGINCORE_GUI_DEFINES ${THREEOHTHREE_GUI_DEFINES})
endif()

add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)

# --------------------------------------------------------- the implementation
#
# The plugin itself is a static library rather than the module directly, which
# is how the suite plugins are wired so that a second format can be added
# without touching anything but its own entry file.
add_library(ThreeOhThree-impl STATIC
   src/plugin.cpp
   src/preset_provider.cpp
   src/preset.cpp
   src/params.cpp
   src/dsp/acid_engine.cpp
   src/pattern.cpp
   ${THREEOHTHREE_GUI_SOURCES}
   "${THREEOHTHREE_GENERATED_HEADER}"
)
add_dependencies(ThreeOhThree-impl threeohthree_presets)

target_link_libraries(ThreeOhThree-impl PUBLIC plugincore::shared)

target_include_directories(ThreeOhThree-impl PUBLIC
   "${CLAP_INCLUDE_DIR}"
   "${CMAKE_CURRENT_SOURCE_DIR}/src"
   "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_options(ThreeOhThree-impl PRIVATE
   -Wall -Wextra
   -fvisibility=hidden
   # Deliberately NOT -ffast-math: on x86 GCC that links crtfastmath.o, which
   # flips FTZ/DAZ for the whole host process. A plugin has no business doing
   # that to its host.
   -fno-math-errno
)

target_link_libraries(ThreeOhThree-impl PUBLIC ${CMAKE_DL_LIBS})

# The version lives in two places that have to agree: project() above and
# kPluginVersion in src/threeohthree.h. Handing the project version to the
# compiler lets that header static_assert they match.
target_compile_definitions(ThreeOhThree-impl PRIVATE
   THREEOHTHREE_CMAKE_VERSION="${PROJECT_VERSION}")

if(THREEOHTHREE_BUILD_GUI)
   target_compile_definitions(ThreeOhThree-impl PRIVATE THREEOHTHREE_WITH_GUI)
   target_include_directories(ThreeOhThree-impl PUBLIC ${THREEOHTHREE_GUI_DEPS_INCLUDE_DIRS})
   target_link_libraries(ThreeOhThree-impl PUBLIC ${THREEOHTHREE_GUI_DEPS_LIBRARIES})
   target_link_directories(ThreeOhThree-impl PUBLIC ${THREEOHTHREE_GUI_DEPS_LIBRARY_DIRS})
   find_package(Threads REQUIRED)
   target_link_libraries(ThreeOhThree-impl PUBLIC Threads::Threads)
endif()

# --------------------------------------------------------------- the CLAP module
add_library(ThreeOhThree MODULE src/entry.cpp)
target_link_libraries(ThreeOhThree PRIVATE ThreeOhThree-impl)
target_compile_options(ThreeOhThree PRIVATE -Wall -Wextra -fvisibility=hidden)

# Only clap_entry is exported; everything else stays internal to the DSO.
target_link_options(ThreeOhThree PRIVATE
   "-Wl,--version-script=${PLUGINCORE_SHARED_DIR}/cmake/clap_entry.version"
   "-Wl,--no-undefined"
)

set_target_properties(ThreeOhThree PROPERTIES
   PREFIX ""
   OUTPUT_NAME "ThreeOhThree"
   SUFFIX ".clap"
)

# ------------------------------------------------------------- offline test host
if(THREEOHTHREE_BUILD_TOOLS)
   # The preset text format is linked in as well as loaded through the plugin,
   # so the self-test can check that writing a preset and reading it back agree.
   add_executable(threeohthree-render tools/render.cpp src/preset.cpp src/params.cpp src/pattern.cpp)
   target_link_libraries(threeohthree-render PRIVATE plugincore::shared)
   target_include_directories(threeohthree-render PRIVATE
      "${CLAP_INCLUDE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
   )
   target_compile_options(threeohthree-render PRIVATE -Wall -Wextra)
   target_link_libraries(threeohthree-render PRIVATE ${CMAKE_DL_LIBS})

   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND)
      pkg_check_modules(GUIHOST_X11 QUIET x11)
   endif()
   if(GUIHOST_X11_FOUND)
      add_executable(threeohthree-guihost tools/guihost.cpp)
      target_include_directories(threeohthree-guihost PRIVATE
         "${CLAP_INCLUDE_DIR}" ${GUIHOST_X11_INCLUDE_DIRS})
      target_compile_options(threeohthree-guihost PRIVATE -Wall -Wextra)
      target_link_libraries(threeohthree-guihost PRIVATE ${CMAKE_DL_LIBS} ${GUIHOST_X11_LIBRARIES})
   endif()
endif()

# ---------------------------------------------------------------------- install
# The plugin looks for a "presets" directory next to its own binary, so both
# land in the same ThreeOhThree/ folder.
install(TARGETS ThreeOhThree LIBRARY DESTINATION "ThreeOhThree")
install(FILES ${THREEOHTHREE_PRESET_FILES} DESTINATION "ThreeOhThree/presets")

message(STATUS "ThreeOhThree: 'cmake --install' will write to ${CMAKE_INSTALL_PREFIX}/ThreeOhThree")
