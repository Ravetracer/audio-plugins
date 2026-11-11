# The plugin: the CLAP module and, through the clap-wrapper, the VST3 bundle,
# both from one implementation library. Included from CMakeLists.txt.

set(CLAP_SDK_ROOT "${RESIDENT_SDK_DIR}/clap" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------- gui deps
#
# pkg-config on this machine describes this machine, so a cross build must not
# consult it. On Windows, RESIDENT_WIN_CAIRO points at a Cairo cross-built with
# its win32 backend (../setup-winbuild.sh puts one in ../winbuild/cairo-mingw).
add_library(resident-gui-deps INTERFACE)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(RESIDENT_WIN_CAIRO "${CMAKE_CURRENT_SOURCE_DIR}/../winbuild/cairo-mingw"
        CACHE PATH "Cairo cross-built for Windows (setup-winbuild.sh)")
    if (NOT EXISTS "${RESIDENT_WIN_CAIRO}/lib/libcairo.a")
        message(FATAL_ERROR "ReSIDent: no Windows Cairo at RESIDENT_WIN_CAIRO=${RESIDENT_WIN_CAIRO}; run ../setup-winbuild.sh")
    endif()
    target_include_directories(resident-gui-deps INTERFACE
        "${RESIDENT_WIN_CAIRO}/include" "${RESIDENT_WIN_CAIRO}/include/cairo")
    target_link_directories(resident-gui-deps INTERFACE "${RESIDENT_WIN_CAIRO}/lib")
    target_link_libraries(resident-gui-deps INTERFACE
        cairo pixman-1 gdi32 msimg32 user32 shell32 comdlg32 ole32 uuid)
    target_compile_definitions(resident-gui-deps INTERFACE CAIRO_WIN32_STATIC_BUILD)
    set(RESIDENT_WINDOW_SOURCE src/gui/Win32Window.cpp)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RESIDENT_GUI REQUIRED IMPORTED_TARGET cairo x11)
    target_link_libraries(resident-gui-deps INTERFACE PkgConfig::RESIDENT_GUI)
    set(RESIDENT_WINDOW_SOURCE src/gui/X11Window.cpp)
endif()

add_library(resident-impl STATIC
    src/emu/Snapshot.cpp
    src/plugin/MachineSettings.cpp
    src/plugin/CcMap.cpp
    src/plugin/Params.cpp
    src/plugin/ResidentPlugin.cpp
    src/gui/Widget.cpp
    src/gui/Widgets.cpp
    src/gui/Controls.cpp
    src/gui/FileDialog.cpp
    src/gui/Keymap.cpp
    src/gui/Editor.cpp
    ${RESIDENT_WINDOW_SOURCE}
)
target_include_directories(resident-impl PUBLIC src)
# Lets src/resident.h static_assert that kPluginVersion matches project().
target_compile_definitions(resident-impl PUBLIC RESIDENT_CMAKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(resident-impl PUBLIC resident-emu clap resident-gui-deps Threads::Threads)

# ------------------------------------------------------------- the CLAP module
add_library(ReSIDent MODULE src/entry.cpp)
target_link_libraries(ReSIDent PRIVATE resident-impl)
set_target_properties(ReSIDent PROPERTIES PREFIX "" OUTPUT_NAME "ReSIDent" SUFFIX ".clap")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_options(ReSIDent PRIVATE "-Wl,--exclude-all-symbols" "-Wl,--no-undefined")
    resident_static_runtime(ReSIDent)
else()
    target_link_options(ReSIDent PRIVATE "-Wl,--no-undefined")
endif()

# ------------------------------------------------------------------------ VST3
#
# The same plugin behind the clap-wrapper. Needs CLAP/clap-wrapper and
# CLAP/vst3sdk (3.8 or newer), with ../shared/patches/ applied to the wrapper.
# release.sh builds the target ReSIDent-vst3 and picks the bundle up from
# build/vst3/ReSIDent.vst3.
if (RESIDENT_BUILD_VST3)
    if (NOT EXISTS "${RESIDENT_SDK_DIR}/clap-wrapper/cmake/wrap_vst3.cmake")
        message(FATAL_ERROR "ReSIDent: no clap-wrapper at ${RESIDENT_SDK_DIR}/clap-wrapper")
    endif()
    if (NOT EXISTS "${RESIDENT_SDK_DIR}/vst3sdk/public.sdk/source/main/pluginfactory.cpp")
        message(FATAL_ERROR "ReSIDent: no VST3 SDK at ${RESIDENT_SDK_DIR}/vst3sdk")
    endif()
    set(VST3_SDK_ROOT "${RESIDENT_SDK_DIR}/vst3sdk" CACHE PATH "" FORCE)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_OUTPUT_NAME "ReSIDent" CACHE STRING "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS FALSE CACHE BOOL "" FORCE)
    add_subdirectory("${RESIDENT_SDK_DIR}/clap-wrapper" clap-wrapper EXCLUDE_FROM_ALL)

    add_library(ReSIDent-vst3 MODULE src/entry.cpp)
    target_link_libraries(ReSIDent-vst3 PRIVATE resident-impl)
    target_add_vst3_wrapper(
        TARGET ReSIDent-vst3
        OUTPUT_NAME "ReSIDent"
        BUNDLE_IDENTIFIER "de.ravetracer.sidmidi.vst3"
        BUNDLE_VERSION "${PROJECT_VERSION}"
        ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vst3"
    )
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(ReSIDent-vst3 PROPERTIES PREFIX "")
        target_link_options(ReSIDent-vst3 PRIVATE "-Wl,--no-undefined")
        resident_static_runtime(ReSIDent-vst3)
    endif()
    # Part of the default build: `ninja -C build` makes the .clap and the .vst3.
    add_custom_target(resident-vst3-all ALL DEPENDS ReSIDent-vst3)
endif()

# ---------------------------------------------------------------------- install
install(TARGETS ReSIDent LIBRARY DESTINATION "ReSIDent")
