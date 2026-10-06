# The CLAP plugin (native) and the VST3 (via clap-wrapper), plus the tests that
# need the plugin implementation.
set(CLAP_SDK_ROOT "${AURUM_SDK_DIR}/clap" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------- gui deps
#
# pkg-config on this machine describes this machine, so a cross build must not
# consult it. On Windows, AURUM_WIN_CAIRO points at a Cairo cross-built with
# its win32 backend (../setup-winbuild.sh puts one in ../winbuild/cairo-mingw).
add_library(aurum-gui-deps INTERFACE)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(AURUM_WIN_CAIRO "${CMAKE_CURRENT_SOURCE_DIR}/../winbuild/cairo-mingw"
        CACHE PATH "Cairo cross-built for Windows (setup-winbuild.sh)")
    if (NOT EXISTS "${AURUM_WIN_CAIRO}/lib/libcairo.a")
        # The editor cannot be built without it, and a reverb with no window
        # is not something to ship by accident.
        message(FATAL_ERROR "Aurum: no Windows Cairo at AURUM_WIN_CAIRO=${AURUM_WIN_CAIRO}; run ../setup-winbuild.sh")
    endif()
    # Both levels: the code says <cairo/cairo.h>.
    target_include_directories(aurum-gui-deps INTERFACE
        "${AURUM_WIN_CAIRO}/include" "${AURUM_WIN_CAIRO}/include/cairo")
    target_link_directories(aurum-gui-deps INTERFACE "${AURUM_WIN_CAIRO}/lib")
    # Cairo's win32 backend draws and measures text through GDI; shell32 is
    # drag-and-drop of files, comdlg32/ole32 the file chooser.
    target_link_libraries(aurum-gui-deps INTERFACE
        cairo pixman-1 gdi32 msimg32 user32 shell32 comdlg32 ole32 uuid)
    # Cairo's headers declare everything dllimport unless told the library is
    # static, which is how it is built here.
    target_compile_definitions(aurum-gui-deps INTERFACE CAIRO_WIN32_STATIC_BUILD)
    set(AURUM_WINDOW_SOURCE src/gui/Win32Window.cpp)
    message(STATUS "Aurum: building the plugin window (Win32 + Cairo)")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AURUM_GUI REQUIRED IMPORTED_TARGET cairo x11)
    target_link_libraries(aurum-gui-deps INTERFACE PkgConfig::AURUM_GUI)
    set(AURUM_WINDOW_SOURCE src/gui/X11Window.cpp)
endif()

add_library(aurum-impl STATIC
    src/plugin/Params.cpp
    src/plugin/AurumPlugin.cpp
    src/state/StateIO.cpp
    src/state/Settings.cpp
    src/state/PresetManager.cpp
    src/state/FactoryPresets.cpp
    src/state/FfpImport.cpp
    src/state/AudioFile.cpp
    src/state/IrImport.cpp
    src/plugin/PresetSession.cpp
    src/gui/FileDialog.cpp
    src/gui/PresetBrowser.cpp
    src/gui/Widget.cpp
    src/gui/Widgets.cpp
    ${AURUM_WINDOW_SOURCE}
    src/gui/EqPanel.cpp
    src/gui/Editor.cpp
)
target_include_directories(aurum-impl PUBLIC src)
target_link_libraries(aurum-impl PUBLIC aurum-dsp clap aurum-gui-deps)

# ------------------------------------------------------------- the CLAP module
add_library(Aurum MODULE src/entry.cpp)
target_link_libraries(Aurum PRIVATE aurum-impl)
set_target_properties(Aurum PROPERTIES PREFIX "" OUTPUT_NAME "Aurum" SUFFIX ".clap")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # clap_entry carries dllexport from CLAP_EXPORT; --exclude-all-symbols
    # keeps the rest of the DLL to itself.
    target_link_options(Aurum PRIVATE "-Wl,--exclude-all-symbols" "-Wl,--no-undefined")
    aurum_static_runtime(Aurum)
