# The plugin: the CLAP module and, through the clap-wrapper, the VST3 bundle,
# both from one implementation library. Included from CMakeLists.txt.

set(CLAP_SDK_ROOT "${SIDMIDI_SDK_DIR}/clap" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------- gui deps
#
# pkg-config on this machine describes this machine, so a cross build must not
# consult it. On Windows, SIDMIDI_WIN_CAIRO points at a Cairo cross-built with
# its win32 backend (../setup-winbuild.sh puts one in ../winbuild/cairo-mingw).
add_library(sidmidi-gui-deps INTERFACE)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SIDMIDI_WIN_CAIRO "${CMAKE_CURRENT_SOURCE_DIR}/../winbuild/cairo-mingw"
        CACHE PATH "Cairo cross-built for Windows (setup-winbuild.sh)")
    if (NOT EXISTS "${SIDMIDI_WIN_CAIRO}/lib/libcairo.a")
        message(FATAL_ERROR "SidMidi: no Windows Cairo at SIDMIDI_WIN_CAIRO=${SIDMIDI_WIN_CAIRO}; run ../setup-winbuild.sh")
    endif()
    target_include_directories(sidmidi-gui-deps INTERFACE
        "${SIDMIDI_WIN_CAIRO}/include" "${SIDMIDI_WIN_CAIRO}/include/cairo")
    target_link_directories(sidmidi-gui-deps INTERFACE "${SIDMIDI_WIN_CAIRO}/lib")
    target_link_libraries(sidmidi-gui-deps INTERFACE
        cairo pixman-1 gdi32 msimg32 user32 shell32 comdlg32 ole32 uuid)
    target_compile_definitions(sidmidi-gui-deps INTERFACE CAIRO_WIN32_STATIC_BUILD)
    set(SIDMIDI_WINDOW_SOURCE src/gui/Win32Window.cpp)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SIDMIDI_GUI REQUIRED IMPORTED_TARGET cairo x11)
    target_link_libraries(sidmidi-gui-deps INTERFACE PkgConfig::SIDMIDI_GUI)
    set(SIDMIDI_WINDOW_SOURCE src/gui/X11Window.cpp)
endif()

add_library(sidmidi-impl STATIC
    src/emu/Snapshot.cpp
    src/plugin/MachineSettings.cpp
    src/plugin/SidMidiPlugin.cpp
    src/gui/Widget.cpp
    src/gui/Widgets.cpp
    src/gui/Controls.cpp
    src/gui/FileDialog.cpp
    src/gui/Keymap.cpp
    src/gui/Editor.cpp
    ${SIDMIDI_WINDOW_SOURCE}
)
target_include_directories(sidmidi-impl PUBLIC src)
# Lets src/sidmidi.h static_assert that kPluginVersion matches project().
target_compile_definitions(sidmidi-impl PUBLIC SIDMIDI_CMAKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(sidmidi-impl PUBLIC sidmidi-emu clap sidmidi-gui-deps Threads::Threads)

# ------------------------------------------------------------- the CLAP module
add_library(SidMidi MODULE src/entry.cpp)
target_link_libraries(SidMidi PRIVATE sidmidi-impl)
set_target_properties(SidMidi PROPERTIES PREFIX "" OUTPUT_NAME "SidMidi" SUFFIX ".clap")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_options(SidMidi PRIVATE "-Wl,--exclude-all-symbols" "-Wl,--no-undefined")
    sidmidi_static_runtime(SidMidi)
else()
    target_link_options(SidMidi PRIVATE "-Wl,--no-undefined")
endif()

# ------------------------------------------------------------------------ VST3
#
# The same plugin behind the clap-wrapper. Needs CLAP/clap-wrapper and
# CLAP/vst3sdk (3.8 or newer), with ../shared/patches/ applied to the wrapper.
# release.sh builds the target SidMidi-vst3 and picks the bundle up from
# build/vst3/SidMidi.vst3.
if (SIDMIDI_BUILD_VST3)
    if (NOT EXISTS "${SIDMIDI_SDK_DIR}/clap-wrapper/cmake/wrap_vst3.cmake")
        message(FATAL_ERROR "SidMidi: no clap-wrapper at ${SIDMIDI_SDK_DIR}/clap-wrapper")
    endif()
    if (NOT EXISTS "${SIDMIDI_SDK_DIR}/vst3sdk/public.sdk/source/main/pluginfactory.cpp")
        message(FATAL_ERROR "SidMidi: no VST3 SDK at ${SIDMIDI_SDK_DIR}/vst3sdk")
    endif()
    set(VST3_SDK_ROOT "${SIDMIDI_SDK_DIR}/vst3sdk" CACHE PATH "" FORCE)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_OUTPUT_NAME "SidMidi" CACHE STRING "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS FALSE CACHE BOOL "" FORCE)
    add_subdirectory("${SIDMIDI_SDK_DIR}/clap-wrapper" clap-wrapper EXCLUDE_FROM_ALL)

    add_library(SidMidi-vst3 MODULE src/entry.cpp)
    target_link_libraries(SidMidi-vst3 PRIVATE sidmidi-impl)
    target_add_vst3_wrapper(
        TARGET SidMidi-vst3
        OUTPUT_NAME "SidMidi"
        BUNDLE_IDENTIFIER "de.ravetracer.sidmidi.vst3"
        BUNDLE_VERSION "${PROJECT_VERSION}"
        ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vst3"
    )
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(SidMidi-vst3 PROPERTIES PREFIX "")
        target_link_options(SidMidi-vst3 PRIVATE "-Wl,--no-undefined")
        sidmidi_static_runtime(SidMidi-vst3)
    endif()
    # Part of the default build: `ninja -C build` makes the .clap and the .vst3.
    add_custom_target(sidmidi-vst3-all ALL DEPENDS SidMidi-vst3)
endif()

# ---------------------------------------------------------------------- install
install(TARGETS SidMidi LIBRARY DESTINATION "SidMidi")
