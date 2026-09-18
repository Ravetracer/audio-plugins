# The code every plugin in this repository is built on.
#
# Consumed by a plugin with
#
#    add_subdirectory("${PLUGINCORE_SHARED_DIR}" plugincore-shared)
#    target_link_libraries(<target> PRIVATE plugincore::shared)
#
# which also brings in the include directory, so shared headers are reached as
# <plugincore/params.h>, <plugincore/dsp/filters.h> and so on.
#
# A static library rather than an interface one: this is real code, and building
# it once per plugin is cheaper than compiling it into every target that needs
# it. Being static also means its file-scope state belongs to one plugin binary,
# which is what makes the shared preset provider safe.
#
# Before including this, a plugin sets:
#
#   CLAP_INCLUDE_DIR            required, the CLAP headers
#   PLUGINCORE_WITH_GUI           ON to build the plugin window
#   PLUGINCORE_GUI_INCLUDE_DIRS   X11/Cairo, or a cross-built Cairo
#   PLUGINCORE_GUI_LIBRARY_DIRS
#   PLUGINCORE_GUI_LIBRARIES
#   PLUGINCORE_GUI_DEFINES
#
# The GUI dependencies are discovered by the plugin, not here: a cross build
# must not consult this machine's pkg-config.

cmake_minimum_required(VERSION 3.16)

set(PLUGINCORE_SOURCES
   src/params.cpp
   src/preset.cpp
   src/preset_provider.cpp
)

if(PLUGINCORE_WITH_GUI)
   list(APPEND PLUGINCORE_SOURCES src/gui/window.cpp)
endif()

add_library(plugincore_shared STATIC ${PLUGINCORE_SOURCES})
add_library(plugincore::shared ALIAS plugincore_shared)

target_include_directories(plugincore_shared PUBLIC
   "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# The preset-discovery provider is written against the CLAP API. The plugin that
# pulls this in has already located the headers.
if(NOT CLAP_INCLUDE_DIR)
   message(FATAL_ERROR "plugincore_shared: CLAP_INCLUDE_DIR must be set before add_subdirectory()")
endif()
target_include_directories(plugincore_shared PUBLIC "${CLAP_INCLUDE_DIR}")

if(PLUGINCORE_WITH_GUI)
   target_compile_definitions(plugincore_shared PUBLIC PLUGINCORE_WITH_GUI ${PLUGINCORE_GUI_DEFINES})
   target_include_directories(plugincore_shared PUBLIC ${PLUGINCORE_GUI_INCLUDE_DIRS})
   target_link_directories(plugincore_shared PUBLIC ${PLUGINCORE_GUI_LIBRARY_DIRS})
   target_link_libraries(plugincore_shared PUBLIC ${PLUGINCORE_GUI_LIBRARIES})
   find_package(Threads REQUIRED)
   target_link_libraries(plugincore_shared PUBLIC Threads::Threads)
endif()

target_compile_features(plugincore_shared PUBLIC cxx_std_17)

# Linked into a plugin MODULE, so it has to be position independent, and it
# must not export anything of its own: the plugins hand out one symbol.
set_target_properties(plugincore_shared PROPERTIES
   POSITION_INDEPENDENT_CODE ON
   CXX_VISIBILITY_PRESET hidden
   VISIBILITY_INLINES_HIDDEN ON
)

target_compile_options(plugincore_shared PRIVATE -Wall -Wextra)