else()
    target_link_options(Aurum PRIVATE "-Wl,--no-undefined")
endif()

# ------------------------------------------------------------------------ VST3
#
# The same plugin behind the clap-wrapper, which implements a CLAP host against
# the VST3 API. Needs CLAP/clap-wrapper and CLAP/vst3sdk (3.8 or newer), with
# ../shared/patches/ applied to the wrapper. release.sh builds the target
# Aurum-vst3 and picks the bundle up from build/vst3/Aurum.vst3.
if (AURUM_BUILD_VST3)
    if (NOT EXISTS "${AURUM_SDK_DIR}/clap-wrapper/cmake/wrap_vst3.cmake")
        message(FATAL_ERROR "Aurum: no clap-wrapper at ${AURUM_SDK_DIR}/clap-wrapper")
    endif()
    if (NOT EXISTS "${AURUM_SDK_DIR}/vst3sdk/public.sdk/source/main/pluginfactory.cpp")
        message(FATAL_ERROR "Aurum: no VST3 SDK at ${AURUM_SDK_DIR}/vst3sdk")
    endif()
    set(VST3_SDK_ROOT "${AURUM_SDK_DIR}/vst3sdk" CACHE PATH "" FORCE)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_OUTPUT_NAME "Aurum" CACHE STRING "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS FALSE CACHE BOOL "" FORCE)
    add_subdirectory("${AURUM_SDK_DIR}/clap-wrapper" clap-wrapper EXCLUDE_FROM_ALL)

    add_library(Aurum-vst3 MODULE src/entry.cpp)
    target_link_libraries(Aurum-vst3 PRIVATE aurum-impl)
    target_add_vst3_wrapper(
        TARGET Aurum-vst3
        OUTPUT_NAME "Aurum"
        BUNDLE_IDENTIFIER "de.ravetracer.aurum.vst3"
        BUNDLE_VERSION "${PROJECT_VERSION}"
        ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vst3"
    )
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # The wrapper leaves CMake's "lib" prefix on a mingw MODULE, and a
        # VST3 bundle's binary must be named exactly like the bundle.
        set_target_properties(Aurum-vst3 PROPERTIES PREFIX "")
        # GetPluginFactory carries dllexport from SMTG_EXPORT_SYMBOL, so no
        # --exclude-all-symbols here.
        target_link_options(Aurum-vst3 PRIVATE "-Wl,--no-undefined")
        aurum_static_runtime(Aurum-vst3)
    endif()
endif()

# ---------------------------------------------------------------------- install
# Factory presets are compiled in and written to the user's preset folder on
# first run, so the .clap is all there is to install.
install(TARGETS Aurum LIBRARY DESTINATION "Aurum")

# ----------------------------------------------------------------------- tests
if (AURUM_BUILD_TOOLS)
    add_executable(gui_snapshot tests/gui_snapshot.cpp)
    target_link_libraries(gui_snapshot PRIVATE aurum-impl)
    aurum_static_runtime(gui_snapshot)

    add_executable(import_test tests/import_test.cpp tests/WavFile.cpp)
    target_include_directories(import_test PRIVATE tests)
    target_link_libraries(import_test PRIVATE aurum-impl)
    aurum_static_runtime(import_test)

    add_executable(longrun_test tests/longrun_test.cpp)
    target_link_libraries(longrun_test PRIVATE aurum-impl)
    aurum_static_runtime(longrun_test)

    # A minimal CLAP host that opens the plugin's window: X11 on Linux, a real
    # win32 window on Windows (run under wine, it is how the win32 backend gets
    # exercised from here at all).
    add_executable(clap_gui_host tests/clap_gui_host.cpp)
    target_link_libraries(clap_gui_host PRIVATE clap Threads::Threads)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(clap_gui_host PRIVATE user32 gdi32)
        aurum_static_runtime(clap_gui_host)
    else()
        target_link_libraries(clap_gui_host PRIVATE PkgConfig::AURUM_GUI ${CMAKE_DL_LIBS})
    endif()
endif()
