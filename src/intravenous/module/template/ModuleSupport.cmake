include_guard(GLOBAL)

include(${IV_SOURCE_DIR}/module/template/JuceSupport.cmake)
include(${IV_SOURCE_DIR}/module/template/ModuleProjectInit.cmake)

option(IV_MODULE_SOURCE_INTROSPECTION
    "Collect authored IV module source/state metadata" ON)

function(iv_configure_iv_module_shared_import)
    set(IV_MODULE_SHARED_LIBRARY "${IV_MODULE_SHARED_LIBRARY}" CACHE FILEPATH "Path to the built iv_module_shared library")
    if(NOT IV_MODULE_SHARED_LIBRARY OR NOT EXISTS "${IV_MODULE_SHARED_LIBRARY}")
        return()
    endif()
    if(NOT TARGET iv_module_shared)
        set(_iv_links "")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            list(APPEND _iv_links stdc++exp)
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
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
        message(FATAL_ERROR
            "IV modules require Clang 20 or newer; configured compiler is "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
    endif()
    if(NOT DEFINED IV_MODULE_FINALIZER OR IV_MODULE_FINALIZER STREQUAL ""
       OR NOT EXISTS "${IV_MODULE_FINALIZER}")
        message(FATAL_ERROR
            "iv_add_runtime_module(${target}) requires a built IV_MODULE_FINALIZER")
    endif()
    if(IV_MODULE_SOURCE_INTROSPECTION
       AND (NOT DEFINED IV_CLANG_SOURCE_INTROSPECTION_PLUGIN
            OR IV_CLANG_SOURCE_INTROSPECTION_PLUGIN STREQUAL ""
            OR NOT EXISTS "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}"))
        message(FATAL_ERROR
            "iv_add_runtime_module(${target}) requires a built "
            "IV_CLANG_SOURCE_INTROSPECTION_PLUGIN")
    endif()

    iv_configure_iv_module_shared_import()

    set(_iv_metadata_dir "${CMAKE_CURRENT_BINARY_DIR}/iv-module-metadata")
    file(MAKE_DIRECTORY "${_iv_metadata_dir}")

    add_library(${target}__compile_settings INTERFACE)
    target_compile_features(${target}__compile_settings INTERFACE cxx_std_26)
    target_compile_options(${target}__compile_settings INTERFACE
        # Preserve the complete TU as LLVM bitcode for iv_module_finalize.
        -flto=full
        # Authoring is JITed immediately. Expensive optimization belongs after
        # the graph has been authored and the execution kernel exists.
        -O0
        -g
        -Xclang -disable-O0-optnone
        -Wall -Wextra -Wpedantic)
    if(IV_MODULE_SOURCE_INTROSPECTION)
        target_compile_options(${target}__compile_settings INTERFACE
            "-fplugin=${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}"
            -Xclang -plugin-arg-iv-module-metadata
            -Xclang "core-source-dir=${IV_SOURCE_DIR}"
            -Xclang -plugin-arg-iv-module-metadata
            -Xclang "metadata-dir=${_iv_metadata_dir}")
    endif()
    target_include_directories(${target}__compile_settings INTERFACE
        ${IV_INCLUDE_DIR}
        ${IV_MODULE_SOURCE_DIR}
        ${IV_MODULE_GENERATED_INCLUDE_DIR})
    if(DEFINED IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR AND NOT IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR STREQUAL "")
        target_include_directories(${target}__compile_settings INTERFACE
            ${IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR})
    endif()
    if(DEFINED IV_MODULE_INCLUDE_DIRS AND NOT IV_MODULE_INCLUDE_DIRS STREQUAL "")
        target_include_directories(${target}__compile_settings INTERFACE ${IV_MODULE_INCLUDE_DIRS})
    endif()
    target_include_directories(${target}__compile_settings SYSTEM INTERFACE ${IV_THIRD_PARTY_INCLUDE_DIR})

    if(IVM_ENABLE_JUCE AND DEFINED IV_CORE_ENABLE_JUCE_VST AND IV_CORE_ENABLE_JUCE_VST)
        target_compile_definitions(${target}__compile_settings INTERFACE IV_ENABLE_JUCE_VST=1 JUCE_PLUGINHOST_VST3=1)
        if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
            target_include_directories(${target}__compile_settings SYSTEM INTERFACE ${IV_JUCE_MODULES_DIR})
        endif()
    else()
        target_compile_definitions(${target}__compile_settings INTERFACE IV_ENABLE_JUCE_VST=0)
    endif()

    add_library(${target} SHARED ${IV_MODULE_EXPORT_FILE} ${IVM_SOURCES})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
        INTERPROCEDURAL_OPTIMIZATION OFF
        OUTPUT_NAME ${IV_MODULE_OUTPUT_NAME}
        RUNTIME_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR})
    target_link_libraries(${target} PRIVATE ${target}__compile_settings)
    target_link_options(${target} PRIVATE -flto=full)

    # CMake still owns the complete custom link line. The launcher consumes the
    # target's LTO objects, authors the graph through ORC, embeds the authored
    # graph/config/type tables, emits one native object, then executes this
    # original link command with all custom libraries/options intact.
    set_property(TARGET ${target} PROPERTY CXX_LINKER_LAUNCHER
        "${IV_MODULE_FINALIZER};--metadata-dir=${_iv_metadata_dir};--")

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
