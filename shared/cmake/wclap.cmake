# Helpers for a plugin's WCLAP (WebCLAP) build: CLAP compiled to WebAssembly
# with wasi-sdk, for browser hosts such as openDAW. See shared/web/README.md.
# Include from a plugin's CMakeLists.txt when CMAKE_SYSTEM_NAME is WASI.
#
#   wclap_module(<target>)            link options of module.wasm (the plugin)
#   wclap_ui(<target>)                compile/link options and Cairo of ui.wasm
#   wclap_bundle(NAME <name> MODULE <target> UI <target> PAGE <dir>
#                [FONTS <files...>] [FILES <files...>] [LICENSE <file>])
#                                     <build>/<name>.wclap/ and its .tar.gz
#
# The editor's Cairo, pixman and FreeType come from WCLAP_CAIRO (default
# <repo>/wasmbuild/cairo-wasi, built by shared/cmake/build-wasm-cairo.sh).

get_filename_component(_wclap_shared "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_wclap_repo "${_wclap_shared}/.." ABSOLUTE)
set(WCLAP_CAIRO "${_wclap_repo}/wasmbuild/cairo-wasi"
    CACHE PATH "Cairo, pixman and FreeType for wasm32-wasip1 (shared/cmake/build-wasm-cairo.sh)")
set(WCLAP_HAVE_CAIRO OFF)
if (EXISTS "${WCLAP_CAIRO}/lib/libcairo.a")
    set(WCLAP_HAVE_CAIRO ON)
endif()

# module.wasm: what WCLAP hosts expect -- clap_entry, a growable function table
# the host adds its callbacks to, malloc, memory up to 2 GB; WASI imports only.
function(wclap_module target)
    set_target_properties(${target} PROPERTIES SUFFIX ".wasm")
    target_link_options(${target} PRIVATE
        -mexec-model=reactor
        "-Wl,--max-memory=2147483648,--export-table,--growable-table,--export=malloc,--export=clap_entry"
        # wasm's default stack is 64 KB, and an overflow silently writes into
        # the data below it (Shapeau's engine alone puts a 21 KB CurveSet on
        # it every block).
        "-Wl,-z,stack-size=1048576"
        # The sysroot's libraries carry DWARF; a host downloads this file.
        "-Wl,--strip-debug")
endfunction()

# ui.wasm: the plugin's Cairo editor for the page (shared/web/wclap-ui.js).
function(wclap_ui target)
    if (NOT WCLAP_HAVE_CAIRO)
        message(FATAL_ERROR "wclap_ui(${target}): no wasm Cairo at WCLAP_CAIRO=${WCLAP_CAIRO}; "
                            "run shared/cmake/build-wasm-cairo.sh <wasi-sdk> ${WCLAP_CAIRO}")
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "ui" SUFFIX ".wasm")
    target_include_directories(${target} PRIVATE "${WCLAP_CAIRO}/include" "${WCLAP_CAIRO}/include/freetype2")
    target_link_libraries(${target} PRIVATE
        "${WCLAP_CAIRO}/lib/libcairo.a" "${WCLAP_CAIRO}/lib/libpixman-1.a" "${WCLAP_CAIRO}/lib/libfreetype.a")
    # FreeType's headers need setjmp, which wasm only has through exception
    # handling; every current browser runs it.
    target_compile_options(${target} PRIVATE -mexception-handling -mllvm -wasm-enable-sjlj)
    target_link_options(${target} PRIVATE
        -mexec-model=reactor -mexception-handling
        # wasm's default stack is 64 KB, and an overflow silently writes into
        # the data below it; Cairo and a widget tree want more.
        "-Wl,-z,stack-size=1048576"
        -lsetjmp -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks
        "-Wl,--strip-debug")
endfunction()

function(wclap_bundle)
    cmake_parse_arguments(WB "" "NAME;MODULE;UI;PAGE;LICENSE" "FONTS;FILES" ${ARGN})
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(bundle "${CMAKE_BINARY_DIR}/${WB_NAME}.wclap")
    if (NOT WB_LICENSE)
        set(WB_LICENSE "${_wclap_repo}/LICENSE")
    endif()
    set(commands
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${bundle}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle}/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${WB_MODULE}>" "${bundle}/module.wasm"
        COMMAND ${CMAKE_COMMAND} -E copy "${WB_PAGE}/index.html" "${_wclap_shared}/web/wclap-ui.js"
                "${_wclap_shared}/web/wasi.js" "${bundle}/"
        COMMAND ${CMAKE_COMMAND} -E copy "${WB_LICENSE}" "${bundle}/LICENSE")
    set(deps ${WB_MODULE} "${WB_PAGE}/index.html" "${_wclap_shared}/web/wclap-ui.js" "${_wclap_shared}/web/wasi.js")
    if (WB_UI)
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${WB_UI}>" "${bundle}/ui.wasm")
        list(APPEND deps ${WB_UI})
    endif()
    if (WB_FONTS)
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy ${WB_FONTS} "${bundle}/fonts/")
        list(APPEND deps ${WB_FONTS})
    endif()
    if (WB_FILES)
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy ${WB_FILES} "${bundle}/")
        list(APPEND deps ${WB_FILES})
    endif()
    add_custom_target(${WB_NAME}-wclap-bundle ALL
        ${commands}
        COMMAND ${Python3_EXECUTABLE} "${_wclap_shared}/tools/pack-wclap.py" "${bundle}"
        DEPENDS ${deps}
        COMMENT "Packing ${WB_NAME}.wclap.tar.gz"
        VERBATIM)
endfunction()
