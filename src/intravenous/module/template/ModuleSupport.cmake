include_guard(GLOBAL)

include(${IV_SOURCE_DIR}/module/template/JuceSupport.cmake)
include(${IV_SOURCE_DIR}/module/template/ModuleProjectInit.cmake)

option(IV_MODULE_SOURCE_INTROSPECTION
    "Collect authored IV module source identity metadata" ON)

set(IV_MODULE_FINALIZER_OPTIMIZATION "O3" CACHE STRING
    "Optimization level used by iv-module-finalize for the final native module")
set_property(CACHE IV_MODULE_FINALIZER_OPTIMIZATION PROPERTY STRINGS O0 O3)

set(IV_MODULE_FINALIZER_TIMINGS_FILE "" CACHE FILEPATH
    "Optional path for iv-module-finalize stage timings")

option(IV_MODULE_CLANG_TIME_TRACE
    "Write Clang frontend time-trace JSON for each IV module compilation" OFF)

function(iv_configure_iv_module_shared_import)
    set(IV_MODULE_SHARED_LIBRARY "${IV_MODULE_SHARED_LIBRARY}" CACHE FILEPATH
        "Path to the built iv_module_shared library")
    if(NOT IV_MODULE_SHARED_LIBRARY OR NOT EXISTS "${IV_MODULE_SHARED_LIBRARY}")
        return()
    endif()
    if(NOT TARGET iv_module_shared)
        set(_iv_links "")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            find_library(_iv_module_stdcxxexp_library NAMES stdc++exp
                HINTS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} REQUIRED)
            list(APPEND _iv_links "${_iv_module_stdcxxexp_library}")
        endif()
        add_library(iv_module_shared SHARED IMPORTED GLOBAL)
        set_target_properties(iv_module_shared PROPERTIES
            IMPORTED_LOCATION "${IV_MODULE_SHARED_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_iv_links}")
    endif()
endfunction()

