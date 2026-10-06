cmake_minimum_required(VERSION 3.21)
project(Aurum VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(AURUM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CLAP" CACHE PATH "Directory holding clap, clap-wrapper and vst3sdk")
option(AURUM_BUILD_PLUGIN "Build the CLAP/VST3 plugin" ON)
option(AURUM_BUILD_TESTS "Build DSP tests" ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# x86-64-v3 (AVX2/FMA) keeps the attenuation banks vectorised.
set(AURUM_ARCH_FLAGS "-march=x86-64-v3" CACHE STRING "Target architecture flags")
add_compile_options(${AURUM_ARCH_FLAGS} -fno-math-errno)

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

if (AURUM_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if (AURUM_BUILD_PLUGIN)
    include(cmake/plugin.cmake)
endif()
