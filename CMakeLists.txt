cmake_minimum_required(VERSION 3.20)

project(intravenous-dsp LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR
        "Intravenous Phase 4 requires GCC 16 or newer for C++26 reflection; "
        "configured compiler is ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "(${CMAKE_CXX_COMPILER})")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-freflection>)

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

add_subdirectory(src/intravenous)
add_subdirectory(tests)
add_subdirectory(benchmark)

function(iv_force_cxx26 directory)
    get_property(_iv_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_iv_target IN LISTS _iv_targets)
        set_target_properties(${_iv_target} PROPERTIES
            CXX_STANDARD 26
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
    endforeach()

    get_property(_iv_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_iv_subdirectory IN LISTS _iv_subdirectories)
        iv_force_cxx26("${_iv_subdirectory}")
    endforeach()
endfunction()
iv_force_cxx26("${CMAKE_SOURCE_DIR}")

if(IV_BUILD_CLANG_SOURCE_SPAN_TOOL)
    include(ExternalProject)

    set(IV_CLANG_CXX "$ENV{IV_CLANG_CXX}" CACHE FILEPATH
        "Clang++ executable used only to build the source span rewriter")
    if(NOT IV_CLANG_CXX)
        find_program(IV_CLANG_CXX NAMES clang++ REQUIRED)
    endif()

    set(_iv_source_span_rewriter_build_dir "${CMAKE_BINARY_DIR}/tools-clang")
    set(_iv_source_span_rewriter_executable
        "${_iv_source_span_rewriter_build_dir}/iv_clang_source_span_rewriter${CMAKE_EXECUTABLE_SUFFIX}")

    ExternalProject_Add(iv_clang_source_span_rewriter_build
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/tools"
        BINARY_DIR "${_iv_source_span_rewriter_build_dir}"
        CMAKE_ARGS
            "-DCMAKE_CXX_COMPILER=${IV_CLANG_CXX}"
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        BUILD_BYPRODUCTS "${_iv_source_span_rewriter_executable}"
        INSTALL_COMMAND "")

    add_executable(iv_clang_source_span_rewriter IMPORTED GLOBAL)
    set_target_properties(iv_clang_source_span_rewriter PROPERTIES
        IMPORTED_LOCATION "${_iv_source_span_rewriter_executable}")
    add_dependencies(iv_clang_source_span_rewriter iv_clang_source_span_rewriter_build)

    target_compile_definitions(intravenous_module_loader PRIVATE
        IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER="$<TARGET_FILE:iv_clang_source_span_rewriter>"
    )
else()
    target_compile_definitions(intravenous_module_loader PRIVATE
        IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER=""
    )
endif()
