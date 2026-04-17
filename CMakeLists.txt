cmake_minimum_required(VERSION 3.20)

project(intravenous-dsp LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

set(JUCE_DIR "" CACHE PATH "Path to a JUCE source tree or installed JUCE package root")
option(IV_BUILD_CLANG_SOURCE_SPAN_TOOL "Build the Clang node source span rewriter (set to OFF to skip it)" ON)

# avoid pulling heavy GUI/web dependencies
# unless explicitly re-enabled by the user at configure time.
set(JUCE_WEB_BROWSER 0 CACHE BOOL "Enable JUCE web browser component" FORCE)
set(JUCE_USE_CURL 0 CACHE BOOL "Enable JUCE curl integration" FORCE)

set(IV_JUCE_SOURCE_DIR "")
if(JUCE_DIR)
    if(EXISTS "${JUCE_DIR}/CMakeLists.txt")
        set(IV_JUCE_SOURCE_DIR "${JUCE_DIR}")
    elseif(EXISTS "${JUCE_DIR}/JUCEConfig.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${JUCE_DIR}")
    elseif(EXISTS "${JUCE_DIR}/lib/cmake/JUCE/JUCEConfig.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${JUCE_DIR}/lib/cmake/JUCE")
    endif()
endif()

if(IV_JUCE_SOURCE_DIR)
    add_subdirectory("${IV_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/juce" EXCLUDE_FROM_ALL)
    set(JUCE_FOUND TRUE)
else()
    find_package(JUCE CONFIG QUIET)
endif()

add_subdirectory(intravenous)
add_subdirectory(tests)
add_subdirectory(benchmark)

if(IV_BUILD_CLANG_SOURCE_SPAN_TOOL)
    add_subdirectory(tools)
    target_compile_definitions(intravenous_module_runtime PUBLIC
        IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER="$<TARGET_FILE:iv_clang_source_span_rewriter>"
    )
else()
    target_compile_definitions(intravenous_module_runtime PUBLIC
        IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER=""
    )
endif()