function(iv_add_runtime_module target)
    set(options ENABLE_JUCE)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(IVM "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT DEFINED IV_MODULE_EXPORT_FILE OR IV_MODULE_EXPORT_FILE STREQUAL "")
        message(FATAL_ERROR "iv_add_runtime_module(${target}) requires IV_MODULE_EXPORT_FILE")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 23)
        message(FATAL_ERROR
            "IV modules require Clang 23 or newer; configured compiler is "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
    endif()
    if(NOT DEFINED IV_MODULE_FINALIZER OR IV_MODULE_FINALIZER STREQUAL ""
       OR NOT EXISTS "${IV_MODULE_FINALIZER}")
        message(FATAL_ERROR "iv_add_runtime_module(${target}) requires IV_MODULE_FINALIZER")
    endif()
    if(NOT DEFINED IV_CLANG_SOURCE_INTROSPECTION_PLUGIN
       OR IV_CLANG_SOURCE_INTROSPECTION_PLUGIN STREQUAL ""
       OR NOT EXISTS "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")
        message(FATAL_ERROR
            "iv_add_runtime_module(${target}) requires IV_CLANG_SOURCE_INTROSPECTION_PLUGIN for State metadata")
    endif()

    iv_configure_iv_module_shared_import()

    set(_iv_metadata_dir "${CMAKE_CURRENT_BINARY_DIR}/iv-module-metadata")
    file(MAKE_DIRECTORY "${_iv_metadata_dir}")

    add_library(${target}__compile_settings INTERFACE)
    target_compile_features(${target}__compile_settings INTERFACE cxx_std_26)
    # Every source that contributes code to the runtime-module target remains
    # LLVM bitcode until the target link step. iv_module_finalize consumes
    # these objects, executes the graph-building slice through ORC, injects the
    # authored graph/configuration tables, emits one native replacement object,
    # and then resumes CMake's original link command.
    target_compile_options(${target}__compile_settings INTERFACE -O0 -flto=full)
    target_link_options(${target}__compile_settings INTERFACE -flto=full -fuse-ld=lld)

    if(IV_MODULE_CLANG_TIME_TRACE)
        target_compile_options(${target}__compile_settings INTERFACE -ftime-trace)
    endif()

    if(IV_MODULE_SOURCE_INTROSPECTION)
        set(_iv_source_introspection 1)
    else()
        set(_iv_source_introspection 0)
    endif()
    target_compile_options(${target}__compile_settings INTERFACE
        "-fplugin=${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}"
        "-fplugin-arg-iv_module_metadata-core-source-dir=${IV_SOURCE_DIR}"
        "-fplugin-arg-iv_module_metadata-metadata-dir=${_iv_metadata_dir}"
        "-fplugin-arg-iv_module_metadata-source-introspection=${_iv_source_introspection}")

    target_include_directories(${target}__compile_settings INTERFACE
        ${IV_INCLUDE_DIR}
        ${IV_MODULE_SOURCE_DIR}
        ${IV_MODULE_GENERATED_INCLUDE_DIR})
    if(DEFINED IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR
       AND NOT IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR STREQUAL "")
        target_include_directories(${target}__compile_settings INTERFACE
            ${IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR})
    endif()
    if(DEFINED IV_MODULE_INCLUDE_DIRS AND NOT IV_MODULE_INCLUDE_DIRS STREQUAL "")
        target_include_directories(${target}__compile_settings INTERFACE ${IV_MODULE_INCLUDE_DIRS})
    endif()
    target_include_directories(${target}__compile_settings SYSTEM INTERFACE
        ${IV_THIRD_PARTY_INCLUDE_DIR})
    target_compile_options(${target}__compile_settings INTERFACE
        -Wall -Wextra -Wpedantic)

    if(IVM_ENABLE_JUCE AND DEFINED IV_CORE_ENABLE_JUCE_VST AND IV_CORE_ENABLE_JUCE_VST)
        target_compile_definitions(${target}__compile_settings INTERFACE
            IV_ENABLE_JUCE_VST=1 JUCE_PLUGINHOST_VST3=1)
        if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
            target_include_directories(${target}__compile_settings SYSTEM INTERFACE
                ${IV_JUCE_MODULES_DIR})
        endif()
    else()
        target_compile_definitions(${target}__compile_settings INTERFACE IV_ENABLE_JUCE_VST=0)
    endif()

    set(_iv_module_sources ${IV_MODULE_EXPORT_FILE} ${IVM_SOURCES})
    add_library(${target} SHARED ${_iv_module_sources})
    # The metadata plugin runs during each module-source compilation but is
    # loaded only through a compiler flag. CMake otherwise cannot know that a
    # rebuilt plugin invalidates existing LLVM bitcode and its metadata JSON.
    # Make it an explicit object dependency so a host rebuild recompiles
    # persistent module workspaces instead of relinking stale metadata.
    set_property(SOURCE ${_iv_module_sources} APPEND PROPERTY OBJECT_DEPENDS
        "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")
    # The finalizer transforms the bitcode at link time. Its executable is not
    # a normal linker input, so make updates to it invalidate the link result.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${IV_MODULE_FINALIZER}")
    set(_iv_module_finalizer_launcher
        "${IV_MODULE_FINALIZER}"
        "--metadata-dir=${_iv_metadata_dir}"
        "--optimization=${IV_MODULE_FINALIZER_OPTIMIZATION}")
    set(_iv_module_finalizer_timings_file "${IV_MODULE_FINALIZER_TIMINGS_FILE}")
    if(NOT _iv_module_finalizer_timings_file)
        set(_iv_module_finalizer_timings_file
            "${CMAKE_CURRENT_BINARY_DIR}/iv-module-finalizer-timings.txt")
    endif()
    list(APPEND _iv_module_finalizer_launcher
        "--timings-file=${_iv_module_finalizer_timings_file}")
    list(APPEND _iv_module_finalizer_launcher "--")

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
        OUTPUT_NAME ${IV_MODULE_OUTPUT_NAME}
        RUNTIME_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
        CXX_LINKER_LAUNCHER "${_iv_module_finalizer_launcher}")
    target_link_libraries(${target} PRIVATE ${target}__compile_settings)

    if(TARGET iv_module_shared)
        target_link_libraries(${target} PRIVATE iv_module_shared)
    endif()

    if(NOT DEFINED IV_MODULE_PCH_HEADER)
        set(IV_MODULE_PCH_HEADER "${IV_SOURCE_DIR}/module/template/module_pch.h")
    endif()
    if(NOT IV_MODULE_PCH_HEADER STREQUAL "")
        target_precompile_headers(${target} PRIVATE "${IV_MODULE_PCH_HEADER}")
    endif()
endfunction()
