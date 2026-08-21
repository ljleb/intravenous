include_guard(GLOBAL)

include(${IV_SOURCE_DIR}/module/template/JuceSupport.cmake)
include(${IV_SOURCE_DIR}/module/template/SourceSpanRewrite.cmake)
include(${IV_SOURCE_DIR}/module/template/ModuleProjectInit.cmake)

function(iv_configure_iv_module_shared_import)
    set(IV_MODULE_SHARED_LIBRARY "${IV_MODULE_SHARED_LIBRARY}" CACHE FILEPATH "Path to the built iv_module_shared library")
    if(NOT IV_MODULE_SHARED_LIBRARY OR NOT EXISTS "${IV_MODULE_SHARED_LIBRARY}")
        return()
    endif()
    if(NOT TARGET iv_module_shared)
        set(_iv_links "")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
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
    if(NOT IVM_SOURCES)
        message(FATAL_ERROR "iv_add_runtime_module(${target}) requires SOURCES")
    endif()

    iv_configure_iv_module_shared_import()
    add_library(${target}__compile_settings INTERFACE)
    target_compile_features(${target}__compile_settings INTERFACE cxx_std_23)
    target_include_directories(${target}__compile_settings INTERFACE
        ${IV_INCLUDE_DIR}
        ${IV_MODULE_SOURCE_DIR}
        ${IV_MODULE_GENERATED_INCLUDE_DIR})
    target_include_directories(${target}__compile_settings SYSTEM INTERFACE ${IV_THIRD_PARTY_INCLUDE_DIR})
    if(MSVC)
        target_compile_options(${target}__compile_settings INTERFACE /W4 /permissive-)
    else()
        target_compile_options(${target}__compile_settings INTERFACE -Wall -Wextra -Wpedantic)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target}__compile_settings INTERFACE -Wno-unused-comparison -Wno-c2y-extensions)
        endif()
    endif()

    if(IVM_ENABLE_JUCE AND DEFINED IV_CORE_ENABLE_JUCE_VST AND IV_CORE_ENABLE_JUCE_VST)
        target_compile_definitions(${target}__compile_settings INTERFACE IV_ENABLE_JUCE_VST=1 JUCE_PLUGINHOST_VST3=1)
        if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
            target_include_directories(${target}__compile_settings SYSTEM INTERFACE ${IV_JUCE_MODULES_DIR})
        endif()
    else()
        target_compile_definitions(${target}__compile_settings INTERFACE IV_ENABLE_JUCE_VST=0)
    endif()

    # Phase 2 compiles the generated root export TU. It includes the rewritten
    # root definition; dependencies are recursively included through generated
    # extensionless <iv/modules/...> definitions.
    add_library(${target} SHARED ${IV_MODULE_EXPORT_FILE})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
        OUTPUT_NAME ${IV_MODULE_OUTPUT_NAME}
        RUNTIME_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR})
    target_link_libraries(${target} PRIVATE ${target}__compile_settings)
    if(TARGET iv_module_shared)
        target_link_libraries(${target} PRIVATE iv_module_shared)
    endif()
    if(DEFINED IV_MODULE_PCH_HEADER AND NOT IV_MODULE_PCH_HEADER STREQUAL "")
        target_precompile_headers(${target} PRIVATE ${IV_MODULE_PCH_HEADER})
    endif()
endfunction()
