include_guard(GLOBAL)

include(${IV_SOURCE_DIR}/module/template/JuceSupport.cmake)
include(${IV_SOURCE_DIR}/module/template/ModuleProjectInit.cmake)

option(IV_PACKAGE_SOURCE_INTROSPECTION
    "Collect configured IV module source identity metadata" ON)

set(IV_PACKAGE_FINALIZER_TIMINGS_FILE "" CACHE FILEPATH
    "Optional path for iv-package-finalize stage timings")

option(IV_PACKAGE_CLANG_TIME_TRACE
    "Write Clang frontend time-trace JSON for each IV package compilation" OFF)

function(iv_configure_builder_import)
    set(IV_BUILDER_LIBRARY "${IV_BUILDER_LIBRARY}" CACHE FILEPATH
        "Path to the built iv_builder library")
    if(NOT IV_BUILDER_LIBRARY OR NOT EXISTS "${IV_BUILDER_LIBRARY}")
        return()
    endif()
    if(NOT TARGET iv_builder)
        set(_iv_builder_links "")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            find_library(_iv_stdcxxexp_library NAMES stdc++exp
                HINTS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} REQUIRED)
            list(APPEND _iv_builder_links "${_iv_stdcxxexp_library}")
        endif()
        add_library(iv_builder SHARED IMPORTED GLOBAL)
        set_target_properties(iv_builder PROPERTIES
            IMPORTED_LOCATION "${IV_BUILDER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_iv_builder_links}")
    endif()
endfunction()

