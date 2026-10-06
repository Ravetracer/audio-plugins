# CLAP plugin (native) and VST3 (via clap-wrapper).
set(CLAP_SDK_ROOT "${AURUM_SDK_DIR}/clap" CACHE PATH "" FORCE)
set(VST3_SDK_ROOT "${AURUM_SDK_DIR}/vst3sdk" CACHE PATH "" FORCE)
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)
add_subdirectory("${AURUM_SDK_DIR}/clap-wrapper" clap-wrapper EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AURUM_GUI REQUIRED IMPORTED_TARGET cairo x11)

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
    src/gui/X11Window.cpp
    src/gui/EqPanel.cpp
    src/gui/Editor.cpp
)
target_include_directories(aurum-impl PUBLIC src)
target_link_libraries(aurum-impl PUBLIC aurum-dsp clap PkgConfig::AURUM_GUI)

make_clapfirst_plugins(
    TARGET_NAME aurum
    IMPL_TARGET aurum-impl
    OUTPUT_NAME "Aurum"
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/entry.cpp"
    BUNDLE_IDENTIFIER "de.ravetracer.aurum"
    BUNDLE_VERSION ${PROJECT_VERSION}
    COPY_AFTER_BUILD FALSE
    PLUGIN_FORMATS CLAP VST3
    ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

if (AURUM_BUILD_TESTS)
    add_executable(gui_snapshot tests/gui_snapshot.cpp)
    target_link_libraries(gui_snapshot PRIVATE aurum-impl)
endif()
if (AURUM_BUILD_TESTS)
    add_executable(clap_gui_host tests/clap_gui_host.cpp)
    target_link_libraries(clap_gui_host PRIVATE clap PkgConfig::AURUM_GUI ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if (AURUM_BUILD_TESTS)
    add_executable(import_test tests/import_test.cpp tests/WavFile.cpp)
    target_include_directories(import_test PRIVATE tests)
    target_link_libraries(import_test PRIVATE aurum-impl)
endif()
if (AURUM_BUILD_TESTS)
    add_executable(longrun_test tests/longrun_test.cpp)
    target_link_libraries(longrun_test PRIVATE aurum-impl)
endif()
