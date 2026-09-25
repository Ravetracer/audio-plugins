cmake_minimum_required(VERSION 3.16)
project(RumpelKiste VERSION 0.2.0 LANGUAGES CXX)

# RumpelKiste builds as a CLAP and as a VST3, for Linux and for Windows.
#
# It is built on the PluginCore shared library -- the parameter model, the
# preset format, the window -- and expects it at ../shared. See README.md for
# the directory layout this needs.
#
# It builds for Linux and for Windows (cross-compiled with mingw-w64), as a CLAP
# and, through the clap-wrapper, as a VST3. See README.md for what each of those
# needs checked out.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
   message(FATAL_ERROR
      "RumpelKiste builds for Linux and for Windows. Nothing in it is specific to "
      "either beyond the window, which has a backend for both, so another platform "
      "is a matter of a third backend -- but it has never been built or run there, "
      "and a plugin nobody has run is worse than one that refuses to configure.")
endif()

option(RUMPELKISTE_BUILD_TOOLS "Build the offline render/verification host" ON)
option(RUMPELKISTE_BUILD_GUI "Build the plugin window (X11+Cairo, or win32+Cairo)" ON)
option(RUMPELKISTE_BUILD_VST3 "Also build a VST3 through the clap-wrapper" OFF)

# CLAP plugins live in ~/.clap on Linux, so default there rather than /usr/local.
# A cross build has no business guessing a path on the target machine, so it is
# left to whoever runs cmake --install (release.sh sets it to a staging tree).
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT CMAKE_CROSSCOMPILING)
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
message(STATUS "RumpelKiste: using CLAP headers from ${CLAP_INCLUDE_DIR}")