function(iv_add_package target)
    set(options ENABLE_JUCE)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(IVP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 23)
        message(FATAL_ERROR
            "IV packages require Clang 23 or newer; configured compiler is "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
    endif()
    if(NOT DEFINED IV_PACKAGE_FINALIZER OR IV_PACKAGE_FINALIZER STREQUAL ""
       OR NOT EXISTS "${IV_PACKAGE_FINALIZER}")
        message(FATAL_ERROR "iv_add_package(${target}) requires IV_PACKAGE_FINALIZER")
    endif()
    if(NOT DEFINED IV_CLANG_SOURCE_INTROSPECTION_PLUGIN
       OR IV_CLANG_SOURCE_INTROSPECTION_PLUGIN STREQUAL ""
       OR NOT EXISTS "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")
        message(FATAL_ERROR
            "iv_add_package(${target}) requires IV_CLANG_SOURCE_INTROSPECTION_PLUGIN")
    endif()

    iv_configure_builder_import()

    set(_iv_metadata_dir "${CMAKE_CURRENT_BINARY_DIR}/iv-package-metadata")
    file(MAKE_DIRECTORY "${_iv_metadata_dir}")

    set(_iv_package_sources
        ${IV_PACKAGE_SOURCE_FILES}
        ${IVP_SOURCES})
    list(REMOVE_DUPLICATES _iv_package_sources)

    # IV packages are LLVM inputs, not native shared libraries.  Compile each
    # translation unit to full-LTO LLVM bitcode at O0, then combine/prune those
    # objects into one .ivpkg.bc consumed directly by the host's shared ORC JIT.
    set(_iv_objects_target "${target}__objects")
    add_library(${_iv_objects_target} OBJECT ${_iv_package_sources})
    set_target_properties(${_iv_objects_target} PROPERTIES
        CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    target_compile_features(${_iv_objects_target} PRIVATE cxx_std_26)
    target_compile_options(${_iv_objects_target} PRIVATE -O0 -flto=full)
    target_compile_definitions(${_iv_objects_target} PRIVATE
        "IV_PACKAGE_ROOT=\"${IV_PACKAGE_DIR}\"")

    if(IV_PACKAGE_CLANG_TIME_TRACE)
        target_compile_options(${_iv_objects_target} PRIVATE -ftime-trace)
    endif()

    if(IV_PACKAGE_SOURCE_INTROSPECTION)
        set(_iv_source_introspection 1)
    else()
        set(_iv_source_introspection 0)
    endif()
    target_compile_options(${_iv_objects_target} PRIVATE
        "-fplugin=${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}"
        "-fplugin-arg-iv_module_metadata-core-source-dir=${IV_SOURCE_DIR}"
        "-fplugin-arg-iv_module_metadata-metadata-dir=${_iv_metadata_dir}"
        "-fplugin-arg-iv_module_metadata-source-introspection=${_iv_source_introspection}"
        -Wall -Wextra -Wpedantic)

    target_include_directories(${_iv_objects_target} PRIVATE
        ${IV_INCLUDE_DIR}
        ${IV_PACKAGE_DIR})
    if(DEFINED IV_PACKAGE_INCLUDE_DIRS AND NOT IV_PACKAGE_INCLUDE_DIRS STREQUAL "")
        target_include_directories(${_iv_objects_target} PRIVATE ${IV_PACKAGE_INCLUDE_DIRS})
    endif()
    target_include_directories(${_iv_objects_target} SYSTEM PRIVATE
        ${IV_THIRD_PARTY_INCLUDE_DIR})

    if(IVP_ENABLE_JUCE AND DEFINED IV_CORE_ENABLE_JUCE_VST AND IV_CORE_ENABLE_JUCE_VST)
        target_compile_definitions(${_iv_objects_target} PRIVATE
            IV_ENABLE_JUCE_VST=1 JUCE_PLUGINHOST_VST3=1)
        if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
            target_include_directories(${_iv_objects_target} SYSTEM PRIVATE
                ${IV_JUCE_MODULES_DIR})
        endif()
    else()
        target_compile_definitions(${_iv_objects_target} PRIVATE IV_ENABLE_JUCE_VST=0)
    endif()

    # Linking iv_builder as a usage requirement is useful for custom package
    # CMake projects (include/link properties), but no native link is performed.
    if(TARGET iv_builder)
        target_link_libraries(${_iv_objects_target} PRIVATE iv_builder)
    endif()

    if(NOT DEFINED IV_PACKAGE_PCH_HEADER)
        set(IV_PACKAGE_PCH_HEADER "${IV_SOURCE_DIR}/module/template/module_pch.h")
    endif()
    if(NOT IV_PACKAGE_PCH_HEADER STREQUAL "")
        target_precompile_headers(${_iv_objects_target} PRIVATE "${IV_PACKAGE_PCH_HEADER}")
    endif()

    set_property(SOURCE ${_iv_package_sources} APPEND PROPERTY OBJECT_DEPENDS
        "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")

    set(_iv_package_finalizer_timings_file "${IV_PACKAGE_FINALIZER_TIMINGS_FILE}")
    if(NOT _iv_package_finalizer_timings_file)
        set(_iv_package_finalizer_timings_file
            "${CMAKE_CURRENT_BINARY_DIR}/iv-package-finalizer-timings.txt")
    endif()

    set(_iv_package_output
        "${IV_PACKAGE_OUTPUT_DIR}/${IV_PACKAGE_OUTPUT_NAME}.ivpkg.bc")
    add_custom_command(
        OUTPUT "${_iv_package_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${IV_PACKAGE_OUTPUT_DIR}"
        COMMAND "${IV_PACKAGE_FINALIZER}"
            "--metadata-dir=${_iv_metadata_dir}"
            "--timings-file=${_iv_package_finalizer_timings_file}"
            "--output=${_iv_package_output}"
            -- $<TARGET_OBJECTS:${_iv_objects_target}>
        DEPENDS
            ${_iv_objects_target}
            "${IV_PACKAGE_FINALIZER}"
            "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Finalizing IV package LLVM ${IV_PACKAGE_OUTPUT_NAME}.ivpkg.bc")
    add_custom_target(${target} ALL DEPENDS "${_iv_package_output}")
endfunction()