# ------------------------------------------------------- embedded preset table
set(RUMPELKISTE_PRESET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/presets")
file(GLOB RUMPELKISTE_PRESET_FILES "${RUMPELKISTE_PRESET_DIR}/*.rumpelkiste")
set(RUMPELKISTE_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/presets_generated.h")

add_custom_command(
   OUTPUT "${RUMPELKISTE_GENERATED_HEADER}"
   COMMAND "${CMAKE_COMMAND}"
      -DPRESET_DIR=${RUMPELKISTE_PRESET_DIR}
      -DOUTPUT=${RUMPELKISTE_GENERATED_HEADER}
      -DNAMESPACE=rumpelkiste
      -DPRESET_EXT=rumpelkiste
      -P "${PLUGINCORE_SHARED_DIR}/cmake/embed_presets.cmake"
   DEPENDS ${RUMPELKISTE_PRESET_FILES} "${PLUGINCORE_SHARED_DIR}/cmake/embed_presets.cmake"
   COMMENT "Embedding ${CMAKE_CURRENT_SOURCE_DIR}/presets into presets_generated.h"
   VERBATIM
)
add_custom_target(rumpelkiste_presets DEPENDS "${RUMPELKISTE_GENERATED_HEADER}")

# ------------------------------------------------------------------- gui deps
#
# X11 and Cairo only. Both are already on any Linux box that can run a DAW, and
# using them directly keeps this a single self-contained .clap file.
#
# pkg-config on this machine describes this machine, so a cross build must not
# consult it: it would report the host's X11 and Cairo and drag /usr/include
# into a Windows compile. Point RUMPELKISTE_WIN_CAIRO at a Cairo cross-built
# with its win32 backend instead (../shared/cmake/build-windows-cairo.sh, or run
# ../setup-winbuild.sh once).
set(RUMPELKISTE_GUI_SOURCES "")
if(RUMPELKISTE_BUILD_GUI AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
   if(NOT RUMPELKISTE_WIN_CAIRO OR NOT EXISTS "${RUMPELKISTE_WIN_CAIRO}/lib/libcairo.a")
      # A warning and no window, not a hard failure: release.sh has a
      # --windows-no-gui path that depends on being able to get this far.
      set(RUMPELKISTE_BUILD_GUI OFF)
      message(WARNING
         "RumpelKiste: no Windows Cairo at RUMPELKISTE_WIN_CAIRO, building without a window")
   else()
      # Both levels: the code says <cairo/cairo.h>, which needs the prefix on
      # the path, and <cairo-win32.h>, which needs the cairo directory itself.
      set(RUMPELKISTE_GUI_DEPS_INCLUDE_DIRS
         "${RUMPELKISTE_WIN_CAIRO}/include" "${RUMPELKISTE_WIN_CAIRO}/include/cairo")
      set(RUMPELKISTE_GUI_DEPS_LIBRARY_DIRS "${RUMPELKISTE_WIN_CAIRO}/lib")
      # Cairo's win32 backend draws through GDI and measures text with it.
      # ole32/uuid/shell32: dragging a pattern out as a .mid file is an OLE
      # drag, and DoDragDrop, the CF_HDROP format and the interface GUIDs each
      # come from one of them.
      # comdlg32: the file chooser a preset pack is exported and imported
      # through, which is the operating system's own on Windows.
      set(RUMPELKISTE_GUI_DEPS_LIBRARIES cairo pixman-1 gdi32 msimg32 user32 ole32 uuid shell32
          comdlg32)
      set(RUMPELKISTE_GUI_SOURCES src/gui/gui.cpp src/gui/seqwindow.cpp src/gui/dragfile.cpp
          src/gui/filedialog.cpp)
      # Cairo's headers declare everything dllimport unless told the library is
      # static, which is how it is built here.
      set(RUMPELKISTE_GUI_DEFINES CAIRO_WIN32_STATIC_BUILD)
      set(RUMPELKISTE_GUI_FOUND_WIN TRUE)
      message(STATUS "RumpelKiste: building the plugin window (Win32 + Cairo)")
   endif()
endif()

if(RUMPELKISTE_BUILD_GUI AND NOT RUMPELKISTE_GUI_FOUND_WIN)
   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND)
      pkg_check_modules(RUMPELKISTE_GUI_DEPS QUIET x11 cairo)
   endif()
   if(RUMPELKISTE_GUI_DEPS_FOUND)
      set(RUMPELKISTE_GUI_SOURCES src/gui/gui.cpp src/gui/seqwindow.cpp src/gui/dragfile.cpp
          src/gui/filedialog.cpp)
      message(STATUS "RumpelKiste: building the plugin window (X11 + Cairo)")
   else()
      set(RUMPELKISTE_BUILD_GUI OFF)
      message(WARNING "RumpelKiste: x11/cairo not found, building without a GUI")
   endif()
endif()

# ------------------------------------------------------------------ shared code
if(RUMPELKISTE_BUILD_GUI)
   set(PLUGINCORE_WITH_GUI ON)
   set(PLUGINCORE_GUI_INCLUDE_DIRS ${RUMPELKISTE_GUI_DEPS_INCLUDE_DIRS})
   set(PLUGINCORE_GUI_LIBRARY_DIRS ${RUMPELKISTE_GUI_DEPS_LIBRARY_DIRS})
   set(PLUGINCORE_GUI_LIBRARIES ${RUMPELKISTE_GUI_DEPS_LIBRARIES})
   set(PLUGINCORE_GUI_DEFINES ${RUMPELKISTE_GUI_DEFINES})
endif()

add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)

# --------------------------------------------------------- the implementation
#
# The plugin itself is a static library rather than the module directly, which
# is how the suite plugins are wired so that a second format can be added
# without touching anything but its own entry file.
add_library(RumpelKiste-impl STATIC
   src/plugin.cpp
   src/preset_provider.cpp
   src/preset.cpp
   src/params.cpp
   src/dsp/drums.cpp
   src/dsp/drive.cpp
   src/pattern.cpp
   src/midifile.cpp
   ${RUMPELKISTE_GUI_SOURCES}
   "${RUMPELKISTE_GENERATED_HEADER}"
)
add_dependencies(RumpelKiste-impl rumpelkiste_presets)

target_link_libraries(RumpelKiste-impl PUBLIC plugincore::shared)

target_include_directories(RumpelKiste-impl PUBLIC
   "${CLAP_INCLUDE_DIR}"
   "${CMAKE_CURRENT_SOURCE_DIR}/src"
   "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_options(RumpelKiste-impl PRIVATE
   -Wall -Wextra
   -fvisibility=hidden
   # Deliberately NOT -ffast-math: on x86 GCC that links crtfastmath.o, which
   # flips FTZ/DAZ for the whole host process. A plugin has no business doing
   # that to its host.
   -fno-math-errno
)

target_link_libraries(RumpelKiste-impl PUBLIC ${CMAKE_DL_LIBS})

# The version lives in two places that have to agree: project() above and
# kPluginVersion in src/rumpelkiste.h. Handing the project version to the
# compiler lets that header static_assert they match.
target_compile_definitions(RumpelKiste-impl PRIVATE
   RUMPELKISTE_CMAKE_VERSION="${PROJECT_VERSION}")

if(RUMPELKISTE_BUILD_GUI)
   target_compile_definitions(RumpelKiste-impl PRIVATE
      RUMPELKISTE_WITH_GUI ${RUMPELKISTE_GUI_DEFINES})
   target_include_directories(RumpelKiste-impl PUBLIC ${RUMPELKISTE_GUI_DEPS_INCLUDE_DIRS})
   target_link_libraries(RumpelKiste-impl PUBLIC ${RUMPELKISTE_GUI_DEPS_LIBRARIES})
   target_link_directories(RumpelKiste-impl PUBLIC ${RUMPELKISTE_GUI_DEPS_LIBRARY_DIRS})
   find_package(Threads REQUIRED)
   target_link_libraries(RumpelKiste-impl PUBLIC Threads::Threads)
endif()

# --------------------------------------------------------------- the CLAP module
add_library(RumpelKiste MODULE src/entry.cpp)
target_link_libraries(RumpelKiste PRIVATE RumpelKiste-impl)
target_compile_options(RumpelKiste PRIVATE -Wall -Wextra -fvisibility=hidden)

if(WIN32)
   # A version script is an ELF idea. On Windows clap_entry carries dllexport
   # from CLAP_EXPORT, and --exclude-all-symbols keeps the rest of the DLL to
   # itself. The runtime is linked in so the .clap needs no MinGW DLLs beside it
   # on a machine that has never seen a compiler.
   target_link_options(RumpelKiste PRIVATE
      "-Wl,--exclude-all-symbols"
      "-Wl,--no-undefined"
      "-static-libgcc" "-static-libstdc++"
      "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
   )
else()
   # Only clap_entry is exported; everything else stays internal to the DSO.
   target_link_options(RumpelKiste PRIVATE
      "-Wl,--version-script=${PLUGINCORE_SHARED_DIR}/cmake/clap_entry.version"
      "-Wl,--no-undefined"
   )
endif()

set_target_properties(RumpelKiste PROPERTIES
   PREFIX ""
   OUTPUT_NAME "RumpelKiste"
   SUFFIX ".clap"
)

# ------------------------------------------------------------- offline test host
if(RUMPELKISTE_BUILD_TOOLS)
   # The preset text format is linked in as well as loaded through the plugin,
   # so the self-test can check that writing a preset and reading it back agree.
   # dsp/drums.cpp is in here as well as in the plugin: the self-test measures
   # the voices directly -- where a kick settles, how long a tom rings, what a
   # rim shot's spectrum peaks at -- because those are the claims the parameter
   # tips make, and none of them is visible from outside the plugin without an
   # analysis nobody would trust more than the engine itself.
   add_executable(rumpelkiste-render tools/render.cpp src/preset.cpp src/params.cpp src/pattern.cpp
                  src/midifile.cpp src/dsp/drums.cpp src/dsp/drive.cpp)
   target_link_libraries(rumpelkiste-render PRIVATE plugincore::shared)
   target_include_directories(rumpelkiste-render PRIVATE
      "${CLAP_INCLUDE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
   )
   target_compile_options(rumpelkiste-render PRIVATE -Wall -Wextra)
   target_link_libraries(rumpelkiste-render PRIVATE ${CMAKE_DL_LIBS})

   # Cross-built, the renderer has to stand on its own too. Without this it
   # imports libstdc++-6.dll and libgcc_s_seh-1.dll, which are not in a wine
   # prefix or on a clean Windows box, and the process dies before main() with
   # exit code 53 -- ERROR_BAD_NETPATH, Windows' unhelpful way of saying a
   # dependent DLL was not found. That is worth avoiding, because a renderer
   # that runs under wine is how the Windows plugin gets the self-test run
   # against it at all.
   if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      target_link_options(rumpelkiste-render PRIVATE
         "-static-libgcc" "-static-libstdc++"
         "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
      )
   endif()

   # The standalone GUI host is X11 only, and on a cross build pkg-config would
   # answer for this machine rather than the target. The renderer is portable --
   # it is a CLAP host and nothing more -- so a Windows cross build still gets
   # that one, which is how the Windows plugin gets its self-test run under wine.
   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
      pkg_check_modules(GUIHOST_X11 QUIET x11)
   endif()
   if(GUIHOST_X11_FOUND OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
      # guihost.cpp carries both backends: X11 on Linux, a real win32 window on
      # Windows. Cross-built it runs under wine, which is the only way the
      # win32 half of the plugin window gets exercised from here at all.
      add_executable(rumpelkiste-guihost tools/guihost.cpp)
      target_include_directories(rumpelkiste-guihost PRIVATE
         "${CLAP_INCLUDE_DIR}" ${GUIHOST_X11_INCLUDE_DIRS})
      target_compile_options(rumpelkiste-guihost PRIVATE -Wall -Wextra)
      target_link_libraries(rumpelkiste-guihost PRIVATE ${CMAKE_DL_LIBS} ${GUIHOST_X11_LIBRARIES})
      if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
         target_link_options(rumpelkiste-guihost PRIVATE
            "-static-libgcc" "-static-libstdc++"
            "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
         )
      endif()
   endif()
endif()

# ------------------------------------------------------------------------ VST3
#
# The VST3 is the same plugin, not a port: the clap-wrapper implements a CLAP
# host against the VST3 API and loads RumpelKiste-impl through it, so there is
# one implementation and one set of behaviour in both formats.
#
# Needs CLAP/clap-wrapper and CLAP/vst3sdk (VST 3.8 or newer, which is the first
# MIT-licensed one), and the wrapper needs shared/patches/ applied to build
# against 3.8 at all. The repository README has the commands.
if(RUMPELKISTE_BUILD_VST3)
   get_filename_component(RUMPELKISTE_CLAP_DIR "${CLAP_INCLUDE_DIR}/../.." ABSOLUTE)
   set(RUMPELKISTE_WRAPPER_DIR "${RUMPELKISTE_CLAP_DIR}/clap-wrapper"
      CACHE PATH "Directory containing a clap-wrapper checkout")
   set(RUMPELKISTE_VST3_SDK_DIR "${RUMPELKISTE_CLAP_DIR}/vst3sdk"
      CACHE PATH "Directory containing a VST3 SDK checkout")

   if(NOT EXISTS "${RUMPELKISTE_WRAPPER_DIR}/cmake/wrap_vst3.cmake")
      message(FATAL_ERROR "RumpelKiste: no clap-wrapper at ${RUMPELKISTE_WRAPPER_DIR}")
   endif()
   if(NOT EXISTS "${RUMPELKISTE_VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp")
      message(FATAL_ERROR "RumpelKiste: no VST3 SDK at ${RUMPELKISTE_VST3_SDK_DIR}")
   endif()

   # The wrapper resolves both SDKs itself; hand it the checkouts beside this
   # plugin rather than letting it download its own.
   set(CLAP_SDK_ROOT "${RUMPELKISTE_CLAP_DIR}/clap" CACHE PATH "" FORCE)
   set(VST3_SDK_ROOT "${RUMPELKISTE_VST3_SDK_DIR}" CACHE PATH "" FORCE)
   set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE CACHE BOOL "" FORCE)
   set(CLAP_WRAPPER_OUTPUT_NAME "RumpelKiste" CACHE STRING "" FORCE)
   set(CLAP_WRAPPER_BUILD_TESTS FALSE CACHE BOOL "" FORCE)

   add_subdirectory("${RUMPELKISTE_WRAPPER_DIR}" clap-wrapper EXCLUDE_FROM_ALL)

   # The bundle is assembled in place, so point it somewhere predictable rather
   # than at whatever the generator picks per configuration. release.sh looks
   # for build/vst3/RumpelKiste.vst3 and for the target by name.
   set(RUMPELKISTE_VST3_ASSET_DIR "${CMAKE_BINARY_DIR}/vst3")

   add_library(RumpelKiste-vst3 MODULE src/entry.cpp)
   target_link_libraries(RumpelKiste-vst3 PRIVATE RumpelKiste-impl)
   target_compile_options(RumpelKiste-vst3 PRIVATE -Wall -Wextra)

   target_add_vst3_wrapper(
      TARGET RumpelKiste-vst3
      OUTPUT_NAME "RumpelKiste"
      BUNDLE_IDENTIFIER "de.ravetracer.rumpelkiste.vst3"
      BUNDLE_VERSION "${PROJECT_VERSION}"
      ASSET_OUTPUT_DIRECTORY "${RUMPELKISTE_VST3_ASSET_DIR}"
   )

   # Two things the wrapper leaves to the plugin on a mingw cross build.
   if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      # CMake gives a MODULE library a "lib" prefix with the GNU toolchain, and
      # the wrapper only clears it on the single-file path. A VST3 bundle's
      # binary has to be named exactly like the bundle.
      set_target_properties(RumpelKiste-vst3 PROPERTIES PREFIX "")
      # Same reasoning as the .clap: the runtime is linked in so the plugin
      # needs no MinGW DLLs beside it. GetPluginFactory carries dllexport from
      # SMTG_EXPORT_SYMBOL, so unlike the .clap there is no version script and
      # no --exclude-all-symbols here.
      target_link_options(RumpelKiste-vst3 PRIVATE
         "-Wl,--no-undefined"
         "-static-libgcc" "-static-libstdc++"
         "-Wl,-Bstatic,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive,-Bdynamic"
      )
   endif()
endif()

# ---------------------------------------------------------------------- install
# The plugin looks for a "presets" directory next to its own binary, so both
# land in the same RumpelKiste/ folder.
install(TARGETS RumpelKiste LIBRARY DESTINATION "RumpelKiste")
install(FILES ${RUMPELKISTE_PRESET_FILES} DESTINATION "RumpelKiste/presets")

message(STATUS "RumpelKiste: 'cmake --install' will write to ${CMAKE_INSTALL_PREFIX}/RumpelKiste")
